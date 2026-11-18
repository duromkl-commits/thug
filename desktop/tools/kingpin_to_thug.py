#!/usr/bin/env python3
"""Kingpin: Life of Crime cast -> THUG skater bodies.

    kingpin_to_thug.py <thug data dir> <out data dir> <actors dir> <cast.json>

Kingpin characters are a body type (models/actors/<type>/{head,body,legs}.mdx,
vertex-animated, no skeleton) plus three skin numbers from the map
("art_skins" head body legs -> head_NNN / body_NNN / legs_NNN .tga), a head
variant and accessory bits. Frame 0 (standing) of each part is used.

The models are built of rigid pieces (MDX sub-objects): torso, upper arm,
forearm, hand per side in body.mdx; pelvis, thigh, shin, foot per side in
legs.mdx. The rig comes from them: each joint sits where two pieces meet,
and each piece moves wholly with its bone (the torso blends down the spine).
Then gtadae_to_thug.build reposes it to THUG's T-pose and fits it.

punk / shorty / whore have no textures of their own: they use thug / runt /
bitch's; runt legs use thug's. Writes models/kingpin/<key>/.
"""
import json
import os
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kingpin_formats as K  # noqa: E402
from gtadae_to_thug import build, frame  # noqa: E402

TEX = {'punk': 'thug', 'shorty': 'runt', 'whore': 'bitch', 'thug': 'thug', 'runt': 'runt', 'bitch': 'bitch'}
# "head" key: 1 bald, 2 ski mask (women: ponytail), 3 welder's mask (order in gamex86.dll)
HEADS = {'1': 'bald_head', '2': 'ski_head', '3': 'weld_head'}
HEADS_F = {'1': 'bald_head', '2': 'pony_head'}
# "acc" key: bit flags, read off gamex86.dll: 1 cigar, 2 fedora, 4 stetson, 8 cap
ACC = ((1, 'cigar'), (2, 'fedora'), (4, 'stetson'), (8, 'cap'))


def find(d, name):
    low = {f.lower(): f for f in os.listdir(d)} if os.path.isdir(d) else {}
    f = low.get(name.lower())
    return os.path.join(d, f) if f else None


def texture(actors, typ, part, num):
    for fo in (TEX[typ], 'thug'):
        f = find(os.path.join(actors, fo), '%s_%s.tga' % (part, num))
        if f:
            return f
    return None


def end(X, top, frac=0.2):
    """Centroid of a piece's top (or bottom) slice along z."""
    z = X[:, 2]
    lo, hi = z.min(), z.max()
    sel = z >= hi - frac * (hi - lo) if top else z <= lo + frac * (hi - lo)
    return X[sel].mean(0)


def rig(P, piece):
    """Joints (xps names, Quake coords) and per-vertex weights from the pieces.
    piece[i] = (part, sub-object)."""
    def pts(part, so):
        return P[[i for i, p in enumerate(piece) if p == (part, so)]]
    def side_of(part, so):
        return 'left' if pts(part, so)[:, 1].mean() > 0 else 'right'
    J = {}
    seg = {}		# (part, so) -> bone name
    for part, chain in (('body', ('upper', 'fore', 'hand')), ('legs', ('thigh', 'shin', 'foot'))):
        for first in (1, 4):
            sd = side_of(part, first)
            A, B, C = (pts(part, first + k) for k in range(3))
            if part == 'body':
                J['arm %s shoulder 2' % sd] = end(A, True, 0.15)
                J['arm %s elbow' % sd] = (end(A, False) + end(B, True)) / 2
                J['arm %s wrist' % sd] = (end(B, False) + end(C, True)) / 2
                J['arm %s finger 2a' % sd] = end(C, False, 0.3)
                J['arm %s finger 2b' % sd] = end(C, False, 0.05)
                names = ('arm %s shoulder 2', 'arm %s elbow', 'arm %s wrist')
            else:
                J['leg %s thigh' % sd] = end(A, True, 0.15)
                J['leg %s knee' % sd] = (end(A, False) + end(B, True)) / 2
                ank = (end(B, False) + end(C, True, 0.4)) / 2
                J['leg %s ankle' % sd] = ank
                toe = C[C[:, 0] >= np.percentile(C[:, 0], 85)].mean(0)
                J['leg %s toes' % sd] = np.array([toe[0], toe[1], C[:, 2].min() + 0.3 * (ank[2] - C[:, 2].min())])
                names = ('leg %s thigh', 'leg %s knee', 'leg %s ankle')
            for k in range(3):
                seg[(part, first + k)] = names[k] % sd
    T = pts('body', 0)
    hip = (J['leg left thigh'] + J['leg right thigh']) / 2
    sh = (J['arm left shoulder 2'] + J['arm right shoulder 2']) / 2
    neck = end(T, True, 0.08)
    J['root hips'] = hip
    J['spine lower'] = hip + (sh - hip) * 0.2
    J['belly'] = hip + (sh - hip) * 0.45
    J['spine upper'] = hip + (sh - hip) * 0.7
    J['head neck lower'] = neck
    H = np.array([P[i] for i, p in enumerate(piece) if p[0] == 'head'])
    J['head neck upper'] = end(H, False, 0.25)
    for sd in ('left', 'right'):
        s2 = J['arm %s shoulder 2' % sd]
        J['arm %s shoulder 1' % sd] = np.array([s2[0], (s2[1] + neck[1]) / 2, (s2[2] + neck[2]) / 2])
    par = {'root hips': None, 'spine lower': 'root hips', 'belly': 'spine lower', 'spine upper': 'belly',
           'head neck lower': 'spine upper', 'head neck upper': 'head neck lower'}
    for sd in ('left', 'right'):
        par.update({'arm %s shoulder 1' % sd: 'spine upper', 'arm %s shoulder 2' % sd: 'arm %s shoulder 1' % sd,
                    'arm %s elbow' % sd: 'arm %s shoulder 2' % sd, 'arm %s wrist' % sd: 'arm %s elbow' % sd,
                    'arm %s finger 2a' % sd: 'arm %s wrist' % sd, 'arm %s finger 2b' % sd: 'arm %s finger 2a' % sd,
                    'leg %s thigh' % sd: 'root hips', 'leg %s knee' % sd: 'leg %s thigh' % sd,
                    'leg %s ankle' % sd: 'leg %s knee' % sd, 'leg %s toes' % sd: 'leg %s ankle' % sd})
    order = list(par)
    B = [(n, order.index(par[n]) if par[n] else -1, J[n]) for n in order]
    ix = {n: i for i, n in enumerate(order)}
    spine = [('root hips', hip[2]), ('spine lower', J['spine lower'][2]), ('belly', J['belly'][2]),
             ('spine upper', J['spine upper'][2]), ('head neck lower', neck[2])]
    VW = []
    for i, p in enumerate(piece):
        if p[0] == 'head':
            VW.append([(ix['head neck upper'], 1.0)])
        elif p in seg:
            VW.append([(ix[seg[p]], 1.0)])
        elif p == ('legs', 0):
            VW.append([(ix['root hips'], 1.0)])
        else:	# torso: between the two spine joints it lies between
            z = P[i][2]
            w = None
            for (a, za), (b, zb) in zip(spine, spine[1:]):
                if z <= zb:
                    t = float(np.clip((z - za) / max(zb - za, 1e-6), 0, 1))
                    w = [(ix[a], 1 - t), (ix[b], t)]
                    break
            VW.append(w or [(ix['head neck lower'], 1.0)])
    return B, VW


def load(actors, c, td):
    typ = c['type']
    heads = HEADS_F if typ in ('bitch', 'whore') else HEADS
    acc = int(c['acc'] or 0)
    parts = [(heads.get(c['head'], 'head'), 'head', c['skins'][0]), ('body', 'body', c['skins'][1]),
             ('legs', 'legs', c['skins'][2])] + [(m, None, None) for bit, m in ACC if acc & bit]
    pos = []; uvs = []; tris = []; piece = []
    for mdx, part, num in parts:
        fn = find(os.path.join(actors, typ), mdx + '.mdx')
        if not fn and part == 'head':	# runts have one head
            fn = find(os.path.join(actors, typ), 'head.mdx')
        if not fn:
            print('%s: no %s/%s.mdx' % (c['key'], typ, mdx)); continue
        d = open(fn, 'rb').read()
        if part:
            tex = texture(actors, typ, part, num)
        else:	# hats, cigar: their own skin, under models/props
            sk = os.path.basename(K.skins(d)[0])
            tex = None
            for dd in (os.path.join(actors, typ), os.path.join(actors, '..', 'props', mdx)):
                tex = tex or find(dd, sk)
        key = None
        if tex:
            key = '%s_%s.png' % (part or mdx, num or '1')
            Image.open(tex).convert('RGBA').save(os.path.join(td, key))
        else:
            print('%s: no texture for %s' % (c['key'], mdx))
        P, tr, sub = K.read(d, 0)
        b = len(pos)
        pos.extend(P)
        piece.extend((part or 'head', int(s)) for s in sub)
        for tri in tr:
            ids = []
            for vi, s, t in tri:
                uvs.append((s, 1 - t)); ids.append((b + vi, b + vi, len(uvs) - 1))
            tris.append((key, (ids[0], ids[2], ids[1])))	# Quake winds clockwise
    pos = np.array(pos)
    B, VW = rig(pos, piece)
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
    data, out, actors, cast = sys.argv[1:5]
    only = os.environ.get('ONLY')
    for c in json.load(open(cast)):
        if only and c['key'] not in only.split(','):
            continue
        with tempfile.TemporaryDirectory() as td:
            B, pos, N, uvs, tris, VW, mir = load(actors, c, td)
            nv, nt, a, b = build(data, out, c['key'], B, pos, N, uvs, tris, VW, td, 'kingpin')
            print('%s: %d vertices, %d triangles, height %.1f%s' % (c['key'], nv, nt, b[1], ' MIRRORED' if mir else ''))
