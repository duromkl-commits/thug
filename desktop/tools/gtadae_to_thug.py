#!/usr/bin/env python3
"""GTA San Andreas peds exported from Blender as rigged Collada (.dae, the
game's own DFF skeleton) -> THUG skater bodies for the desktop port.

    gtadae_to_thug.py <thug data dir> <out data dir> <name> <model.dae> [sub]

Writes models/<sub, default gta>/<name>/<name>.skin.xbx, .tex.xbx and .cas.xbx.
Same path as xps_to_thug.py: the DFF bones are renamed to the XPS names that
file maps, the mesh is turned to face +Z (worked out from the rig: the toes
say which way is forward, so no model ends up backwards whatever axes it was
exported with), scaled to a pro's height and reposed from GTA's A-pose to
THUG's T-pose by the same FK.
"""

import os
import struct
import sys
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thug_fit  # noqa: E402
from xps_to_thug import (crc, read_skeleton, repose, pack_weights, write_skin,  # noqa: E402
                         write_tex, BONE_MAP)

NS = '{http://www.collada.org/2005/11/COLLADASchema}'


def fl(e):
    return np.array([float(x) for x in e.text.split()]) if e is not None and e.text else np.zeros(0)


def it(e):
    return np.array([int(x) for x in e.text.split()]) if e is not None and e.text else np.zeros(0, int)


def xps_name(raw, spine_rank):
    """DFF bone name -> the XPS GTA name xps_to_thug maps (None: unknown)."""
    t = raw.replace('Bip01', ' ').strip().lower().split()
    side = None
    for s, w in (('l', 'left'), ('r', 'right')):
        if s in t:
            side = w; t.remove(s)
    b = ''.join(t)
    if b.startswith('spine'):
        return 'spine lower' if spine_rank == 0 else 'spine upper'
    plain = {'root': 'root ground', 'normal': 'root ground', 'pelvis': 'root hips', 'belly': 'belly',
             'neck': 'head neck lower', 'head': 'head neck upper', 'jaw': 'head jaw'}
    if b in plain and side is None:
        return plain[b]
    sided = {'clavicle': 'arm %s shoulder 1', 'upperarm': 'arm %s shoulder 2', 'forearm': 'arm %s elbow',
             'hand': 'arm %s wrist', 'finger': 'arm %s finger 2a', 'finger01': 'arm %s finger 2b',
             'thigh': 'leg %s thigh', 'calf': 'leg %s knee', 'foot': 'leg %s ankle', 'toe0': 'leg %s toes',
             'brow': 'head eyebrow %s 1', 'breast': 'breast %s base'}
    if b in sided and side:
        return sided[b] % side
    return None


def read_dae(fn):
    root = ET.parse(fn).getroot()
    ids = {e.get('id'): e for e in root.iter() if e.get('id')}

    # textures: material -> effect -> image file
    def mat_tex(mid):
        m = ids.get(mid)
        if m is None:
            return None
        eff = ids.get(m.find(NS + 'instance_effect').get('url')[1:])
        for t in eff.iter(NS + 'texture'):
            samp = t.get('texture')
            img = None
            for p in eff.iter(NS + 'newparam'):
                if p.get('sid') == samp:
                    src = p.find('.//' + NS + 'source').text
                    for p2 in eff.iter(NS + 'newparam'):
                        if p2.get('sid') == src:
                            img = p2.find('.//' + NS + 'init_from').text
            if img is None:
                img = samp
            ie = ids.get(img)
            if ie is not None:
                return ie.find(NS + 'init_from').text
        return None

    ctrl = root.find('.//' + NS + 'controller')
    skin = ctrl.find(NS + 'skin')
    geom = ids[skin.get('source')[1:]]
    bsm = fl(skin.find(NS + 'bind_shape_matrix')).reshape(4, 4)

    def src(e, sem):
        for i in e.findall(NS + 'input'):
            if i.get('semantic') == sem:
                return ids[i.get('source')[1:]]

    joints_e = skin.find(NS + 'joints')
    jn = src(joints_e, 'JOINT').find(NS + 'Name_array').text.split()
    ibm = fl(src(joints_e, 'INV_BIND_MATRIX').find(NS + 'float_array')).reshape(-1, 4, 4)
    vw = skin.find(NS + 'vertex_weights')
    wts = fl(src(vw, 'WEIGHT').find(NS + 'float_array'))
    offs = {i.get('semantic'): int(i.get('offset')) for i in vw.findall(NS + 'input')}
    vcount = it(vw.find(NS + 'vcount')); v = it(vw.find(NS + 'v')); stride = len(offs)
    VW = []; k = 0
    for c in vcount:
        VW.append([(v[(k + j) * stride + offs['JOINT']], wts[v[(k + j) * stride + offs['WEIGHT']]]) for j in range(c)])
        k += c

    mesh = geom.find(NS + 'mesh')
    verts = mesh.find(NS + 'vertices')
    pos = fl(src(verts, 'POSITION').find(NS + 'float_array')).reshape(-1, 3)
    pos = (np.c_[pos, np.ones(len(pos))] @ bsm.T)[:, :3]
    tris = []	# (material texture, [(vi, ni, ti) x3])
    norms = uvs = None
    for prim in list(mesh.findall(NS + 'triangles')) + list(mesh.findall(NS + 'polylist')):
        ins = {i.get('semantic'): (int(i.get('offset')), i.get('source')[1:]) for i in prim.findall(NS + 'input')}
        st = max(o for o, _ in ins.values()) + 1
        p = it(prim.find(NS + 'p')).reshape(-1, st)
        if 'NORMAL' in ins:
            norms = fl(ids[ins['NORMAL'][1]].find(NS + 'float_array')).reshape(-1, 3)
        if 'TEXCOORD' in ins:
            se = ids[ins['TEXCOORD'][1]]
            uvs = fl(se.find(NS + 'float_array')).reshape(-1, int(se.find('.//' + NS + 'accessor').get('stride')))[:, :2]
        if prim.tag.endswith('polylist'):
            vc = it(prim.find(NS + 'vcount'))
        else:
            vc = np.full(len(p) // 3, 3)
        tex = mat_tex(prim.get('material'))
        # material symbols in instance_material can rename the material
        if tex is None:
            for im in root.iter(NS + 'instance_material'):
                if im.get('symbol') == prim.get('material'):
                    tex = mat_tex(im.get('target')[1:])
        k = 0
        for c in vc:
            cs = [(p[k + j][ins['VERTEX'][0]], p[k + j][ins['NORMAL'][0]] if 'NORMAL' in ins else -1,
                   p[k + j][ins['TEXCOORD'][0]] if 'TEXCOORD' in ins else -1) for j in range(c)]
            for j in range(1, c - 1):
                tris.append((tex, (cs[0], cs[j], cs[j + 1])))
            k += c

    # skeleton: JOINT nodes in hierarchy order, world bind positions from the
    # inverse bind matrices (same space as the mesh)
    bones = []; spine = [0]

    def walk(n, parent):
        me = parent
        if n.get('type') == 'JOINT':
            raw = n.get('name')
            nm = xps_name(raw, spine[0])
            if nm and nm.startswith('spine'):
                spine[0] += 1
            sid = n.get('sid') or n.get('name')
            j = jn.index(sid) if sid in jn else (jn.index(n.get('name')) if n.get('name') in jn else -1)
            bones.append([nm, parent, j, raw])
            me = len(bones) - 1
        for c in n.findall(NS + 'node'):
            walk(c, me)
    vs = root.find('.//' + NS + 'visual_scene')
    for n in vs.findall(NS + 'node'):
        walk(n, -1)
    # nodes outside the skin get their parent's position (rarely used)
    out = []
    for nm, par, j, raw in bones:
        if j >= 0:
            p = np.linalg.inv(ibm[j])[:3, 3]
        else:
            p = out[par][2] if par >= 0 else np.zeros(3)
        out.append((nm, par, p, raw))
    jmap = {}
    for i, b in enumerate(out):
        if bones[i][2] >= 0:
            jmap[bones[i][2]] = i
    return out, pos, norms, uvs, tris, VW, jmap


def frame(bones):
    """Rows: the model's left, up, forward (THUG +X, +Y, +Z)."""
    P = {b[0]: b[2] for b in bones if b[0]}
    feet = (P['leg left ankle'] + P['leg right ankle']) / 2
    up = P['head neck upper'] - feet; up /= np.linalg.norm(up)
    toes = (P['leg left toes'] + P['leg right toes']) / 2 - feet
    fwd = toes - up * (toes @ up); fwd /= np.linalg.norm(fwd)
    left = np.cross(up, fwd)
    lab = P['arm left shoulder 2'] - P['arm right shoulder 2']
    mirrored = lab @ left < 0	# the rig's "left" is on the model's right
    return np.array([left, up, fwd]), mirrored


def load(dae_fn):
    """The rigged model in THUG's frame (+Z forward, +X left, y up), feet on
    y = 0, pelvis over the origin, still in the file's units and A-pose.
    Returns (bones [(xps name, parent, joint)], positions, normals, uvs,
    triangles [(texture, corners (v, n, t) x3)], per-vertex [(bone, weight)],
    texture dir, whether the rig's sides were swapped)."""
    bones, pos, norms, uvs, tris, VW, jmap = read_dae(dae_fn)
    unknown = [b[3] for b in bones if b[0] is None]
    if unknown:
        raise ValueError('%s: bones not mapped: %s' % (dae_fn, unknown))
    M, mirrored = frame(bones)
    if mirrored:
        # swap the sides' names so THUG's left arm drives the model's left arm
        sw = lambda n: n.replace('left', '#').replace('right', 'left').replace('#', 'right')
        bones = [(sw(n), p, x, r) for n, p, x, r in bones]
        M, _ = frame(bones)
    pos = pos @ M.T
    norms = norms @ M.T if norms is not None else None
    B = [(n, p, x @ M.T) for n, p, x, r in bones]
    floor = pos[:, 1].min()
    pel = dict((b[0], b[2]) for b in B)['root hips']
    shift = np.array([pel[0], floor, pel[2]])
    pos = pos - shift
    B = [(n, p, x - shift) for n, p, x in B]
    VW = [[(jmap[j], w) for j, w in vw if j in jmap and w > 0] for vw in VW]
    return B, pos, norms, uvs, tris, VW, os.path.dirname(dae_fn), mirrored


def convert(data, out, name, dae_fn, sub='gta'):
    B, pos, norms, uvs, tris, VW, tex_dir, mirrored = load(dae_fn)
    r = build(data, out, name, B, pos, norms, uvs, tris, VW, tex_dir, sub)
    return r + (mirrored,)


def build(data, out, name, B, pos, norms, uvs, tris, VW, tex_dir, sub='gta', guess=False):
    """Scale to a pro's height, repose to THUG's T-pose and write the files."""
    index, tpos = read_skeleton(os.path.join(data, 'skeletons', 'thps5_human.ske.xbx'))
    scale = 72.6 / pos[:, 1].max()
    R, rest, posed = repose(B, tpos, scale)

    P = []; N = []; UV = []; W = []; BI = []
    remap = {}; meshes = {}; textures = {}
    for tex, tri in tris:
        ids = []
        for vi, ni, ti in tri:
            key = (vi, ni, ti)
            if key not in remap:
                p = pos[vi] * scale
                nr = norms[ni] if ni >= 0 else np.array([0, 0, 1.0])
                qv = np.zeros(3); nq = np.zeros(3); tot = 0.0; per = {}
                for b, w in VW[vi]:
                    qv += w * ((p - rest[b]) @ R[b] + posed[b])
                    nq += w * (nr @ R[b])
                    tot += w
                    t = index[BONE_MAP[B[b][0]]]
                    per[t] = per.get(t, 0.0) + w
                if tot <= 0:
                    qv = p; nq = nr; per = {index['Bone_Pelvis']: 1.0}; tot = 1.0
                qv /= tot; nq /= max(1e-6, np.linalg.norm(nq))
                remap[key] = len(P)
                P.append(qv); N.append(nq)
                UV.append((uvs[ti][0], 1.0 - uvs[ti][1]) if ti >= 0 else (0.0, 0.0))
                W.append({b: w / tot for b, w in per.items()})
            ids.append(remap[key])
        meshes.setdefault(tex, []).append(tuple(ids))	# Collada winds CCW like THUG
    if len(P) > 65535:
        raise ValueError('%s: %d vertices, over THUG\'s 16-bit limit' % (name, len(P)))
    out_meshes = []
    for mi, (tex, tl) in enumerate(meshes.items()):
        tname = os.path.basename(tex.replace('\\', '/')) if tex else None
        if tname and not os.path.exists(os.path.join(tex_dir, tname)):
            low = {f.lower(): f for f in os.listdir(tex_dir)}
            tname = low.get(tname.lower(), tname)
        tc = crc('%s/%s/%s' % (sub, name, tname or 'none'))
        if tname and tc not in textures:
            textures[tc] = Image.open(os.path.join(tex_dir, tname))
        out_meshes.append((crc('%s_%s_mat%d' % (sub, name, mi)), tc, tl))
    P = np.array(P); N = np.array(N); UV = np.array(UV)
    P, W = thug_fit.fit(P, W, thug_fit.joints(B, posed), index, tpos,
                        thug_fit.pro_reference(data), log=print, guess=guess)
    BI = []
    for i, w in enumerate(W):
        t3 = thug_fit.top3(w)
        W[i] = pack_weights([x for _, x in t3]); BI.append([b for b, _ in t3] + [0])
    convert.last = (P, UV, out_meshes, textures)	# for previews
    d = os.path.join(out, 'models', sub, name)
    os.makedirs(d, exist_ok=True)
    write_skin(os.path.join(d, name + '.skin.xbx'), crc(sub + '_' + name), P, N, UV, W, BI, out_meshes)
    write_tex(os.path.join(d, name + '.tex.xbx'), list(textures.items()))
    open(os.path.join(d, name + '.cas.xbx'), 'wb').write(struct.pack('<3I', 2, 0, 0))
    return len(P), sum(len(m[2]) for m in out_meshes), P.min(0), P.max(0)


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    nv, nt, a, b, mir = convert(*sys.argv[1:6])
    print('%s: %d vertices, %d triangles, bbox %s %s%s' % (sys.argv[3], nv, nt, np.round(a, 1), np.round(b, 1),
                                                         ' (rig sides swapped)' if mir else ''))
