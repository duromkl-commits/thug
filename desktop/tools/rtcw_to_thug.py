#!/usr/bin/env python3
"""Return to Castle Wolfenstein characters -> THUG skater bodies.

    rtcw_to_thug.py <thug data dir> <out data dir> <name> <character> <pk3> [<pk3>...]

<character> is the folder under models/players (infantryss, bj2, eva...).
Reads body.mds (skinned, Max Biped bones), head.mdc on the body's tag_head,
and the hat from head_default.skin (md3_hat, on the head's tag_mouth), with
body_default.skin / head_default.skin textures. The body is posed at frame
0 (standing), turned to face +Z and handed to gtadae_to_thug.build, which
scales it, reposes it to THUG's T-pose and fits it to THUG's joints.
Writes models/rtcw/<name>/<name>.skin.xbx, .tex.xbx and .cas.xbx.
"""

import os
import sys
import tempfile
import zipfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rtcw_formats as F  # noqa: E402
from gtadae_to_thug import build, frame  # noqa: E402
from xps_to_thug import inherit_unmapped  # noqa: E402


class Paks:
    """Every pk3 at once, names case-insensitive; later paks win."""
    def __init__(s, paths):
        s.idx = {}
        for p in paths:
            z = zipfile.ZipFile(p)
            for n in z.namelist():
                s.idx[n.lower()] = (z, n)

    def has(s, n):
        return n.lower().replace('\\', '/') in s.idx

    def read(s, n):
        z, real = s.idx[n.lower().replace('\\', '/')]
        return z.read(real)

    def image(s, n):
        """A skin's texture: Quake falls back from .tga to .jpg and back."""
        base = n.lower().replace('\\', '/').rsplit('.', 1)[0]
        for e in ('.tga', '.jpg', '.png'):
            if base + e in s.idx:
                import io
                return Image.open(io.BytesIO(s.read(base + e))).convert('RGBA')
        return None


def skin(paks, path):
    """surface -> texture, plus the md3_* attachments."""
    out = {}
    if not paks.has(path):
        return out
    for ln in paks.read(path).decode('latin-1').splitlines():
        ln = ln.split('//')[0].strip()
        if ',' not in ln:
            continue
        a, b = ln.split(',', 1)
        out[a.strip().lower()] = b.strip().strip('"')
    return out


def pick_skin(paks, d, part):
    """<part>_default.skin, else the plainest variant (base/castle/first; never damaged ones)."""
    if paks.has(d + part + '_default.skin'):
        return d + part + '_default.skin'
    c = sorted(n for n in paks.idx if n.startswith(d + part + '_') and n.endswith('.skin') and '_dam' not in n)
    for pref in ('base1', 'castle1', '1'):
        for n in c:
            if pref in n:
                return n
    return c[0] if c else d + part + '_default.skin'


def biped_name(raw):
    """Max Biped bone -> the XPS GTA name xps_to_thug maps (raw name: folded into its parent)."""
    if raw in ('Bip01', 'Bip02'):
        return 'root ground'
    t = raw.replace('Bip01', ' ').replace('Bip02', ' ').strip().lower().split()
    side = None
    for s, w in (('l', 'left'), ('r', 'right')):
        if s in t:
            side = w; t.remove(s)
    b = ''.join(t)
    plain = {'pelvis': 'root hips', 'spine': 'spine lower', 'spine1': 'belly', 'spine3': 'spine upper',
             'neck': 'head neck lower', 'head': 'head neck upper'}
    if side is None and b in plain:
        return plain[b]
    sided = {'clavicle': 'arm %s shoulder 1', 'upperarm': 'arm %s shoulder 2', 'forearm': 'arm %s elbow',
             'hand': 'arm %s wrist', 'finger2': 'arm %s finger 2a', 'finger21': 'arm %s finger 2b',
             'thigh': 'leg %s thigh', 'calf': 'leg %s knee', 'foot': 'leg %s ankle', 'toe0': 'leg %s toes'}
    if side and b in sided:
        return sided[b] % side
    return raw


def level_axis(ax):
    """A head's axes with the nod taken out: face level, up straight up."""
    f = np.array([ax[0][0], ax[0][1], 0.0]); f /= np.linalg.norm(f)
    u = np.array([0, 0, 1.0])
    return np.array([f, np.cross(u, f), u])


def load(paks, char, frame_no=0):
    d = 'models/players/%s/' % char
    m = F.read(paks.read(d + 'body.mds'))
    T, R = F.pose(m, frame_no)
    raw = [b[0] for b in m['bones']]
    B = [(biped_name(n), p, T[i]) for i, (n, p, _, _) in enumerate(m['bones'])]
    bskin = skin(paks, pick_skin(paks, d, 'body'))
    hskin = skin(paks, pick_skin(paks, d, 'head'))
    pos = []; uvs = []; tris = []; VW = []; textures = {}

    def tex_of(surf, shader, sk):
        t = sk.get(surf.lower()) or shader
        if not t:
            return None
        key = os.path.basename(t.replace('\\', '/')).rsplit('.', 1)[0].lower() + '.png'
        if key not in textures:
            textures[key] = paks.image(t)
        return key if textures[key] is not None else None

    def add(P, ST, TR, weights, tex):
        base = len(pos)
        pos.extend(P); uvs.extend((s, 1.0 - t) for s, t in ST); VW.extend(weights)
        for a, b, c in TR:	# Quake winds clockwise
            tris.append((tex, ((base + a,) * 3, (base + c,) * 3, (base + b,) * 3)))

    for snm, shd, verts, tr in m['surfs']:
        tex = tex_of(snm, shd, bskin)
        P = []; W = []
        for n, uv, ws in verts:
            P.append(sum(w * (T[b] + R[b] @ off) for b, w, off in ws))
            W.append([(b, w) for b, w, off in ws])
        add(P, [v[1] for v in verts], tr, W, tex)

    head_bone = [i for i, n in enumerate(raw) if n.endswith(' Head')][0]
    hfile = next((d + f for f in ('head.mdc', 'head.md3') if paks.has(d + f)), None)
    if 'tag_head' in raw and hfile:
        th = raw.index('tag_head')
        ax = level_axis(R[th])
        hd = paks.read(hfile)
        mdc = hfile.endswith('.mdc')
        for snm, shd, P, N, st, tr in (F.read_mdc if mdc else F.read_md3)(hd):
            if 'blink' in snm.lower():
                continue
            tex = tex_of(snm, shd, hskin)
            add(T[th] + P @ ax, st, tr, [[(head_bone, 1.0)]] * len(P), tex)
        tags = F.mdc_tags(hd) if mdc else F.md3_tags(hd)
        for slot in ('md3_hat', 'md3_hat2', 'md3_hat3'):	# helmets, caps, goggles
            hat = hskin.get(slot)
            if not hat or 'tag_mouth' not in tags:
                continue
            hp = d + hat
            hsk = hskin
            if paks.has(hp + '.skin'):	# "acc/brun1": a skin naming the model (md3_part) and its textures
                hsk = skin(paks, hp + '.skin')
                hp = hp.rsplit('/', 1)[0] + '/' + hsk.get('md3_part', '')
            for cand in (hp, hp.rsplit('.', 1)[0] + '.mdc', 'models/players/' + hat, 'models/players/' + hat.rsplit('.', 1)[0] + '.mdc'):
                if paks.has(cand):
                    o, tax = tags['tag_mouth']
                    origin = T[th] + o @ ax
                    hax = tax @ ax
                    rd = F.read_mdc if cand.lower().endswith('.mdc') else F.read_md3
                    for snm, shd, P, N, st, tr in rd(paks.read(cand)):
                        add(origin + P @ hax, st, tr, [[(head_bone, 1.0)]] * len(P), tex_of(snm, shd, hsk))
                    break
            else:
                print('%s: %s %s not found' % (char, slot, hat))
    pos = np.array(pos, float)
    # normals from the faces
    N = np.zeros_like(pos)
    for tex, ((a, _, _), (b, _, _), (c, _, _)) in tris:
        fn = np.cross(pos[b] - pos[a], pos[c] - pos[a])
        N[a] += fn; N[b] += fn; N[c] += fn
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)

    for side in ('left', 'right'):	# rigs without toe bones (protosoldier): one in front of the ankle
        names = [b[0] for b in B]
        if 'leg %s toes' % side not in names:
            a = names.index('leg %s ankle' % side)
            B.append(('leg %s toes' % side, a, B[a][2] + np.array([6.0, 0, -4.0])))
    inherit_unmapped(B, d)
    M, mirrored = frame(B)
    pos = pos @ M.T; N = N @ M.T
    B = [(n, p, x @ M.T) for n, p, x in B]
    floor = pos[:, 1].min()
    pel = dict((b[0], b[2]) for b in B)['root hips']
    shift = np.array([pel[0], floor, pel[2]])
    pos = pos - shift
    B = [(n, p, x - shift) for n, p, x in B]
    return B, pos, N, uvs, tris, VW, textures, mirrored


def convert(data, out, name, char, paks):
    B, pos, N, uvs, tris, VW, textures, mirrored = load(paks, char)
    with tempfile.TemporaryDirectory() as td:
        for k, im in textures.items():
            if im is not None:
                im.save(os.path.join(td, k))
        return build(data, out, name, B, pos, N, uvs, tris, VW, td, 'rtcw')


if __name__ == '__main__':
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    nv, nt, a, b = convert(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], Paks(sys.argv[5:]))
    print('%s: %d vertices, %d triangles, bbox %s %s' % (sys.argv[3], nv, nt, np.round(a, 1), np.round(b, 1)))
