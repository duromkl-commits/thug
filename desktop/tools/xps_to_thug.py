#!/usr/bin/env python3
"""XNALara .xps models (GTA San Andreas peds ripped with the GTA SA rig) ->
THUG skater bodies for the desktop port.

    xps_to_thug.py <thug data dir> <out data dir> <name> <model.xps> [texture dir]

Writes models/gta/<name>/<name>.skin.xbx, .tex.xbx and .cas.xbx under <out
data dir>. The ped_body entry and the skater profile are added by the game
(desktop/src/custom_music.cpp, s_gta).

How:
- the .xps is read (binary XPS 2.x: bones, one or more meshes, 4 weights);
- scaled to a THUG pro's height (inches); same axes as THUG (Y up, facing +Z, left at +X);
- re-posed from GTA's A-pose to THUG's T-pose: every limb bone is turned so
  it points the way its THUG bone does (forward kinematics from the hips,
  keeping GTA's own bone lengths, so nothing tears);
- each GTA bone's weights go to the THUG bone it stands for
  (skeletons/thps5_human.ske.xbx), the three biggest kept, packed 11:11:10;
- written as a THUG skin scene (one sector, one mesh per texture, strips of
  stitched triangles) and a 32-bit swizzled texture dictionary with mips.
"""

import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image


def crc(s):
    return zlib.crc32(s.lower().encode()) ^ 0xffffffff


# --- XPS ---------------------------------------------------------------------

def rstr(d, p):
    n = sh = 0
    while True:
        b = d[p]; p += 1
        n |= (b & 0x7f) << sh; sh += 7
        if not b & 0x80:
            break
    return d[p:p + n].decode('latin-1'), p + n


def read_xps(fn):
    d = open(fn, 'rb').read()
    magic, vmaj, vmin = struct.unpack_from('<IHH', d, 0)
    if magic != 323232:
        raise ValueError('%s: not a binary .xps' % fn)
    if vmaj != 2 or vmin <= 12:
        raise ValueError('%s: XPS %d.%d not handled (2.13+ only)' % (fn, vmaj, vmin))
    # The header's settings block varies; the bone list follows it, starting
    # with the count and the root bone's name.
    # (A saved pose in the header names the bones too, as text.)
    i = d.find(b'\x0broot ground')
    if i < 0:
        raise ValueError('%s: no GTA rig (root ground)' % fn)
    p = i - 4
    nb, = struct.unpack_from('<I', d, p); p += 4
    bones = []
    for _ in range(nb):
        nm, p = rstr(d, p)
        par, = struct.unpack_from('<h', d, p)
        pos = struct.unpack_from('<3f', d, p + 2); p += 14
        bones.append((nm, par, np.array(pos)))
    nmesh, = struct.unpack_from('<I', d, p); p += 4
    meshes = []
    for _ in range(nmesh):
        nm, p = rstr(d, p)
        uvl, ntex = struct.unpack_from('<II', d, p); p += 8
        tex = []
        for _ in range(ntex):
            t, p = rstr(d, p); tex.append(t); p += 4
        nv, = struct.unpack_from('<I', d, p); p += 4
        V = []
        for _ in range(nv):
            pos = struct.unpack_from('<3f', d, p); nrm = struct.unpack_from('<3f', d, p + 12); p += 28
            uv = struct.unpack_from('<2f', d, p); p += 8 * uvl
            bi = struct.unpack_from('<4H', d, p); bw = struct.unpack_from('<4f', d, p + 8); p += 24
            V.append((pos, nrm, uv, bi, bw))
        nf, = struct.unpack_from('<I', d, p); p += 4
        F = [struct.unpack_from('<3I', d, p + 12 * k) for k in range(nf)]; p += 12 * nf
        meshes.append((nm, tex, V, F))
    return bones, meshes


# --- THUG skeleton -------------------------------------------------------------

THUG_BONES = [
    'Control_Root', 'Bone_Pelvis', 'Bone_Stomach_Lower', 'Bone_Stomach_Upper', 'Bone_Chest',
    'Bone_Collar_L', 'Bone_Bicep_L', 'Bone_Forearm_L', 'Bone_Palm_L', 'Bone_Fingers_Base_L',
    'Bone_Fingers_Tip_L', 'Bone_Forefinger_Base_L', 'Bone_Forefinger_Tip_L', 'Bone_Thumb_L',
    'Bone_Wrist_L', 'Cloth_Cuff_L', 'Bone_Shoulder_L', 'Bone_Collar_R', 'Bone_Bicep_R',
    'Bone_Forearm_R', 'Bone_Palm_R', 'Bone_Forefinger_Base_R', 'Bone_Forefinger_Tip_R',
    'Bone_Fingers_Base_R', 'Bone_Fingers_Tip_R', 'Bone_Thumb_R', 'Bone_Wrist_R', 'Cloth_Cuff_R',
    'Bone_Shoulder_R', 'Bone_Neck', 'Bone_Head', 'Bone_Jaw', 'Cloth_Hat', 'Bone_Brow',
    'Bone_PonyTail_1', 'Bone_PonyTail_2', 'Cloth_Breast', 'Cloth_Shirt_L', 'Cloth_Shirt_C',
    'Cloth_Shirt_R', 'Bone_Thigh_R', 'Bone_Knee_R', 'Bone_Ankle_R', 'Bone_Toe_R',
    'Cloth_Trouser_R', 'Bone_Thigh_L', 'Bone_Knee_L', 'Cloth_Trouser_L', 'Bone_Ankle_L',
    'Bone_Toe_L', 'Bone_Board_Root', 'Bone_Board_Nose', 'Bone_Trucks_Nose', 'Bone_Board_Tail',
    'Bone_Trucks_Tail']


def quat_matrix(q):
    # Gfx/Skeleton.cpp QuatVecToMatrix: the quaternion is inverted; row vectors.
    x, y, z, w = -q[0], -q[1], -q[2], q[3]
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
                     [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
                     [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]])


def read_skeleton(fn):
    """Bone index by name, and each bone's bind position (model space, inches)."""
    d = open(fn, 'rb').read()
    _, _, n = struct.unpack_from('<IIi', d, 0)
    names = struct.unpack_from('<%dI' % n, d, 12)
    parents = struct.unpack_from('<%dI' % n, d, 12 + 4 * n)
    by_crc = {crc(b): b for b in THUG_BONES}
    idx = {c: i for i, c in enumerate(names)}
    p = 12 + 12 * n
    W = [None] * n
    for i in range(n):
        q = struct.unpack_from('<4f', d, p); t = struct.unpack_from('<4f', d, p + 16); p += 32
        L = np.eye(4); L[:3, :3] = quat_matrix(q); L[3, :3] = t[:3]
        W[i] = L if parents[i] not in idx else L @ W[idx[parents[i]]]
    index = {by_crc[c]: i for i, c in enumerate(names) if c in by_crc}
    pos = {by_crc[c]: W[i][3, :3] for i, c in enumerate(names) if c in by_crc}
    missing = [b for b in THUG_BONES if b not in index]
    if missing:
        raise ValueError('skeleton: bones not found: %s' % missing)
    return index, pos


# --- GTA -> THUG ---------------------------------------------------------------

# GTA bone -> THUG bone that gets its weights.
BONE_MAP = {
    'root ground': 'Bone_Pelvis', 'root hips': 'Bone_Pelvis',
    'spine lower': 'Bone_Stomach_Lower', 'belly': 'Bone_Stomach_Upper', 'spine upper': 'Bone_Chest',
    'breast right base': 'Bone_Chest', 'breast left base': 'Bone_Chest',
    'head neck lower': 'Bone_Neck', 'head neck upper': 'Bone_Head',
    'head eyebrow right 1': 'Bone_Head', 'head eyebrow left 1': 'Bone_Head', 'head jaw': 'Bone_Head',
}
for s, S in (('right', 'R'), ('left', 'L')):
    BONE_MAP.update({
        'leg %s thigh' % s: 'Bone_Thigh_' + S, 'leg %s knee' % s: 'Bone_Knee_' + S,
        'leg %s ankle' % s: 'Bone_Ankle_' + S, 'leg %s toes' % s: 'Bone_Toe_' + S,
        'arm %s shoulder 1' % s: 'Bone_Collar_' + S, 'arm %s shoulder 2' % s: 'Bone_Bicep_' + S,
        'arm %s elbow' % s: 'Bone_Forearm_' + S, 'arm %s wrist' % s: 'Bone_Palm_' + S,
        'arm %s finger 2a' % s: 'Bone_Fingers_Base_' + S, 'arm %s finger 2b' % s: 'Bone_Fingers_Tip_' + S,
    })

# GTA bone whose segment (to the next GTA joint) is turned to point along a
# THUG segment: (GTA from, GTA to, THUG from, THUG to).
AIM = []
for s, S in (('right', 'R'), ('left', 'L')):
    AIM += [
        ('arm %s shoulder 2' % s, 'arm %s elbow' % s, 'Bone_Bicep_' + S, 'Bone_Forearm_' + S),
        ('arm %s elbow' % s, 'arm %s wrist' % s, 'Bone_Forearm_' + S, 'Bone_Palm_' + S),
        ('arm %s wrist' % s, 'arm %s finger 2a' % s, 'Bone_Palm_' + S, 'Bone_Fingers_Base_' + S),
        ('arm %s finger 2a' % s, 'arm %s finger 2b' % s, 'Bone_Fingers_Base_' + S, 'Bone_Fingers_Tip_' + S),
        ('leg %s thigh' % s, 'leg %s knee' % s, 'Bone_Thigh_' + S, 'Bone_Knee_' + S),
        ('leg %s knee' % s, 'leg %s ankle' % s, 'Bone_Knee_' + S, 'Bone_Ankle_' + S),
        ('leg %s ankle' % s, 'leg %s toes' % s, 'Bone_Ankle_' + S, 'Bone_Toe_' + S),
    ]


def inherit_unmapped(bones, fn):
    """Bones of fuller rigs (face, thumb, toe-nub, accessory) that have no THUG
    bone take their nearest mapped ancestor's: they follow it in the repose
    and give it their weights."""
    extra = []
    for nm, par, _ in bones:
        if nm in BONE_MAP:
            continue
        a = par
        while a >= 0 and bones[a][0] not in BONE_MAP:
            a = bones[a][1]
        if a < 0:
            raise ValueError('%s: GTA bone %s has no mapped ancestor' % (fn, nm))
        BONE_MAP[nm] = BONE_MAP[bones[a][0]]
        extra.append(nm)
    if extra:
        print('%s: %d extra bones folded into their parents' % (os.path.basename(os.path.dirname(fn)), len(extra)))


def align(a, b):
    """Row-vector rotation turning direction a onto b (shortest arc)."""
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-8:
        return np.eye(3)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    R = np.eye(3) + K + K @ K * (1 / (1 + c))	# column-vector form
    return R.T


def repose(bones, tpos, scale):
    """Per GTA bone: (rotation, rest joint, posed joint) for v' = (v - rest) R + posed."""
    names = [b[0] for b in bones]
    rest = [b[2] * scale for b in bones]
    aim = {g: (names.index(gt), tpos[tf], tpos[tt]) for g, gt, tf, tt in AIM if gt in names}	# a rig missing a fingertip/toe keeps that bone as is
    R = [None] * len(bones); posed = [None] * len(bones)
    for i, (nm, par, _) in enumerate(bones):
        if par < 0:
            R[i] = np.eye(3); posed[i] = rest[i]; continue
        posed[i] = posed[par] + (rest[i] - rest[par]) @ R[par]
        if nm in aim:
            child, a, b = aim[nm]
            R[i] = align(rest[child] - rest[i], b - a)
        else:
            R[i] = R[par]
    return R, rest, posed


# --- writers -------------------------------------------------------------------

# Material: copied from a THUG ped's (one opaque pass, the texture's checksum
# at byte 32); checksums patched in.
MAT_TEMPLATE = bytes.fromhex(
    '0000000000000000010000000100000000000000000000000000000000000000'
    '0000000024100000000000003f0000003f0000003f0000000000000000000000'
    '00000000000000404000004040040001000100000004000000000000c1000000'
    'c1')


def material(mat_crc, tex_crc):
    m = bytearray(MAT_TEMPLATE)
    struct.pack_into('<II', m, 0, mat_crc, mat_crc)
    struct.pack_into('<I', m, 32, tex_crc)
    return bytes(m)


def strip(tris):
    """Triangle list -> one strip, triangles stitched with degenerates, each
    starting on an even index (THUG's winding)."""
    s = []
    for a, b, c in tris:
        if s:
            s += [s[-1], a]
            if len(s) % 2:
                s.append(a)
        s += [a, b, c]
    return s


def pack_weights(ws):
    w = [min(1.0, max(0.0, x)) for x in ws]
    a = int(round(w[0] * 1023)); b = int(round(w[1] * 1023)); c = int(round(w[2] * 511))
    return a | (b << 11) | (c << 22)


def write_skin(fn, sector_crc, P, N, UV, W, BI, meshes):
    """meshes: [(mat_crc, tex_crc, [tri...])]."""
    o = bytearray(struct.pack('<3I', 1, 1, 1))
    o += struct.pack('<I', len(meshes))
    for mc, tc, _ in meshes:
        o += material(mc, tc)
    o += struct.pack('<i', 1)
    flags = 0x80402057		# a THUG pro's: uv, colours, normals, weights
    mn = P.min(0); mx = P.max(0); cen = (mn + mx) / 2; rad = float(np.linalg.norm(P - cen, axis=1).max())
    o += struct.pack('<IiII', sector_crc, -1, flags, len(meshes))
    o += struct.pack('<6f', *mn, *mx) + struct.pack('<4f', *cen, rad)
    nv = len(P)
    o += struct.pack('<ii', nv, 60)
    o += P.astype('<f4').tobytes() + N.astype('<f4').tobytes()
    o += struct.pack('<%dI' % nv, *W) + np.array(BI, dtype='<u2').tobytes()
    o += struct.pack('<i', 1) + UV.astype('<f4').tobytes()
    o += struct.pack('<%dI' % nv, *([0x80808080] * nv))
    for mc, _, tris in meshes:
        used = sorted({i for t in tris for i in t})
        q = P[used]; a = q.min(0); b = q.max(0); c = (a + b) / 2
        r = float(np.linalg.norm(q - c, axis=1).max())
        s = strip(tris)
        o += struct.pack('<4f', *c, r) + struct.pack('<6f', *a, *b)
        o += struct.pack('<3I', 0, mc, 1) + struct.pack('<i', len(s)) + struct.pack('<%dH' % len(s), *s)
    o += struct.pack('<i', 0)
    open(fn, 'wb').write(o)


def swizzle(px, w, h):
    """Linear (h, w, 4) -> Xbox Morton order (the inverse of p_NxTexture.cpp's unswizzle)."""
    out = np.empty((w * h, 4), np.uint8)
    flat = px.reshape(-1, 4)
    for pixel in range(w * h):
        hw, hh = w // 2, h // 2
        x = y = 0; bx = by = bit = 1
        while hw or hh:
            if hw:
                hw //= 2
                if pixel & bit: x |= bx
                bx *= 2; bit *= 2
            if hh:
                hh //= 2
                if pixel & bit: y |= by
                by *= 2; bit *= 2
        out[pixel] = flat[x + y * w]
    return out


def pow2(n):
    p = 1
    while p < n:
        p *= 2
    return min(p, 512)


def write_tex(fn, textures):
    """textures: [(checksum, PIL image)] -> 32-bit A8R8G8B8 swizzled, full mip chain."""
    o = bytearray(struct.pack('<2I', 1, len(textures)))
    for tc, img in textures:
        img = img.convert('RGBA')
        w, h = pow2(img.width), pow2(img.height)
        img = img.resize((w, h), Image.LANCZOS)
        levels = []
        while True:
            a = np.array(img, np.uint8)
            bgra = a[:, :, [2, 1, 0, 3]].copy()
            levels.append(swizzle(bgra, img.width, img.height).tobytes())
            if img.width == 1 and img.height == 1:
                break
            img = img.resize((max(1, img.width // 2), max(1, img.height // 2)), Image.LANCZOS)
        o += struct.pack('<8I', tc, w, h, len(levels), 32, 32, 0, 0)
        for l in levels:
            o += struct.pack('<I', len(l)) + l
    open(fn, 'wb').write(o)


# --- main ----------------------------------------------------------------------

def convert(data, out, name, xps_fn, tex_dir=None):
    index, tpos = read_skeleton(os.path.join(data, 'skeletons', 'thps5_human.ske.xbx'))
    bones, meshes = read_xps(xps_fn)
    inherit_unmapped(bones, xps_fn)
    # Scaled to a THUG pro's height (72.6 in; GTA's rigs put the hip joint at
    # different heights, so it can't be the yardstick). GTA's women come out
    # as tall as the men; the game shrinks them back with female_scale_info.
    top = max(v[0][1] for _, _, V, _ in meshes for v in V)
    scale = 72.6 / top
    R, rest, posed = repose(bones, tpos, scale)
    tex_dir = tex_dir or os.path.dirname(xps_fn)

    P = []; N = []; UV = []; W = []; BI = []
    out_meshes = []; textures = {}
    for mi, (mname, tex, V, F) in enumerate(meshes):
        base = len(P)
        for pos, nrm, uv, bi, bw in V:
            p = np.array(pos) * scale
            q = np.zeros(3); nq = np.zeros(3); tot = 0.0
            per = {}
            for b, w in zip(bi, bw):
                if w <= 0:
                    continue
                q += w * ((p - rest[b]) @ R[b] + posed[b])
                nq += w * (np.array(nrm) @ R[b])
                tot += w
                t = index[BONE_MAP[bones[b][0]]]
                per[t] = per.get(t, 0.0) + w
            if tot <= 0:
                q = p; nq = np.array(nrm); per = {index['Bone_Pelvis']: 1.0}; tot = 1.0
            q /= tot; nq /= max(1e-6, np.linalg.norm(nq))
            P.append(q); N.append(nq); UV.append(uv)
            W.append({b: w / tot for b, w in per.items()})
        tname = os.path.basename(tex[0]) if tex else None
        tc = crc('gta/%s/%s' % (name, tname or 'none'))
        if tname and tc not in textures:
            textures[tc] = Image.open(os.path.join(tex_dir, tname))
        tris = [(base + a, base + c, base + b) for a, b, c in F]	# XPS winds the other way
        out_meshes.append((crc('gta_%s_mat%d' % (name, mi)), tc, tris))

    if len(P) > 65535:
        raise ValueError('%s: %d vertices, over THUG\'s 16-bit limit' % (xps_fn, len(P)))
    P = np.array(P); N = np.array(N); UV = np.array(UV)
    import thug_fit
    P, W = thug_fit.fit(P, W, thug_fit.joints(bones, posed), index, tpos,
                        thug_fit.pro_reference(data), log=print)
    for i, w in enumerate(W):
        t3 = thug_fit.top3(w)
        W[i] = pack_weights([x for _, x in t3]); BI.append([b for b, _ in t3] + [0])
    d = os.path.join(out, 'models', 'gta', name)
    os.makedirs(d, exist_ok=True)
    write_skin(os.path.join(d, name + '.skin.xbx'), crc('gta_' + name), P, N, UV, W, BI, out_meshes)
    write_tex(os.path.join(d, name + '.tex.xbx'), list(textures.items()))
    open(os.path.join(d, name + '.cas.xbx'), 'wb').write(struct.pack('<3I', 2, 0, 0))
    return len(P), sum(len(m[2]) for m in out_meshes), P.min(0), P.max(0)


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    nv, nt, a, b = convert(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],
                           sys.argv[5] if len(sys.argv) > 5 else None)
    print('%s: %d vertices, %d triangles, bbox %s %s' % (sys.argv[3], nv, nt, np.round(a, 1), np.round(b, 1)))
