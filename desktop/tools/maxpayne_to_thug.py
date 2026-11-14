#!/usr/bin/env python3
"""Max Payne character skins -> THUG skater models.

usage: maxpayne_to_thug.py <thug data dir> <out data dir> <skins dir> <skeleton .kfs> <key>=<skin>[:<set>] ...
  <skins dir>  data/database/skins pulled out of x_data.ras (with sharedtextures)
  <skeleton>   data/database/skeletons/default_skeleton/maxpayne-collision.kfs
  <skin>       a skin .txt name (max_payne, C1_All_Johnny, ...); its [LOD] line
               names the mesh (.kfs) and weights (.skd)
  <set>        optional texture set .kfs (enemy looks) whose textures replace the base ones

Every skin is on Remedy's one default skeleton, already in its bind pose, so the
joints come straight from the skeleton's node matrices. The coat-tail bones hang
on the hips and thighs; the gun bone goes with the right hand."""
import os
import re
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import maxpayne_formats as F  # noqa: E402
from gtadae_to_thug import build, frame  # noqa: E402

# Remedy bone -> (xps name it drives, weight goes there too)
MAP = {'Pelvis': 'root hips', 'Torso': 'spine lower', 'shoulders': 'spine upper',
       'Neck': 'head neck lower', 'Head': 'head neck upper', 'Belt': 'root hips', 'JBack': 'root hips',
       'JfUR': 'root hips', 'JfUL': 'root hips',
       'JFLR': 'leg right thigh', 'JBRL': 'leg right thigh', 'JFLL': 'leg left thigh', 'JBLL': 'leg left thigh',
       'Gun': 'arm right wrist'}
for R, s in (('R', 'right'), ('L', 'left')):
    MAP.update({'UpArm-' + R: 'arm %s shoulder 2' % s, 'LoArm-' + R: 'arm %s elbow' % s,
                'Hand-' + R: 'arm %s wrist' % s, 'UpLeg-' + R: 'leg %s thigh' % s,
                'LoLeg-' + R: 'leg %s knee' % s, 'Foot-' + R: 'leg %s ankle' % s, 'Toe-' + R: 'leg %s toes' % s})


def find(d, name):
    low = name.lower()
    for root, _, files in os.walk(d):
        for f in files:
            if f.lower() == low:
                return os.path.join(root, f)
    return None


def lod(skins, skin):
    txt = open(find(skins, skin + '.txt'), encoding='latin1').read()
    m = re.search(r'ExportData\s*=\s*([^;\s]+)\s*;\s*SkinData\s*=\s*([^;\s]+)', txt)
    return m.group(1), m.group(2)


def load(skins, skel, skin, tset, td):
    order, parent, W = F.skeleton(open(skel, 'rb').read())
    kfs, skdn = lod(skins, skin)
    meshes = F.mesh(open(find(skins, kfs), 'rb').read())
    rec, names = F.skd(open(find(skins, skdn), 'rb').read())
    m = meshes[0]
    mats = m['mats']
    if tset:	# a texture set: same materials, other files
        for k, v in F.mesh_materials(open(find(skins, tset), 'rb').read()).items():
            if v:
                mats[k] = v
    # left handed -> right handed: mirror x (faces keep their order: D3D's
    # clockwise fronts come out counter-clockwise)
    S = np.array([-1.0, 1, 1])
    pos = m['P'] * S
    J = {n: W[n][3, :3] * S for n in order}

    def pts(bone):
        bi = names.index(bone)
        best = {}
        for v, b, w in rec:
            if v not in best or w > best[v][1]:
                best[v] = (b, w)
        return pos[[v for v, (b, w) in best.items() if b == bi]]

    X = {}
    for nm, x in MAP.items():
        if nm in ('Pelvis', 'Torso', 'shoulders', 'Neck', 'Head') or nm.startswith(('UpArm', 'LoArm', 'Hand',
                                                                                     'UpLeg', 'LoLeg', 'Foot', 'Toe')):
            X[x] = J[nm]
    sl, su = X['spine lower'], X['spine upper']
    X['belly'] = sl + (su - sl) * 0.45
    for R, s in (('R', 'right'), ('L', 'left')):
        H = pts('Hand-' + R)
        d = X['arm %s wrist' % s] - X['arm %s elbow' % s]
        d /= np.linalg.norm(d)
        t = (H - X['arm %s wrist' % s]) @ d
        top = t.max()
        X['arm %s finger 2a' % s] = X['arm %s wrist' % s] + d * top * 0.55
        X['arm %s finger 2b' % s] = X['arm %s wrist' % s] + d * top * 0.95
        s2 = X['arm %s shoulder 2' % s]
        X['arm %s shoulder 1' % s] = (s2 + su) / 2
    par = {'root hips': None, 'spine lower': 'root hips', 'belly': 'spine lower', 'spine upper': 'belly',
           'head neck lower': 'spine upper', 'head neck upper': 'head neck lower'}
    for s in ('left', 'right'):
        par.update({'arm %s shoulder 1' % s: 'spine upper', 'arm %s shoulder 2' % s: 'arm %s shoulder 1' % s,
                    'arm %s elbow' % s: 'arm %s shoulder 2' % s, 'arm %s wrist' % s: 'arm %s elbow' % s,
                    'arm %s finger 2a' % s: 'arm %s wrist' % s, 'arm %s finger 2b' % s: 'arm %s finger 2a' % s,
                    'leg %s thigh' % s: 'root hips', 'leg %s knee' % s: 'leg %s thigh' % s,
                    'leg %s ankle' % s: 'leg %s knee' % s, 'leg %s toes' % s: 'leg %s ankle' % s})
    olist = list(par)
    ix = {n: i for i, n in enumerate(olist)}
    B = [(n, ix[par[n]] if par[n] else -1, X[n]) for n in olist]

    VW = [dict() for _ in pos]
    for v, b, w in rec:
        tgt = MAP.get(names[b])
        if tgt is None:
            continue
        if names[b] == 'Torso':	# chest: spread up the THUG spine by height
            y = pos[v][1]
            ys = [('spine lower', sl[1]), ('belly', X['belly'][1]), ('spine upper', su[1])]
            k = 0
            if y <= ys[0][1]:
                VW[v][ix['spine lower']] = VW[v].get(ix['spine lower'], 0) + w
                continue
            for (a, ya), (c, yc) in zip(ys, ys[1:]):
                if y <= yc or c == 'spine upper':
                    f = float(np.clip((y - ya) / max(yc - ya, 1e-6), 0, 1))
                    VW[v][ix[a]] = VW[v].get(ix[a], 0) + w * (1 - f)
                    VW[v][ix[c]] = VW[v].get(ix[c], 0) + w * f
                    break
            continue
        VW[v][ix[tgt]] = VW[v].get(ix[tgt], 0) + w
    VW = [list(d.items()) or [(ix['root hips'], 1.0)] for d in VW]

    # textures: per material, the first file
    keys = {}
    for mat, files in mats.items():
        fn = None
        for f in files:
            fn = fn or find(skins, f)
        if fn:
            key = re.sub(r'[^a-z0-9_]+', '_', os.path.splitext(os.path.basename(fn))[0].lower()) + '.png'
            Image.open(fn).convert('RGBA').save(os.path.join(td, key))
            keys[mat] = key
        else:
            print('%s: no texture for %s %s' % (skin, mat, files))
    uvs = [(float(u[0]), 1.0 + float(u[1])) for u in m['UV']]
    tris = []
    for f, uf, mat in zip(m['F'], m['UF'], m['mat']):
        for k in range(1, len(f) - 1):
            tris.append((keys.get(mat), ((f[0], f[0], uf[0]), (f[k], f[k], uf[k]), (f[k + 1], f[k + 1], uf[k + 1]))))
    N = np.zeros_like(pos)
    for _, ((a, _, _), (b2, _, _), (c2, _, _)) in tris:
        fn_ = np.cross(pos[b2] - pos[a], pos[c2] - pos[a])
        N[a] += fn_; N[b2] += fn_; N[c2] += fn_
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
    M, mirrored = frame(B)
    pos = pos @ M.T; N = N @ M.T
    B = [(n, p, x @ M.T) for n, p, x in B]
    floor = pos[:, 1].min()
    pel = dict((b[0], b[2]) for b in B)['root hips']
    shift = np.array([pel[0], floor, pel[2]])
    return [(n, p, x - shift) for n, p, x in B], pos - shift, N, uvs, tris, VW, mirrored


if __name__ == '__main__':
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    data, out, skins, skel = sys.argv[1:5]
    for arg in sys.argv[5:]:
        key, spec = arg.split('=')
        skin, _, tset = spec.partition(':')
        with tempfile.TemporaryDirectory() as td:
            B, pos, N, uvs, tris, VW, mir = load(skins, skel, skin, tset or None, td)
            nv, nt, a, b = build(data, out, key, B, pos, N, uvs, tris, VW, td, 'maxpayne')
            print('%s: %d vertices, %d triangles, height %.1f%s' % (key, nv, nt, b[1], ' MIRRORED' if mir else ''))
