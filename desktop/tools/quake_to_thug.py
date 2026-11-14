#!/usr/bin/env python3
"""Quake 1 monsters/player (.mdl) -> THUG skater models.

usage: quake_to_thug.py <thug data dir> <out data dir> <palette.lmp> <key>=<mdl>[:<skin index>] ...

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


def marks(name):
    d = MARKS[name]
    out = {k: v for k, v in d.items() if k not in ('R', 'L')}
    for S in 'RL':
        for n, p in zip(('shoulder', 'elbow', 'wrist'), d[S]):
            out['%s %s' % (n, S)] = p
    return out


def load(mdl, pal, skin, td):
    r = Q.read(open(mdl, 'rb').read(), pal)
    names = [f[0] for f in r['frames']]
    ri = next((i for i, n in enumerate(names) if 'stand' in n.lower() or 'stnd' in n.lower()), 0)
    F = np.array([f[1] for f in r['frames']])
    pos = F[ri]
    base = os.path.splitext(os.path.basename(mdl))[0]
    B, VW, _ = V.rig_landmarks(F, pos, marks('morph' if base.startswith('morph') else base),
                               tris=[[c[0] for c in t] for t in r['tris']])
    Image.fromarray(r['skins'][skin]).convert('RGBA').save(os.path.join(td, 'skin.png'))
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
    pal = Q.palette(open(palf, 'rb').read())
    for arg in sys.argv[4:]:
        key, spec = arg.split('=')
        mdl, _, sk = spec.partition(':')
        with tempfile.TemporaryDirectory() as td:
            B, pos, N, uvs, tris, VW, mir = load(mdl, pal, int(sk or 0), td)
            nv, nt, a, b = build(data, out, key, B, pos, N, uvs, tris, VW, td, 'quake')
            print('%s: %d vertices, %d triangles, height %.1f%s' % (key, nv, nt, b[1], ' MIRRORED' if mir else ''))
