#!/usr/bin/env python3
"""Characters exported in a T-pose with no skeleton (plain .obj + .mtl, Y up)
-> THUG skater bodies for the desktop port. Started as the Casino Inc
converter; used for the Simpsons, whose arms stick straight out, which the
GTA rig borrowing in gtaobj_to_thug.py (built for A-poses) can't fit.

    tpose_to_thug.py <thug data dir> <out data dir> <name> <model.obj> [flip]

Writes models/gta/<name>/<name>.skin.xbx, .tex.xbx and .cas.xbx under <out
data dir>, one mesh per .mtl material (map_Kd next to the .obj); 'flip'
turns a model that faces -Z around. Same writers as xps_to_thug.py. There is no rig, so one is made:
the mesh is stood on the ground, centred, and warped piecewise so its arms,
crotch and shoulders sit where THUG's T-pose skeleton has them; each vertex
is then weighted to the THUG bones whose segments it lies nearest. Pieces
that stick far out from the skeleton and hang from a hand or the head
(canes, brooms, hats) go whole to that bone, so they move rigidly.
"""

import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xps_to_thug import crc, read_skeleton, pack_weights, write_skin, write_tex  # noqa: E402

ARM_Y = 58.4		# THUG T-pose arm line
CROTCH_Y = 33.5		# a THUG pro's crotch, under the hip joints (37.2)
SHOULDER_X = 7.0
TIP_X = 35.4		# fingertips
ARM_Z = -2.5
RIGID = 9.0			# a piece reaching this far (inches) from every bone moves rigidly


def load_obj(fn):
    V = []; T = []; N = []; F = []; mat = None
    for line in open(fn):
        s = line.split()
        if not s:
            continue
        if s[0] == 'usemtl':
            mat = s[1]
        elif s[0] == 'v':
            V.append([float(x) for x in s[1:4]])
        elif s[0] == 'vt':
            T.append([float(x) for x in s[1:3]])
        elif s[0] == 'vn':
            N.append([float(x) for x in s[1:4]])
        elif s[0] == 'f':
            c = [[int(x) - 1 if x else -1 for x in (k.split('/') + ['', ''])[:3]] for k in s[1:]]
            for k in range(1, len(c) - 1):
                F.append((c[0], c[k], c[k + 1], mat))
    return np.array(V), np.array(T), np.array(N) if N else None, F


def interp(x, xs, ys):
    """Piecewise linear, extended past the ends with the end slopes."""
    xs = np.asarray(xs, float); ys = np.asarray(ys, float)
    y = np.interp(x, xs, ys)
    lo = x < xs[0]; hi = x > xs[-1]
    y[lo] = ys[0] + (x[lo] - xs[0]) * (ys[1] - ys[0]) / (xs[1] - xs[0])
    y[hi] = ys[-1] + (x[hi] - xs[-1]) * (ys[-1] - ys[-2]) / (xs[-1] - xs[-2])
    return y


def fit(V):
    """Mesh -> THUG T-pose space (inches, +Z forward, +X the skater's left)."""
    P = V.copy()
    P[:, 1] -= P[:, 1].min()
    h = P[:, 1].max()
    mid = P[(P[:, 1] > 0.4 * h) & (P[:, 1] < 0.6 * h)]
    P[:, 0] -= np.median(mid[:, 0])
    torso = P[(P[:, 1] > 0.55 * h) & (P[:, 1] < 0.75 * h) & (np.abs(P[:, 0]) < 0.15 * h)]
    P[:, 2] -= np.median(torso[:, 2])
    span = np.abs(P[:, 0]).max()
    arm = P[np.abs(P[:, 0]) > 0.6 * span]
    arm_y = np.median(arm[:, 1])
    s = ARM_Y / arm_y
    P *= s
    # crotch: lowest point on the centre line above the shins; a dress or
    # apron hem reads too low, so fall back to a pro's proportions then
    c = P[(np.abs(P[:, 0]) < 1.0) & (P[:, 1] > 12)]
    crotch = c[:, 1].min() if len(c) else 0
    if crotch < 26 or crotch > 40:
        crotch = CROTCH_Y
    P[:, 1] = interp(P[:, 1], [0, crotch, ARM_Y], [0, CROTCH_Y, ARM_Y])
    # arms: the stretch past the shoulder onto THUG's, the arm line onto THUG's
    ax = np.abs(P[:, 0])
    band = (ax > SHOULDER_X + 3) & (np.abs(P[:, 1] - ARM_Y) < 6)
    tip = np.percentile(ax[band], 99) if band.sum() > 4 else TIP_X
    out = ax > SHOULDER_X
    k = np.clip((ax - SHOULDER_X) / 4.0, 0, 1)	# 0 at the shoulder, 1 four inches out
    nx = np.where(out, SHOULDER_X + (ax - SHOULDER_X) * (TIP_X - SHOULDER_X) / (tip - SHOULDER_X), ax)
    P[:, 0] = np.sign(P[:, 0]) * nx
    if band.sum() > 4:
        az = np.median(P[band, 2])
        P[:, 2] += k * (ARM_Z - az) * (P[:, 1] > ARM_Y - 15)
    return P


def segments(tpos):
    """(bone, from, to): the stretch of body each bone carries."""
    t = tpos
    up = lambda b, dy: t[b] + np.array([0, dy, 0])
    S = [
        ('Bone_Pelvis', t['Bone_Pelvis'] - np.array([0, 3, 0]), t['Bone_Stomach_Lower']),
        ('Bone_Stomach_Lower', t['Bone_Stomach_Lower'], t['Bone_Stomach_Upper']),
        ('Bone_Stomach_Upper', t['Bone_Stomach_Upper'], t['Bone_Chest']),
        ('Bone_Chest', t['Bone_Chest'], t['Bone_Neck']),
        ('Bone_Neck', t['Bone_Neck'], t['Bone_Head']),
        ('Bone_Head', t['Bone_Head'], up('Bone_Head', 10)),
    ]
    for s in 'LR':
        S += [
            ('Bone_Bicep_' + s, t['Bone_Shoulder_' + s], t['Bone_Forearm_' + s]),
            ('Bone_Forearm_' + s, t['Bone_Forearm_' + s], t['Bone_Wrist_' + s]),
            ('Bone_Palm_' + s, t['Bone_Wrist_' + s], t['Bone_Fingers_Tip_' + s]),
            ('Bone_Thigh_' + s, t['Bone_Thigh_' + s], t['Bone_Knee_' + s]),
            ('Bone_Knee_' + s, t['Bone_Knee_' + s], t['Bone_Ankle_' + s]),
            ('Bone_Ankle_' + s, t['Bone_Ankle_' + s], t['Bone_Toe_' + s]),
        ]
    return S


def seg_dist(P, a, b):
    ab = b - a
    u = np.clip(((P - a) @ ab) / max(1e-9, ab @ ab), 0, 1)
    return np.linalg.norm(P - (a + u[:, None] * ab), axis=1)


def allowed(P, bone):
    x, y = P[:, 0], P[:, 1]
    side = 1 if bone.endswith('_L') else -1
    if any(k in bone for k in ('Bicep', 'Forearm', 'Palm')):
        return x * side > 5
    if any(k in bone for k in ('Thigh', 'Knee', 'Ankle')):
        return (x * side > -1.5) & (y < 40)
    if bone in ('Bone_Neck', 'Bone_Head'):
        return (y > 56) & (np.abs(x) < 9)
    return np.abs(x) < 12


def components(n, F):
    p = list(range(n))

    def f(a):
        while p[a] != a:
            p[a] = p[p[a]]; a = p[a]
        return a
    for t in F:
        for c in t[1:3]:
            p[f(c[0])] = f(t[0][0])
    return np.array([f(i) for i in range(n)])


def weights(P, F, tpos):
    S = segments(tpos)
    D = np.full((len(P), len(S)), np.inf)
    for k, (b, a, c) in enumerate(S):
        m = allowed(P, b)
        D[m, k] = seg_dist(P[m], a, c)
    D[np.isinf(D).all(1), 0] = 1e3		# nowhere allowed (stray point): pelvis
    near = D.min(1)
    W = np.zeros_like(D)
    order = np.argsort(D, 1)
    for i in range(len(P)):
        ks = order[i, :3]
        d = np.maximum(D[i, ks], 0.25)
        w = 1.0 / d ** 4
        w[~np.isfinite(D[i, ks])] = 0
        W[i, ks] = w / w.sum()
    # props in the hands and hats: pieces sticking far out that hang from a
    # palm or the head move rigidly with it
    comp = components(len(P), F)
    for c in np.unique(comp):
        idx = np.where(comp == c)[0]
        if near[idx].max() < RIGID or len(idx) > 0.2 * len(P):
            continue
        j = idx[np.argmin(near[idx])]
        k = int(np.argmin(D[j]))
        if not any(t in S[k][0] for t in ('Palm', 'Head', 'Neck')):
            continue	# body pieces (coats, bellies) keep their blended weights
        W[idx] = 0
        W[idx, k] = 1.0
    return [s[0] for s in S], W


def materials(obj_fn):
    tex = {}; cur = None
    mtl = os.path.splitext(obj_fn)[0] + '.mtl'
    for line in open(mtl):
        s = line.split()
        if s and s[0] == 'newmtl':
            cur = s[1]
        elif s and s[0] == 'map_Kd' and cur:
            tex[cur] = os.path.join(os.path.dirname(obj_fn), s[-1])
    return tex


def convert(data, out, name, obj_fn, flip=False, sub='gta'):
    index, tpos = read_skeleton(os.path.join(data, 'skeletons', 'thps5_human.ske.xbx'))
    V, T, N, F = load_obj(obj_fn)
    if flip:
        V = V * [-1, 1, -1]
        N = N * [-1, 1, -1] if N is not None else None
    P = fit(V)
    bones, W = weights(P, F, tpos)
    # fit() with THUG's own joints (fit() already put the mesh on them): pro
    # weights, rigid head
    import thug_fit
    Wd = [{index[bones[k]]: W[i, k] for k in np.nonzero(W[i])[0]} for i in range(len(P))]
    P, Wd = thug_fit.fit(P, Wd, dict(tpos), index, tpos, thug_fit.pro_reference(data), log=print)
    if N is None:
        N = np.tile([0.0, 0.0, 1.0], (len(V), 1))
    PP = []; NN = []; UV = []; WW = []; BI = []
    remap = {}
    tris = {}
    for tri in F:
        ids = []
        for vi, ti, ni in tri[:3]:
            key = (vi, ti, ni)
            if key not in remap:
                remap[key] = len(PP)
                PP.append(P[vi])
                n = N[ni] if ni >= 0 else N[vi]
                NN.append(n / max(1e-6, np.linalg.norm(n)))
                UV.append((T[ti][0], 1.0 - T[ti][1]) if ti >= 0 else (0.0, 0.0))
                t3 = thug_fit.top3(Wd[vi])
                WW.append(pack_weights([x for _, x in t3]))
                BI.append([b for b, _ in t3] + [0])
            ids.append(remap[key])
        tris.setdefault(tri[3], []).append(tuple(ids))	# OBJ winds CCW like THUG
    PP = np.array(PP); NN = np.array(NN); UV = np.array(UV)
    texmap = materials(obj_fn)
    meshes = []; textures = {}
    for mi, (m, tl) in enumerate(tris.items()):
        tex_fn = texmap[m]
        tc = crc('%s/%s/%s' % (sub, name, os.path.basename(tex_fn)))
        textures.setdefault(tc, tex_fn)
        meshes.append((crc('%s_%s_mat%d' % (sub, name, mi)), tc, tl))
    d = os.path.join(out, 'models', sub, name)
    os.makedirs(d, exist_ok=True)
    write_skin(os.path.join(d, name + '.skin.xbx'), crc(sub + '_' + name), PP, NN, UV, WW, BI, meshes)
    write_tex(os.path.join(d, name + '.tex.xbx'), [(tc, Image.open(fn)) for tc, fn in textures.items()])
    open(os.path.join(d, name + '.cas.xbx'), 'wb').write(struct.pack('<3I', 2, 0, 0))
    return len(PP), sum(len(t) for t in tris.values()), PP.min(0), PP.max(0)


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    nv, nt, a, b = convert(*sys.argv[1:5], flip=sys.argv[5:6] == ['flip'])
    print('%s: %d vertices, %d triangles, bbox %s %s' % (sys.argv[3], nv, nt, np.round(a, 1), np.round(b, 1)))
