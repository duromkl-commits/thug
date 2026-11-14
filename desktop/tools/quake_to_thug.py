#!/usr/bin/env python3
"""Quake 1 monsters/player (.mdl) -> THUG skater models.

usage: quake_to_thug.py <thug data dir> <out data dir> <palette.lmp> <key>=<mdl>[:<skin index>] ...
       quake_to_thug.py <thug data dir> <out data dir> - <key>=<md2>:<skin .pcx> ...   (Quake 2)

Quake models are vertex animated with no skeleton, so each one gets a rig from a few
hand-placed joints (shoulders, elbows, wrists, neck and crotch heights, read off front and
side views of the standing frame) plus the legs found from the mesh, then every vertex
is given to the bone whose rigid motion through all of the model's animation frames fits
it best. The standing frame is the bind pose."""
import os
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quake_mdl as Q  # noqa: E402
import vertex_anim_rig as V  # noqa: E402
from gtadae_to_thug import build, frame  # noqa: E402

# Quake axes: +x forward, +y left, +z up. R = model's right (-y).
MARKS = {
    'player': dict(R=((-2, -10, 15), (-3, -14, 9), (6, -6, 8)), L=((-2, 10, 15), (-3, 14, 9), (6, 6, 8)), neck=20, crotch=0),
    'soldier': dict(R=((-2, -10, 13), (0, -14, 5), (8, -4, 4)), L=((-2, 10, 13), (0, 14, 5), (8, 4, 4)), neck=18, crotch=-3),
    'enforcer': dict(R=((-4, -12, 24), (-2, -16, 18), (2, -22, 16)), L=((-4, 12, 24), (-2, 16, 18), (6, 16, 14)), neck=28, crotch=-2),
    'knight': dict(R=((0, -9, 12), (0, -12, 5), (8, -8, 1)), L=((0, 9, 12), (0, 11, 5), (6, 11, 0)), neck=16, crotch=-4),
    'hknight': dict(R=((0, -11, 26), (0, -16, 18), (4, -20, 11)), L=((0, 11, 26), (0, 14, 16), (2, 15, 8)), neck=32, crotch=2),
    'ogre': dict(R=((0, -18, 22), (0, -24, 10), (0, -24, -2)), L=((0, 18, 22), (2, 22, 10), (6, 22, -2)), neck=28, crotch=-6,
                 legbox=(-12, 15, -19, 16), propbox=[(-20, 0, -19, -16.8, -15, 0)]),
    'zombie': dict(R=((0, -6, 17), (-1, -7, 9), (-1, -8, 2)), L=((0, 6, 17), (-1, 7, 9), (-1, 8, 2)), neck=21, crotch=4),
    'mummy': dict(R=((0, -6, 17), (-1, -7, 9), (-1, -8, 2)), L=((0, 6, 17), (-1, 7, 9), (-1, 8, 2)), neck=21, crotch=4),
    'shambler': dict(R=((6, -20, 48), (14, -34, 30), (28, -44, 10)), L=((6, 20, 48), (16, 34, 30), (36, 36, 16)), neck=54, crotch=-2),
    'morph': dict(R=((0, -12, 36), (6, -14, 24), (16, -10, 16)), L=((0, 12, 36), (6, 13, 24), (18, 8, 18)), neck=42, crotch=6),
}

# Quake 2 (same axes); 'frame' = the bind frame (the players' stand frames are side-on
# fighting stances, so a frontal taunt/salute frame is used instead)
MARKS2 = {
    'male': dict(frame='taunt02', R=((8, -8, 19), (2, -10, 10), (1, -6, 0)), L=((10, 6, 19), (12, 8, 10), (13, 8, 2)),
                 neck=22, crotch=-1, propbox=[(-4, 3.5, -8, -1, -3, 3)]),
    'female': dict(frame='salute07', R=((6, -7, 21), (4, -8, 13), (3, -9, 5)), L=((8, 4, 21), (10, 14, 20.5), (12, 5, 25)),
                   neck=23, crotch=0),
    'cyborg': dict(frame='salute05', R=((5, -10, 19), (3, -15, 10), (4, -14, 1)), L=((8, 5, 20), (10, 7, 15), (13, 9, 26)),
                   neck=21, crotch=-2, legbox=(-50, 50, -11.5, 10)),
    'soldier': dict(R=((-1, -10, 21), (-4, -12, 13), (5, -7, 7)), L=((1, 7, 21), (5, 9, 13), (8, 5, 6)), neck=25, crotch=0),
    'infantry': dict(R=((-3, -10, 22), (-3, -15, 13), (-2, -18, 4)), L=((-3, 10, 22), (-3, 16, 14), (-2, 20, 5)),
                     neck=25, crotch=3, legbox=(-50, 50, -14, 14)),
    'gunner': dict(R=((0, -12, 28), (0, -14, 17), (0, -14, 3)), L=((0, 11, 29), (0, 17, 17), (0, 18, -2)), neck=34, crotch=10,
                   legbox=(-50, 50, -15, 15), propbox=[(-20, 20, -18, -12.5, 0, 12)]),
    'berserk': dict(R=((0, -10, 26), (0, -12, 15), (0, -12, 4)), L=((0, 12, 27), (0, 19, 14), (0, 19, 2)), neck=30, crotch=8,
                    legbox=(-50, 50, -10.5, 13), propbox=[(-20, 20, -18, -11.5, -9, 9)]),
    'bitch': dict(R=((0, -5, 45), (0, -9, 37), (0, -12, 30)), L=((0, 5, 45), (-3, 10, 38), (-3, 11, 28)), neck=49, crotch=24,
                  propbox=[(-20, 20, 7.5, 15, 20, 40)]),
    'brain': dict(R=((0, -14, 24), (0, -17, 7), (0, -17, -6)), L=((0, 14, 24), (0, 17, 7), (0, 17, -6)), neck=28, crotch=0,
                  legbox=(-10, 8, -14, 14)),
    'tank': dict(R=((-8, -16, 46), (0, -20, 38), (15, -20, 32)), L=((0, 17, 48), (3, 20, 32), (5, 20, 18)), neck=54, crotch=22,
                 propbox=[(-50, 50, 16, 26, 10, 22)]),
    'insane': dict(R=((1, -9, 18), (1, -12, 9), (1, -12, 1)), L=((1, 9, 18), (1, 12, 9), (1, 11, 1)), neck=21, crotch=2,
                   legbox=(-20, 20, -9.5, 9.5)),
}


def marks(name, table=MARKS):
    d = table[name]
    out = {k: v for k, v in d.items() if k not in ('R', 'L', 'frame')}
    for S in 'RL':
        for n, p in zip(('shoulder', 'elbow', 'wrist'), d[S]):
            out['%s %s' % (n, S)] = p
    return out


def load(mdl, pal, skin, td):
    if mdl.lower().endswith('.md2'):	# Quake 2: model named by its folder, skin is a .pcx
        r = Q.read_md2(open(mdl, 'rb').read())
        base = os.path.basename(os.path.dirname(mdl))
        L = marks(base, MARKS2)
        names = [f[0] for f in r['frames']]
        fr = MARKS2[base].get('frame')
        ri = names.index(fr) if fr else next(i for i, n in enumerate(names) if n.lower().startswith(('stand', 'wait')))
        Image.open(skin).convert('RGBA').save(os.path.join(td, 'skin.png'))
    else:
        r = Q.read(open(mdl, 'rb').read(), pal)
        names = [f[0] for f in r['frames']]
        ri = next((i for i, n in enumerate(names) if 'stand' in n.lower() or 'stnd' in n.lower()), 0)
        base = os.path.splitext(os.path.basename(mdl))[0]
        L = marks('morph' if base.startswith('morph') else base)
        Image.fromarray(r['skins'][int(skin or 0)]).convert('RGBA').save(os.path.join(td, 'skin.png'))
    F = np.array([f[1] for f in r['frames']])
    pos = F[ri]
    B, VW, _ = V.rig_landmarks(F, pos, L, tris=[[c[0] for c in t] for t in r['tris']])
    uvs = []; tris = []
    for tri in r['tris']:
        ids = []
        for v, s, t in tri:
            uvs.append((s, 1 - t)); ids.append((v, v, len(uvs) - 1))
        tris.append(('skin.png', (ids[0], ids[2], ids[1])))	# Quake winds clockwise
    N = np.zeros_like(pos)
    for _, ((a, _, _), (b2, _, _), (c2, _, _)) in tris:
        fnrm = np.cross(pos[b2] - pos[a], pos[c2] - pos[a]); N[a] += fnrm; N[b2] += fnrm; N[c2] += fnrm
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
    M, mirrored = frame(B)
    pos = pos @ M.T; N = N @ M.T
    B = [(n, p, x @ M.T) for n, p, x in B]
    floor = pos[:, 1].min()
    pel = dict((b[0], b[2]) for b in B)['root hips']
    shift = np.array([pel[0], floor, pel[2]])
    return [(n, p, x - shift) for n, p, x in B], pos - shift, N, uvs, tris, VW, mirrored


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    data, out, palf = sys.argv[1:4]
    pal = Q.palette(open(palf, 'rb').read()) if palf != '-' else None
    for arg in sys.argv[4:]:
        key, spec = arg.split('=')
        mdl, _, sk = spec.partition(':')
        with tempfile.TemporaryDirectory() as td:
            B, pos, N, uvs, tris, VW, mir = load(mdl, pal, sk, td)
            nv, nt, a, b = build(data, out, key, B, pos, N, uvs, tris, VW, td, 'quake2' if mdl.endswith('.md2') else 'quake')
            print('%s: %d vertices, %d triangles, height %.1f%s' % (key, nv, nt, b[1], ' MIRRORED' if mir else ''))
