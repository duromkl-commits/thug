#!/usr/bin/env python3
"""ReTHAWed (THAW PC) zone -> THUG level files, for the desktop port.

    thaw_level_to_thug.py <z_<zone>.pak.wpc> <L> <out Data folder>

writes levels/<L>/<L>.scn.xbx, .col.xbx, .tex.xbx, <L>.qb, <L>_scripts.qb.

The zone pak holds the zone's textures, scene, collision and qb files. THAW
is THUG's engine two games on, so most things map across directly:
 - scene: THAW's per-mesh vertex buffers (pos, normal, colour, uv; stride 36)
   are merged per sector into THUG's streams; strips carry over; materials are
   single-pass with THUG's own flag bits and blend modes. Vertex colours are
   already THUG's 0-128 range.
 - textures: decoded and written as 32-bit (THUG's tex writer).
 - collision: version 10 -> 9. Same objects, vertices, intensities and
   faces; THAW's header has extra fields and its BSP is rebuilt as one leaf
   per object (like the THPS4 converter).
 - qb: THAW's item-tree qb with LZSS scripts -> THUG's token stream. The
   script bytecode is THUG's, except THAW's short-offset if/else.
   <zone>_NodeArray is renamed <L>_NodeArray. Game objects (CTF flags, crowns:
   THAW models) are made inert. Gap scripts carry over as they are.
Personal use with your own copy of the game data.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thaw_qb as Q  # noqa: E402
from thaw_qb import crc  # noqa: E402


# ---------------------------------------------------------------- pak

def read_pak(fn):
    d = open(fn, 'rb').read()
    out = []
    o = 0
    while o + 32 <= len(d):
        t, off, sz, c1, c2, c3, c4, fl = struct.unpack_from('<8I', d, o)
        if t in (0, 0x2cb3ef3b):
            break
        name = ''
        step = 32
        if fl & 0x20:
            name = d[o + 32:o + 192].split(b'\0')[0].decode('latin-1')
            step = 192
        out.append((t, name, d[o + off:o + off + sz]))
        o += step
    return out


T_TEX, T_SCN, T_COL, T_QB, T_QB2, T_QB3 = 0x8bfa5e8e, 0x7ea7357b, 0x72a6d78c, 0x49875607, 0x5d796624, 0xa7f505c4


# ---------------------------------------------------------------- textures

def convert_tex(path_or_bytes):
    import tempfile
    import thaw_formats as TF
    from xps_to_thug import write_tex
    with tempfile.NamedTemporaryFile(suffix='.tex.wpc', delete=False) as f:
        f.write(path_or_bytes)
        fn = f.name
    try:
        texs = TF.read_tex(fn)
    finally:
        os.unlink(fn)
    out = fn + '.xbx'
    write_tex(out, [(ck, img) for ck, (img, fm) in texs.items()])
    data = open(out, 'rb').read()
    os.unlink(out)
    alpha = {}
    for ck, (img, fm) in texs.items():
        a = np.array(img.convert('RGBA'))[:, :, 3]
        alpha[ck] = (a < 250).mean() > 0.01
    return data, alpha


# ---------------------------------------------------------------- scene

def material(m, has_alpha):
    """THAW 0x120-byte material -> THUG material record."""
    mc, name, passes = struct.unpack_from('<3I', m, 0)
    flags0, = struct.unpack_from('<I', m, 0x0c)
    pflags, = struct.unpack_from('<I', m, 0x30)
    tex, = struct.unpack_from('<I', m, 0x40)
    col = struct.unpack_from('<4f', m, 0x50)
    blend, fixed = struct.unpack_from('<2I', m, 0x90)
    tile = struct.unpack_from('<2f', m, 0xb0)
    if passes == 0:
        tex, pflags, blend = 0, 0, 0
    pflags &= 0x00001fff & ~(0x1 | 0x2 | 0x800)	# no wibble/animation data carried over
    if tex:
        pflags |= 0x04
    two_sided = bool(flags0 & 0x100) or blend != 0
    acut = 64 if (has_alpha.get(tex) and blend == 0) else 1
    o = struct.pack('<4I', mc, name, 1, acut)
    o += struct.pack('<BfBBiBf', 0, 0.0, 0, 1 if two_sided else 0, 0, 0, 0.0)
    rgb = [max(0.0, min(1.0, c / 2)) for c in col[:3]] if passes else [0.5, 0.5, 0.5]
    reg = (blend & 0xffffff) | ((fixed & 0xff) << 32)
    o += struct.pack('<IIB3fQII2fI', tex, pflags, 1 if rgb != [0.5] * 3 else 0, *rgb, reg, 0, 0,
                     tile[0] or 1.0, tile[1] or 1.0, 0x10004)
    o += struct.pack('<IIff', 1, 4, -8.0, -8.0)
    return o


def convert_scn(d, has_alpha):
    sz, = struct.unpack_from('<I', d, 0x24)
    S = 0x20 + sz
    B = S + 8 + struct.unpack_from('<I', d, S + 4)[0]
    nmat, = struct.unpack_from('<H', d, 0x22)
    nsec, o1, o2, _, o3, o4 = struct.unpack_from('<6I', d, B + 0x80)
    mats = [d[0x30 + i * 0x120:0x30 + (i + 1) * 0x120] for i in range(nmat)]
    out = bytearray(struct.pack('<3I', 1, 1, 1))
    out += struct.pack('<I', nmat)
    for m in mats:
        out += material(m, has_alpha)
    out += struct.pack('<i', nsec)
    mo = B + o4
    dt = np.dtype({'names': ['p', 'n', 'c', 'uv'], 'formats': [('<f4', 3), ('<f4', 3), '<u4', ('<f4', 2)],
                   'offsets': [0, 12, 24, 28], 'itemsize': 36})
    sectors = []
    for s in range(nsec):
        scs, = struct.unpack_from('<I', d, B + o1 + 48 * s + 4)
        nm, = struct.unpack_from('<I', d, B + o2 + 0x68 * s + 0x44)
        P = []; C = []; UV = []; meshes = []; base = 0
        for _ in range(nm):
            assert d[mo] == 0x40, hex(mo)
            mat, = struct.unpack_from('<I', d, mo + 0x14)
            st = d[mo + 0x18]
            assert st == 36, st
            nv, = struct.unpack_from('<H', d, mo + 0x26)
            ni, = struct.unpack_from('<I', d, mo + 0x28)
            io, = struct.unpack_from('<I', d, mo + 0x5c)
            vo, = struct.unpack_from('<I', d, mo + 0x7c)
            v = np.frombuffer(d[B + vo:B + vo + nv * 36], dt)
            idx = np.array(struct.unpack_from('<%dH' % ni, d, B + io + 4), np.int64) + base
            P.append(v['p']); C.append(v['c']); UV.append(v['uv'])
            meshes.append((mat, idx))
            base += nv
            mo += 0xe0
        sectors.append(scs)
        P = np.concatenate(P) if P else np.zeros((0, 3), np.float32)
        C = np.concatenate(C) if C else np.zeros(0, np.uint32)
        UV = np.concatenate(UV) if UV else np.zeros((0, 2), np.float32)
        if base > 65535:
            raise ValueError('sector %08x: %d vertices' % (scs, base))
        flags = 0x80000003
        if len(P):
            lo, hi = P.min(0), P.max(0)
        else:
            lo = hi = np.zeros(3)
        c = (lo + hi) / 2
        rad = float(np.linalg.norm(P - c, axis=1).max()) if len(P) else 0.0
        out += struct.pack('<IiII', scs, -1, flags, len(meshes))
        out += struct.pack('<6f', *lo, *hi) + struct.pack('<4f', *c, rad)
        out += struct.pack('<ii', base, 24)
        out += P.astype('<f4').tobytes()
        out += struct.pack('<i', 1) + UV.astype('<f4').tobytes()
        out += C.astype('<u4').tobytes()
        for mat, idx in meshes:
            q = P[np.unique(idx)] if len(idx) else P[:1]
            a, b = q.min(0), q.max(0); mc = (a + b) / 2
            r = float(np.linalg.norm(q - mc, axis=1).max())
            out += struct.pack('<4f', *mc, r) + struct.pack('<6f', *a, *b)
            out += struct.pack('<3I', 0, mat, 1) + struct.pack('<i', len(idx)) + idx.astype('<u2').tobytes()
    out += struct.pack('<i', 0)
    return bytes(out), sectors


# ---------------------------------------------------------------- collision

def convert_col(d, drop=()):
    version, nobj, total_verts, nfl, nfs = struct.unpack_from('<5i', d, 0)
    if version != 10:
        raise ValueError('col version %d, expected 10' % version)
    ob = 80
    objs = []
    for i in range(nobj):
        o = ob + 64 * i
        cs, fla, nv, nf, small, fixed, foff = struct.unpack_from('<IHHHBBI', d, o)
        bbox = d[o + 16:o + 48]
        voff_b, bsp, vidx, _ = struct.unpack_from('<4I', d, o + 48)
        objs.append([cs, fla, nv, nf, small, fixed, foff, bbox, vidx])
    vb = (ob + nobj * 64 + 15) & ~15
    ib = vb + total_verts * 12
    fb = (ib + total_verts + 3) & ~3
    nfl -= sum(o[3] for o in objs if o[0] in drop and not o[4])
    nfs -= sum(o[3] for o in objs if o[0] in drop and o[4])
    hdr = struct.pack('<8i', 9, nobj, total_verts, nfl, nfs, total_verts, 0, 0)
    obj_out = bytearray(); faces = bytearray(); nodes = bytearray(); fidx = bytearray(); nidx = 0
    for cs, fla, nv, nf, small, fixed, foff, bbox, vidx in objs:
        if cs in drop:
            nf = 0
        new_foff = len(faces)
        fsz = 8 if small else 10
        faces += d[fb + foff:fb + foff + nf * fsz]
        bsp_off = len(nodes)
        nodes += struct.pack('<BBHI', 3, 0, nf, nidx)
        fidx += struct.pack('<%dH' % nf, *range(nf))
        nidx += nf
        obj_out += struct.pack('<IHHHBBI', cs, fla, nv, nf, small, 0, new_foff) + bbox
        obj_out += struct.pack('<4I', vidx * 12, bsp_off, vidx, 0)
    out = bytearray(hdr + obj_out)
    out += bytes(((len(out) + 15) & ~15) - len(out))
    out += d[vb:vb + total_verts * 12] + d[ib:ib + total_verts]
    out += bytes(((len(out) + 3) & ~3) - len(out))
    out += faces
    if nfl & 1:
        out += bytes(2)
    out += struct.pack('<i', len(nodes)) + nodes + fidx
    return bytes(out)


# ---------------------------------------------------------------- qb

def fix_code(code):
    """THAW script bytes -> THUG: short if/else -> THUG's if/else."""
    out = bytearray()
    for i, t, b in Q.tokens(code):
        if t == 0x47:
            out.append(0x25)
        elif t == 0x48:
            out.append(0x26)
        elif t == 0x49:
            raise ValueError('THAW short jump in a script')
        else:
            out += b
    return bytes(out)


class W:
    def __init__(s): s.o = bytearray()
    def nl(s): s.o.append(1)
    def name(s, c): s.o += b'\x16' + struct.pack('<I', c)
    def value(s, v):
        o = s.o
        if isinstance(v, Q.Key): s.name(v)
        elif isinstance(v, bool): o += b'\x17' + struct.pack('<i', int(v))
        elif isinstance(v, int): o += b'\x17' + struct.pack('<i', v)
        elif isinstance(v, float): o += b'\x1a' + struct.pack('<f', v)
        elif isinstance(v, str):
            b = v.encode('latin-1') + b'\0'; o += b'\x1b' + struct.pack('<I', len(b)) + b
        elif isinstance(v, tuple) and len(v) == 3: o += b'\x1e' + struct.pack('<3f', *v)
        elif isinstance(v, tuple) and len(v) == 2: o += b'\x1f' + struct.pack('<2f', *v)
        elif isinstance(v, Q.Struct): s.struct_(v)
        elif isinstance(v, list): s.array(v)
        else:
            raise TypeError(type(v))

    def struct_(s, items):
        s.o.append(3)
        for k, v in items:
            if k:
                s.name(k); s.o.append(7)
            s.value(v)
        s.o.append(4)

    def array(s, v):
        s.o.append(5)
        for e in v:
            s.value(e)
        s.o.append(6)


def _looks_struct(v):
    return isinstance(v, Q.Struct)


def qb_to_thug(data, renames=None, node_fix=None, keep=None):
    q = Q.QB(data)
    renames = renames or {}
    w = W()
    for t, c, v in q.items:
        c = renames.get(c, c)
        if keep is not None and not keep(t, c, v):
            continue
        w.nl()
        if isinstance(v, Q.Script):
            w.o.append(0x23); w.name(c)
            w.o += fix_code(v.code)
            continue
        if node_fix and t == 0x0c and c in node_fix:
            v = node_fix[c](v)
        w.name(c); w.o.append(7)
        if isinstance(v, Q.Struct):
            w.struct_(v)
        else:
            w.value(v)
    w.nl()
    w.o.append(0)
    return bytes(w.o), q


INERT = crc('th4_unused')


def fix_nodes(nodes):
    """GameObjects (THAW models: CTF flags, crowns) can't be created in THUG."""
    out = []
    for n in nodes:
        n = Q.Struct(n)
        cls = dict((k, v) for k, v in n if k).get(crc('Class'))
        if cls == crc('GameObject'):
            n = Q.Struct([(k, Q.Key(INERT) if k == crc('Class') else v) for k, v in n
                          if k not in (crc('Model'),) and not (k == 0 and v == crc('CreatedAtStart'))])
        out.append(n)
    return out


def convert_level(pak, L, dst):
    files = read_pak(pak)
    zone = os.path.basename(pak).split('.')[0]			# z_ware
    o = os.path.join(dst, 'levels', L)
    os.makedirs(o, exist_ok=True)
    tex = next(b for t, n, b in files if t == T_TEX)
    tex_x, alpha = convert_tex(tex)
    open(os.path.join(o, L + '.tex.xbx'), 'wb').write(tex_x)
    scn, sectors = convert_scn(next(b for t, n, b in files if t == T_SCN), alpha)
    open(os.path.join(o, L + '.scn.xbx'), 'wb').write(scn)
    col = convert_col(next(b for t, n, b in files if t == T_COL))
    open(os.path.join(o, L + '.col.xbx'), 'wb').write(col)
    renames = {crc(zone + '_NodeArray'): crc(L + '_NodeArray')}
    qbs = {n: b for t, n, b in files if n.endswith('.qb.wpc')}
    main = next(b for n, b in qbs.items() if n.endswith('/%s.qb.wpc' % zone))
    q, _ = qb_to_thug(main, renames, {crc(L + '_NodeArray'): fix_nodes})
    open(os.path.join(o, L + '.qb'), 'wb').write(q)
    sc = next((b for n, b in qbs.items() if n.endswith('/%s_scripts.qb.wpc' % zone)), None)
    s, _ = qb_to_thug(sc) if sc else (b'\x01\x00', None)
    open(os.path.join(o, L + '_scripts.qb'), 'wb').write(s)
    print('%s: %d sectors, %d bytes scene, %d bytes collision -> %s' % (L, len(sectors), len(scn), len(col), o))


if __name__ == '__main__':
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    convert_level(*sys.argv[1:4])
