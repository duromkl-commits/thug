"""Red Dead Revolver (PS2, SLUS-205.00) file readers for rdr_to_thug.py.

Archives (SWAT.DAT etc.) are Rockstar San Diego "Dave" archives: header
'Dave', count, toc size, names size; 16-byte TOC entries at 0x800 (name
offset, data offset, size, compressed size, raw deflate when they differ);
names 6-bit packed LSB first, a first symbol >= 56 meaning "copy
(s1-56)+(s2-32)*8 chars of the previous name". Characters are a 3-4 letter
code: <c>.skel (text, offset-only bones), <c>_lod_01.mod (body, vertices
bone-local), <c>_face.mod and hat_<c>_lod_01.mod (both on the head bone),
<c>01.tex (8-bit, 256-entry BGRA palette, not swizzled).
.mod comes as binary 2.12, binary 2.00 (u32 fields, no reskins) or text 1.x.
"""

import re
import struct
import zlib

import numpy as np

CS = "\0 #$()-./?0123456789_abcdefghijklmnopqrstuvwxyz~"


class Dave:
    def __init__(self, path):
        self.path = path
        f = open(path, 'rb'); h = f.read(0x800)
        if h[:4] != b'Dave':
            raise ValueError('%s: not a Dave archive' % path)
        n, ts, ns = struct.unpack_from('<III', h, 4)
        f.seek(0x800); toc = f.read(ts); nb = f.read(ns)
        self.entries = {}
        prev = ''
        for i in range(n):
            no, off, size, csize = struct.unpack_from('<IIII', toc, 16 * i)
            g = []; k = 0
            while True:
                bp = k * 6; by = no + bp // 8
                v = int.from_bytes(nb[by:by + 2], 'little') >> (bp % 8) & 63
                if v == 0:
                    break
                g.append(v); k += 1
            s = ''
            if g and g[0] >= 56:
                s = prev[:(g[0] - 56) + (g[1] - 32) * 8]; g = g[2:]
            s += ''.join(CS[x] if x < len(CS) else '?' for x in g)
            self.entries[s] = (off, size, csize); prev = s

    def __contains__(self, n):
        return n in self.entries

    def get(self, n):
        off, size, csize = self.entries[n]
        f = open(self.path, 'rb'); f.seek(off); d = f.read(csize)
        return d if csize == size else zlib.decompress(d, -15)


def read_skel(txt):
    """-> names, parent index (-1 root), world joint positions (offsets summed)."""
    toks = re.findall(r'bone\s+(\S+)\s*\{|offset\s+(\S+)\s+(\S+)\s+(\S+)|(\})', txt)
    names = []; parent = []; off = []; stack = []
    for nm, x, y, z, close in toks:
        if nm:
            parent.append(stack[-1] if stack else -1); names.append(nm); off.append([0, 0, 0]); stack.append(len(names) - 1)
        elif x:
            off[stack[-1]] = [float(x), float(y), float(z)]
        elif close and stack:
            stack.pop()
    off = np.array(off, float); W = np.zeros_like(off)
    for i, p in enumerate(parent):
        W[i] = off[i] + (W[p] if p >= 0 else 0)
    return names, parent, W


def _arrays(b, o, nv, nn, nc, nt, nt2, ntg):
    P = np.frombuffer(b, np.float32, nv * 3, o).reshape(-1, 3); o += nv * 12
    N = np.frombuffer(b, np.float32, nn * 3, o).reshape(-1, 3); o += nn * 12
    o += nc * 16
    T = np.frombuffer(b, np.float32, nt * 2, o).reshape(-1, 2); o += nt * 8
    o += nt2 * 8 + ntg * 12
    return P, N, T, o


def _mat_headers(b, o, nm, extra):
    mats = []
    for _ in range(nm):
        e = b.index(b' ', o); name = b[o:e].decode('latin1'); o = e + 1
        npk, _, ntex, _ = struct.unpack_from('<4I', b, o); o += 16 + 48
        tex = []
        for _ in range(ntex):
            e = b.index(b' ', o); tex.append(b[o:e].decode('latin1')); o = e + 1
        if extra:
            o += 4
        mats.append(dict(name=name, tex=tex, npk=npk, packets=[]))
    return mats, o


def read_mod_bin(b):
    o = b.index(b'\0') + 1
    v200 = b.startswith(b'version: 2.00')
    nh = 10 if v200 else 11
    H = struct.unpack_from('<%dI' % nh, b, o); o += 4 * nh
    P, N, T, o = _arrays(b, o, *H[:6])
    mats, o = _mat_headers(b, o, H[6], not v200)
    for mt in mats:
        for _ in range(mt['npk']):
            if v200:
                nadj, nstr, nmtx, nbl = struct.unpack_from('<4I', b, o); o += 16
                adj = np.frombuffer(b, np.uint32, nadj * 6, o).reshape(-1, 6).astype(np.int64); o += nadj * 24
                bl = []
                strips = []
                for _ in range(nstr):
                    fl, cnt = struct.unpack_from('<2I', b, o); o += 8
                    strips.append((fl, list(struct.unpack_from('<%dI' % cnt, b, o)))); o += 4 * cnt
                mtx = list(struct.unpack_from('<%dI' % nmtx, b, o)); o += 4 * nmtx
            else:
                nadj, nstr, nmtx, nbl = struct.unpack_from('<4H', b, o); o += 8
                adj = np.frombuffer(b, np.uint16, nadj * 6, o).reshape(-1, 6).astype(np.int64); o += nadj * 12
                bl = [struct.unpack_from('<HH4f', b, o + 20 * i) for i in range(nbl)]; o += 20 * nbl
                strips = []
                for _ in range(nstr):
                    fl, cnt = b[o], b[o + 1]; strips.append((fl, list(b[o + 2:o + 2 + cnt]))); o += 2 + cnt
                mtx = list(b[o:o + nmtx]); o += nmtx
            mt['packets'].append(dict(adj=adj, bl=bl, strips=strips, mtx=mtx))
    return dict(P=P, N=N, T=T, mats=mats)


def read_mod_text(t):
    t = t.decode('latin1')
    P = []; N = []; T = []; mats = []; cur = None; pk = None
    for line in t.splitlines():
        s = line.split()
        if not s:
            continue
        k = s[0]
        if k == 'v': P.append([float(x) for x in s[1:4]])
        elif k == 'n': N.append([float(x) for x in s[1:4]])
        elif k == 't1': T.append([float(x) for x in s[1:3]])
        elif k == 'mtl': cur = dict(name=s[1], tex=[], packets=[]); mats.append(cur)
        elif k == 'texture:': cur['tex'].append(line.split('"')[1])
        elif k == 'packet': pk = dict(adj=[], bl=[], strips=[], mtx=[]); cur['packets'].append(pk)
        elif k == 'adj': pk['adj'].append([int(x) for x in s[1:7]])
        elif k == 'reskin': pk['bl'].append((int(s[1]), int(s[2]), float(s[3]), float(s[4]), float(s[5]), float(s[6])))
        elif k in ('str', 'stp'): pk['strips'].append((1 if k == 'str' else 2, [int(x) for x in s[2:]]))
        elif k == 'mtx': pk['mtx'] = [int(x) for x in s[1:]]
    for m in mats:
        for p in m['packets']:
            p['adj'] = np.array(p['adj'], dtype=np.int64).reshape(-1, 6)
    return dict(P=np.array(P), N=np.array(N), T=np.array(T), mats=mats)


def read_mod(b):
    return read_mod_text(b) if b.startswith(b'version: 1.') else read_mod_bin(b)


def read_tex(b):
    """-> PIL RGBA image."""
    from PIL import Image
    w, h = struct.unpack_from('<HH', b, 0)
    npal = (len(b) - 14 - w * h) // 4
    pal = np.frombuffer(b, np.uint8, npal * 4, 14).reshape(-1, 4)[:, [2, 1, 0, 3]]
    px = np.frombuffer(b, np.uint8, w * h, 14 + npal * 4).reshape(h, w)
    return Image.fromarray(np.ascontiguousarray(pal[px]), 'RGBA')


def strips_to_tris(strips, base=0):
    """Rockstar strips: flag 1 'str', 2 'stp' (opposite parity). Output CCW
    seen from outside once x and z are negated (the model turned to face +Z)."""
    out = []
    for fl, s in strips:
        for k in range(len(s) - 2):
            a, b, c = s[k], s[k + 1], s[k + 2]
            if (k + (fl == 2)) % 2:
                a, b = b, a
            if a != b and b != c and a != c:
                out.append((base + a, base + b, base + c))
    return out


def character(dave, code, hat=True):
    """-> (names, parent, joints, verts, uvs, normals, tris, tex_names).
    verts: per vertex [(bone, weight, offset from that bone's joint)]."""
    names, parent, J = read_skel(dave.get(code + '.skel').decode('latin1'))
    head = names.index('head')
    VB = []; UV = []; NR = []; TRI = []; TEX = []; hatv = set()
    parts = [(code + '_lod_01.mod', False), (code + '_face.mod', True)]
    if hat:
        parts.append(('hat_%s_lod_01.mod' % code, True))
    for f, on_head in parts:
        if f not in dave:
            continue
        m = read_mod(dave.get(f))
        for mt in m['mats']:
            for pk in mt['packets']:
                base = len(VB)
                if f.startswith('hat_'):
                    hatv |= set(range(base, base + len(pk['adj'])))
                for pos, nrm, _, uv, _, lb in pk['adj']:
                    bi = head if on_head else pk['mtx'][lb]
                    VB.append([(bi, 1.0, m['P'][pos].astype(float))]); UV.append(m['T'][uv]); NR.append(m['N'][nrm])
                if not on_head:
                    for ai, lb, w, ox, oy, oz in pk['bl']:
                        i = base + ai; b0, _, l0 = VB[i][0]
                        VB[i] = [(b0, 1 - w, l0), (pk['mtx'][lb], w, np.array([ox, oy, oz]))]
                t = strips_to_tris(pk['strips'], base)
                TRI += t; TEX += [mt['tex'][0] if mt['tex'] else None] * len(t)
    _reattach(names, J, VB, TRI, hatv)
    return names, parent, J, VB, np.array(UV, float), np.array(NR, float), TRI, TEX


def _reattach(names, J, VB, TRI, hat):
    """Pieces of the 'hat' model that sit below the neck or off to one side
    (a hat hung on the back, a shoulder strap) follow the body bone under them
    instead of the head."""
    if not hat:
        return
    from collections import Counter
    from scipy.spatial import cKDTree
    world = lambda i: sum(w * (J[b] + l) for b, w, l in VB[i])
    body = [i for i in range(len(VB)) if i not in hat]
    tree = cKDTree(np.array([world(i) for i in body]))
    bdom = [max(VB[i], key=lambda x: x[1])[0] for i in body]
    par = {i: i for i in hat}

    def find(x):
        while par[x] != x:
            par[x] = par[par[x]]; x = par[x]
        return x
    for t in TRI:
        if all(v in hat for v in t):
            a = find(t[0])
            for v in t[1:]:
                par[find(v)] = a
    comps = {}
    for v in hat:
        comps.setdefault(find(v), []).append(v)
    neck = J[names.index('neck_01')][1]
    for c in comps.values():
        P = np.array([world(v) for v in c])
        if P[:, 1].min() > neck - 0.05 and abs(P[:, 0].mean()) < 0.15:
            continue
        _, j = tree.query(P)
        b = Counter(bdom[k] for k in j).most_common(1)[0][0]
        for v, p in zip(c, P):
            VB[v] = [(b, 1.0, p - J[b])]
