#!/usr/bin/env python3
"""Garry's Mod player models (Source .mdl on the ValveBiped rig) -> THUG skaters.

usage: gmod_to_thug.py <thug data dir> <out data dir> <addon roots, ':'-separated> <key>=<model .mdl path> ...
  SUB env var = models/<SUB>/ folder (default gmod)

Each ValveBiped joint drives the THUG joint it stands for; helper bones
(attachments, twist/ulna/wrist/bicep helpers, jiggle bones) hand their
weights to their nearest mapped parent. Textures come from the .vmt
$basetexture (.vtf, DXT or raw), searched in every addon root.
"""
import os
import re
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import source_mdl as S  # noqa: E402
from gtadae_to_thug import build, frame  # noqa: E402

VB = 'ValveBiped.Bip01_'
MAP = {'Pelvis': 'root hips', 'Spine': 'spine lower', 'Spine1': 'belly', 'Spine2': 'spine upper',
       'Spine4': 'spine upper', 'Neck1': 'head neck lower', 'Head1': 'head neck upper'}
for R, s in (('R', 'right'), ('L', 'left')):
    MAP.update({R + '_Clavicle': 'arm %s shoulder 1' % s, R + '_UpperArm': 'arm %s shoulder 2' % s,
                R + '_Forearm': 'arm %s elbow' % s, R + '_Hand': 'arm %s wrist' % s,
                R + '_Finger2': 'arm %s finger 2a' % s, R + '_Finger21': 'arm %s finger 2b' % s,
                R + '_Thigh': 'leg %s thigh' % s, R + '_Calf': 'leg %s knee' % s,
                R + '_Foot': 'leg %s ankle' % s, R + '_Toe0': 'leg %s toes' % s})
PAR = {'root hips': None, 'spine lower': 'root hips', 'belly': 'spine lower', 'spine upper': 'belly',
       'head neck lower': 'spine upper', 'head neck upper': 'head neck lower'}
for s in ('left', 'right'):
    PAR.update({'arm %s shoulder 1' % s: 'spine upper', 'arm %s shoulder 2' % s: 'arm %s shoulder 1' % s,
                'arm %s elbow' % s: 'arm %s shoulder 2' % s, 'arm %s wrist' % s: 'arm %s elbow' % s,
                'arm %s finger 2a' % s: 'arm %s wrist' % s, 'arm %s finger 2b' % s: 'arm %s finger 2a' % s,
                'leg %s thigh' % s: 'root hips', 'leg %s knee' % s: 'leg %s thigh' % s,
                'leg %s ankle' % s: 'leg %s knee' % s, 'leg %s toes' % s: 'leg %s ankle' % s})


def target(name):
    if name.startswith(VB):
        return MAP.get(name[len(VB):])
    return None


def load(mdl, roots, td):
    m = S.read_model(mdl)
    bones = m['bones']
    pos = m['P']
    # mapped target of every source bone (helpers -> nearest mapped ancestor)
    tg = []
    for i, (n, p, _) in enumerate(bones):
        t = target(n)
        j = p
        while t is None and j >= 0:
            t = target(bones[j][0]); j = bones[j][1]
        tg.append(t or 'root hips')
    J = {}
    for n, p, x in bones:
        t = target(n)
        if t and (t not in J or n.endswith('Spine2')):
            J[t] = x
    if 'root hips' not in J or 'head neck upper' not in J:
        raise ValueError('no ValveBiped rig')
    if 'spine upper' not in J:
        J['spine upper'] = J.get('head neck lower', J['head neck upper']) * 0.3 + J['root hips'] * 0.7
    if 'spine lower' not in J:
        J['spine lower'] = J['root hips'] + (J['spine upper'] - J['root hips']) * 0.25
    if 'belly' not in J:
        J['belly'] = J['spine lower'] + (J['spine upper'] - J['spine lower']) * 0.45
    if 'head neck lower' not in J:
        J['head neck lower'] = (J['spine upper'] + J['head neck upper']) / 2
    # vertices per mapped bone (for hands/feet tips)
    dom = np.array([max(w, key=lambda t: t[1])[0] if w else 0 for w in m['W']])

    def pts(t):
        ids = [i for i, b in enumerate(tg) if b == t]
        return pos[np.isin(dom, ids)]
    for s in ('left', 'right'):
        if 'arm %s shoulder 1' % s not in J:
            J['arm %s shoulder 1' % s] = (J['arm %s shoulder 2' % s] + J['spine upper']) / 2
        w, e = J['arm %s wrist' % s], J['arm %s elbow' % s]
        dd = (w - e) / np.linalg.norm(w - e)
        H = pts('arm %s wrist' % s)
        if len(H) == 0:
            H = pts('arm %s finger 2a' % s)
        top = max(((H - w) @ dd).max() if len(H) else 0, np.linalg.norm(w - e) * 0.35)
        if 'arm %s finger 2a' % s not in J:
            J['arm %s finger 2a' % s] = w + dd * top * 0.55
        if 'arm %s finger 2b' % s not in J:
            a = J['arm %s finger 2a' % s]
            J['arm %s finger 2b' % s] = a + dd * max(top - (a - w) @ dd, top * 0.4)
        if 'leg %s toes' % s not in J:
            F = pts('leg %s ankle' % s)
            a = J['leg %s ankle' % s]
            if len(F):
                far = F[np.argmax(np.linalg.norm((F - a) * np.array([1, 1, 0.2]), axis=1))]
                J['leg %s toes' % s] = a + (far - a) * 0.7
            else:
                J['leg %s toes' % s] = a + np.array([4.0, 0, -2.0])
    olist = list(PAR)
    ix = {n: i for i, n in enumerate(olist)}
    B = [(n, ix[PAR[n]] if PAR[n] else -1, J[n]) for n in olist]
    VW = []
    for w in m['W']:
        d = {}
        for b, x in w:
            k = ix[tg[b]]
            d[k] = d.get(k, 0) + x
        VW.append(list(d.items()) or [(ix['root hips'], 1.0)])
    # textures
    keys = {}
    for i in range(len(m['materials'])):
        v, alpha = S.find_texture(roots, m['materials'], m['cddirs'], i)
        if not v:
            print('%s: no texture for %s' % (os.path.basename(mdl), m['materials'][i]))
            continue
        try:
            im = S.read_vtf(v)
        except Exception as e:
            print('%s: %s: %s' % (os.path.basename(mdl), v, e))
            continue
        if not alpha:
            im.putalpha(255)
        if max(im.size) > 512:
            k = 512.0 / max(im.size)
            im = im.resize((max(1, int(im.size[0] * k)), max(1, int(im.size[1] * k))))
        key = re.sub(r'[^a-z0-9_]+', '_', os.path.splitext(os.path.basename(v))[0].lower()) + '.png'
        im.save(os.path.join(td, key))
        keys[i] = key
    uvs = [(float(u), 1.0 - float(v)) for u, v in m['UV']]
    N = m['N']
    tris = []
    # Source fronts are clockwise: flip to THUG's counter-clockwise when the
    # winding disagrees with the stored normals
    agree = 0
    for mat, (a, b, c) in m['tris']:
        fn_ = np.cross(pos[b] - pos[a], pos[c] - pos[a])
        agree += np.sign(fn_ @ (N[a] + N[b] + N[c]))
    flip = agree < 0
    for mat, (a, b, c) in m['tris']:
        if flip:
            b, c = c, b
        tris.append((keys.get(mat), ((a, a, a), (b, b, b), (c, c, c))))
    M, mirrored = frame(B)
    if mirrored:
        sw = lambda n: n.replace('left', '#').replace('right', 'left').replace('#', 'right')
        B = [(sw(n), p, x) for n, p, x in B]
        M, _ = frame(B)
    pos = pos @ M.T; N = N @ M.T
    B = [(n, p, x @ M.T) for n, p, x in B]
    floor = pos[:, 1].min()
    pel = dict((b[0], b[2]) for b in B)['root hips']
    shift = np.array([pel[0], floor, pel[2]])
    return [(n, p, x - shift) for n, p, x in B], pos - shift, N, uvs, tris, VW, mirrored


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    data, out, roots = sys.argv[1:4]
    roots = roots.split(':')
    sub = os.environ.get('SUB', 'gmod')
    for arg in sys.argv[4:]:
        key, mdl = arg.split('=', 1)
        with tempfile.TemporaryDirectory() as td:
            try:
                B, pos, N, uvs, tris, VW, mir = load(mdl, roots, td)
                nv, nt, a, b = build(data, out, key, B, pos, N, uvs, tris, VW, td, sub)
                print('%s: %d vertices, %d triangles, height %.1f%s' % (key, nv, nt, b[1], ' MIRRORED' if mir else ''))
            except Exception as e:
                print('%s: FAILED %s' % (key, e))
