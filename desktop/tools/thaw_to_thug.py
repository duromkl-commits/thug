#!/usr/bin/env python3
"""THAW PC (ReTHAWed) skater models -> THUG skater bodies for the desktop port.

    thaw_to_thug.py <thug data dir> <out data dir> <model base path> <sub> <name>

<model base path> is e.g. .../Models/newPeds/thps3/THPS3Hawk (no extension);
writes models/<sub>/<name>/<name>.skin.xbx, .tex.xbx and .cas.xbx. THAW skins
are already in THUG's T-pose, facing +Z, in inches, and THAW's human skeleton is
THUG's with the legs renumbered, so the vertices carry over as they are and only
the bone indices are remapped.
"""

import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xps_to_thug import crc, read_skeleton, pack_weights, write_skin, write_tex, MAT_TEMPLATE  # noqa: E402
import xps_to_thug  # noqa: E402
import thaw_formats as TF  # noqa: E402

# THAW bone (index / 3) -> THUG thps5_human bone. 0..38 are the same bones.
LEGS = {39: 40, 40: 41, 41: 42, 42: 43, 43: 45, 44: 46, 45: 48, 46: 49}


CHEST, PONY1, PONY2 = 4, 34, 35


def thug_index(b):
    b //= 3
    return LEGS.get(b, b) if b > 38 else b


def unpack_normal(n):
    def s(v, bits):
        v = v.astype(np.int32)
        return np.where(v >= 1 << (bits - 1), v - (1 << bits), v)
    x = s(n & 0x7ff, 11) / 1023.0; y = s((n >> 11) & 0x7ff, 11) / 1023.0; z = s(n >> 22, 10) / 511.0
    v = np.stack([x, y, z], 1)
    return v / np.maximum(1e-6, np.linalg.norm(v, axis=1))[:, None]


def bleed(img):
    """See-through texels keep whatever colour the artist left (THPS1 Steamer's
    hair: magenta), which filtering and mips smear onto the edges. Fill them
    with their opaque neighbours' colour instead."""
    from PIL import Image
    a = np.array(img.convert('RGBA')).astype(np.float64)
    solid = a[:, :, 3] >= 128
    if solid.all() or not solid.any():
        return img
    rgb = a[:, :, :3] * solid[:, :, None]; have = solid.astype(np.float64)
    for _ in range(64):
        if have.all():
            break
        s = np.zeros_like(rgb); n = np.zeros_like(have)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (-1, -1), (1, -1), (-1, 1)):
            s += np.roll(np.roll(rgb * have[:, :, None], dy, 0), dx, 1)
            n += np.roll(np.roll(have, dy, 0), dx, 1)
        new = (have == 0) & (n > 0)
        rgb[new] = s[new] / n[new][:, None]
        have[new] = 1
    rgb[have == 0] = a[:, :, :3][solid].mean(0)
    keep = solid[:, :, None]
    a[:, :, :3] = np.where(keep, a[:, :, :3], rgb)
    return Image.fromarray(a.round().astype(np.uint8), 'RGBA')


def tris_of(idx):
    out = []
    for k in range(len(idx) - 2):
        a, b, c = idx[k], idx[k + 1], idx[k + 2]
        if a == b or b == c or a == c:
            continue
        out.append((a, c, b) if k % 2 else (a, b, c))
    return out


def convert(data, out, base, sub, name):
    read_skeleton(os.path.join(data, 'skeletons', 'thps5_human.ske.xbx'))	# sanity: THUG data present
    mats, meshes = TF.read_skin(base + '.skin.wpc')
    texs = TF.read_tex(base + '.tex.wpc')
    P = []; N = []; UV = []; W = []; BI = []
    groups = {}; n0 = 0
    for m in meshes:
        v = m['v']
        P.append(v['p'].astype(np.float64)); N.append(unpack_normal(v['n'])); UV.append(v['uv'])
        ws = TF.weights(v['w'])
        for k in range(len(v)):
            W.append(pack_weights(list(ws[k])))
            bi = [thug_index(int(b)) for b in v['b'][k][:3]]
            # THAW's jiggle bones for breasts are THUG's ponytail ones (they'd
            # follow the head): front-of-chest vertices go on the chest.
            if v['p'][k][1] < 58 and v['p'][k][2] > 1 and not {29, 30} & set(bi):
                bi = [CHEST if b in (PONY1, PONY2) else b for b in bi]
            BI.append(bi + [0])
        groups.setdefault(m['mat'], []).extend(tuple(i + n0 for i in t) for t in tris_of(m['idx']))
        n0 += len(v)
    P = np.concatenate(P); N = np.concatenate(N); UV = np.concatenate(UV).astype(np.float64)
    if len(P) > 65535:
        raise ValueError('%s: too many vertices' % name)

    textures = {}; mlist = []; flags = {}
    for mc, tris in groups.items():
        mrec = mats.get(mc)
        tck = struct.unpack_from('<I', mrec, 0x40)[0] if mrec else 0
        tc = crc('%s/%s/%08x' % (sub, name, tck))
        cut = 1
        if tck in texs and tc not in textures:
            img = bleed(texs[tck][0])
            textures[tc] = img
        if tck in texs:
            a = np.array(texs[tck][0])[:, :, 3]
            if (a < 250).mean() > 0.01:
                cut = 64	# hair cards, laces, see-through bits: alpha test, two-sided
        mcrc = crc('%s_%s_%08x' % (sub, name, mc))
        flags[mcrc] = cut
        mlist.append((mcrc, tc, tris))

    orig = xps_to_thug.material
    def material(mat_crc, tex_crc):
        m = bytearray(orig(mat_crc, tex_crc))
        if flags.get(mat_crc, 1) > 1:
            struct.pack_into('<I', m, 12, flags[mat_crc]); m[22] = 1
        return bytes(m)
    xps_to_thug.material = material
    try:
        d = os.path.join(out, 'models', sub, name)
        os.makedirs(d, exist_ok=True)
        write_skin(os.path.join(d, name + '.skin.xbx'), crc(sub + '_' + name), P, N, UV, W, BI, mlist)
    finally:
        xps_to_thug.material = orig
    write_tex(os.path.join(d, name + '.tex.xbx'), list(textures.items()))
    open(os.path.join(d, name + '.cas.xbx'), 'wb').write(struct.pack('<3I', 2, 0, 0))
    return len(P), sum(len(t) for _, _, t in mlist), P.min(0), P.max(0)


if __name__ == '__main__':
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    nv, nt, a, b = convert(*sys.argv[1:6])
    print('%s: %d vertices, %d triangles, bbox %s %s' % (sys.argv[5], nv, nt, np.round(a, 1), np.round(b, 1)))
