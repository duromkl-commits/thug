"""Source engine model reader (Garry's Mod player models): .mdl (v44-49) +
.vvd + .dx90.vtx, and .vmt/.vtf materials.

read_model(mdl_path) -> dict(bones=[(name, parent, world_joint)], P, N, UV,
                             W=[[(bone, weight)...]], tris=[(material, (a, b, c))],
                             materials=[name], cddirs=[dir])
Only LOD 0, the first model of every body part that has meshes, skin family 0.
"""
import os
import struct

import numpy as np


def cstr(d, o):
    e = d.index(0, o)
    return d[o:e].decode('latin-1')


def read_mdl(d):
    if d[:4] != b'IDST':
        raise ValueError('not a Source mdl')
    ver = struct.unpack_from('<i', d, 4)[0]
    nb, ob = struct.unpack_from('<2i', d, 156)
    ntex, otex, ncd, ocd, nsr, nsf, osr, nbp, obp = struct.unpack_from('<9i', d, 204)
    bones = []
    for i in range(nb):
        o = ob + i * 216
        nm = cstr(d, o + struct.unpack_from('<i', d, o)[0])
        par = struct.unpack_from('<i', d, o + 4)[0]
        m = np.array(struct.unpack_from("<12f", d, o + 96)).reshape(3, 4)	# poseToBone
        R, t = m[:, :3], m[:, 3]
        bones.append((nm, par, -R.T @ t))
    mats = []
    for i in range(ntex):
        o = otex + i * 64
        mats.append(cstr(d, o + struct.unpack_from('<i', d, o)[0]))
    cds = [cstr(d, struct.unpack_from('<i', d, ocd + 4 * i)[0]) for i in range(ncd)]
    skin = list(struct.unpack_from('<%dh' % nsr, d, osr)) if nsr else list(range(ntex))
    parts = []	# per body part: list of models; model = (vertexindex, [(material, vertexoffset, nverts)])
    for b in range(nbp):
        o = obp + b * 16
        nmod, base, mi = struct.unpack_from('<3i', d, o + 4)
        models = []
        for k in range(nmod):
            mo = o + mi + k * 148
            nmesh, meshi, nv, vi = struct.unpack_from('<4i', d, mo + 72)
            meshes = []
            for q in range(nmesh):
                so = mo + meshi + q * 116
                mat, _, mnv, voff = struct.unpack_from('<4i', d, so)
                meshes.append((skin[mat] if mat < len(skin) else mat, voff, mnv))
            models.append((vi // 48, meshes))
        parts.append(models)
    return ver, bones, mats, cds, parts


def read_vvd(d):
    if d[:4] != b'IDSV':
        raise ValueError('not a vvd')
    nlod = struct.unpack_from('<i', d, 12)[0]
    nl = struct.unpack_from('<8i', d, 16)
    nfix, ofix, overt, otan = struct.unpack_from('<4i', d, 48)
    n0 = nl[0]
    raw = np.frombuffer(d, dtype=np.dtype([('w', '<f4', 3), ('b', 'u1', 3), ('nb', 'u1'), ('p', '<f4', 3),
                                           ('n', '<f4', 3), ('uv', '<f4', 2)]), count=(otan - overt) // 48 if otan > overt else n0,
                        offset=overt)
    if nfix:
        idx = []
        for i in range(nfix):
            lod, src, num = struct.unpack_from('<3i', d, ofix + 12 * i)
            if lod >= 0:
                idx.extend(range(src, src + num))
        raw = raw[np.array(idx)]
    return raw[:n0]


def read_vtx(d, parts):
    """Triangles [(part, model, mesh, (i, j, k) mesh-local vertex ids)] from LOD 0."""
    nbp, obp = struct.unpack_from('<2i', d, 28)
    out = []

    def groups(so, nsg, wide):
        size = 33 if wide else 25
        res = []
        for g in range(nsg):
            go = so + g * size
            nv, vo, ni, io = struct.unpack_from('<4i', d, go)
            verts = [struct.unpack_from('<H', d, go + vo + 9 * v + 4)[0] for v in range(nv)]
            idx = struct.unpack_from('<%dH' % ni, d, go + io)
            if any(i >= nv for i in idx):
                raise ValueError
            res.append((verts, idx))
        return res
    for b in range(min(nbp, len(parts))):
        bo = obp + b * 8
        nm, mo = struct.unpack_from('<2i', d, bo)
        for k in range(min(nm, len(parts[b]))):
            mho = bo + mo + k * 8
            nl, lo = struct.unpack_from('<2i', d, mho)
            if nl == 0:
                continue
            lho = mho + lo	# LOD 0
            nmesh, meo = struct.unpack_from('<2i', d, lho)
            for q in range(nmesh):
                mho2 = lho + meo + q * 9
                nsg, sgo = struct.unpack_from('<2i', d, mho2)
                try:
                    gs = groups(mho2 + sgo, nsg, False)
                except Exception:
                    gs = groups(mho2 + sgo, nsg, True)
                for verts, idx in gs:
                    for t in range(0, len(idx) - 2, 3):
                        out.append((b, k, q, (verts[idx[t]], verts[idx[t + 1]], verts[idx[t + 2]])))
    return out


def read_model(mdl_path, parts_pick=None):
    base = os.path.splitext(mdl_path)[0]
    ver, bones, mats, cds, parts = read_mdl(open(mdl_path, 'rb').read())
    V = read_vvd(open(base + '.vvd', 'rb').read())
    vtxp = base + '.dx90.vtx'
    if not os.path.exists(vtxp):
        vtxp = base + '.vtx'
    T = read_vtx(open(vtxp, 'rb').read(), parts)
    # which model of each body part: the first one with meshes (or parts_pick[b])
    pick = []
    for b, models in enumerate(parts):
        c = parts_pick[b] if parts_pick and b < len(parts_pick) else None
        if c is None:
            c = next((k for k, (vi, ms) in enumerate(models) if ms and sum(m[2] for m in ms)), -1)
        pick.append(c)
    tris = []
    used = {}
    for b, k, q, (x, y, z) in T:
        if pick[b] != k:
            continue
        vi, meshes = parts[b][k]
        mat, voff, _ = meshes[q]
        g = [vi + voff + x, vi + voff + y, vi + voff + z]
        tris.append((mat, tuple(used.setdefault(i, len(used)) for i in g)))
    order = sorted(used, key=used.get)
    sub = V[np.array(order, dtype=np.int64)]
    W = []
    for r in sub:
        W.append([(int(r['b'][j]), float(r['w'][j])) for j in range(int(r['nb'])) if r['w'][j] > 0])
    return dict(bones=bones, P=sub['p'].astype(float), N=sub['n'].astype(float), UV=sub['uv'].astype(float),
                W=W, tris=tris, materials=mats, cddirs=cds, version=ver)


# --- materials ---------------------------------------------------------------

def _ifind(root, rel):
    """Case-insensitive path lookup under root."""
    cur = root
    for part in rel.replace('\\', '/').split('/'):
        if not part:
            continue
        try:
            ents = os.listdir(cur)
        except OSError:
            return None
        low = part.lower()
        m = next((e for e in ents if e.lower() == low), None)
        if m is None:
            return None
        cur = os.path.join(cur, m)
    return cur


def vmt_basetexture(path):
    import re
    t = open(path, 'rb').read().decode('latin-1')
    inc = re.search(r'"?include"?\s+"([^"]+)"', t, re.I)
    m = re.search(r'"?\$basetexture"?\s+"?([^"\s]+)"?', t, re.I)
    tr = bool(re.search(r'"?\$(translucent|alphatest)"?\s+"?1', t, re.I))
    return (m.group(1) if m else None), tr, (inc.group(1) if inc else None)


def find_texture(roots, mats, cds, i):
    """(vtf path, alpha-tested) for material i, searching every addon root."""
    name = mats[i]
    for cd in cds + ['']:
        for root in roots:
            p = _ifind(root, 'materials/' + cd + name + '.vmt')
            if not p:
                continue
            bt, tr, inc = vmt_basetexture(p)
            if not bt and inc:
                q = _ifind(root, inc)
                if q:
                    bt, tr2, _ = vmt_basetexture(q)
                    tr = tr or tr2
            if not bt:
                continue
            for r2 in roots:
                v = _ifind(r2, 'materials/' + bt + '.vtf')
                if v:
                    return v, tr
    return None, False


FMT = {0: ('RGBA', 4), 1: ('ABGR', 4), 2: ('RGB', 3), 3: ('BGR', 3), 12: ('BGRA', 4), 13: ('DXT1', 0.5),
       14: ('DXT3', 1), 15: ('DXT5', 1), 16: ('BGRX', 4), 20: ('DXT1A', 0.5)}


def read_vtf(path):
    from PIL import Image
    d = open(path, 'rb').read()
    if d[:4] != b'VTF\0':
        raise ValueError('not a vtf')
    vmaj, vmin, hs = struct.unpack_from('<3I', d, 4)
    w, h, flags, frames = struct.unpack_from('<HHIH', d, 16)
    fmt = struct.unpack_from('<i', d, 52)[0]
    mips = d[56]
    if fmt not in FMT:
        raise ValueError('vtf format %d' % fmt)
    kind, bpp = FMT[fmt]

    def size(mw, mh):
        if kind.startswith('DXT'):
            return max(1, (mw + 3) // 4) * max(1, (mh + 3) // 4) * (8 if kind.startswith('DXT1') else 16)
        return mw * mh * int(bpp)
    top = size(w, h) * max(1, frames)
    data = d[len(d) - top:len(d) - top + size(w, h)]	# the full-size mip is stored last
    if kind.startswith('DXT'):
        n = {'DXT1': 1, 'DXT1A': 1, 'DXT3': 2, 'DXT5': 3}[kind]
        im = Image.frombytes('RGBA', (w, h), data, 'bcn', n)
    elif kind == 'ABGR':
        a = np.frombuffer(data, np.uint8).reshape(h, w, 4)[:, :, [3, 2, 1, 0]]
        im = Image.fromarray(np.ascontiguousarray(a), 'RGBA')
    elif kind == 'BGRX':
        a = np.frombuffer(data, np.uint8).reshape(h, w, 4)[:, :, [2, 1, 0]]
        im = Image.fromarray(np.ascontiguousarray(a), 'RGB').convert('RGBA')
    else:
        im = Image.frombytes('RGBA' if bpp == 4 else 'RGB', (w, h), data, 'raw', kind).convert('RGBA')
    return im
