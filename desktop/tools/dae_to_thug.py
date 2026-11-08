#!/usr/bin/env python3
"""THPS2 skaters (Collada .dae exports of the PS1/N64-era rigid-part rigs:
PELVIS, STOMACH, CHEST, HEAD, BICEP/FOREARM/HAND, THIGH/SHIN/SHOE, BOARD and
wheels) -> THUG skater bodies for the desktop port.

    dae_to_thug.py <thug data dir> <out data dir> <name> <model.dae>

Writes models/thps2/<name>/<name>.skin.xbx, .tex.xbx and .cas.xbx under <out
data dir>. Same pipeline as xps_to_thug.py (shared writers): scaled to a THUG
pro's height, re-posed so each limb points the way its THUG bone does, each
part's vertices weighted 100% to the THUG bone it stands for. The board, its
trucks and wheels are left out (THUG skates its own board).
"""

import os
import struct
import sys
import xml.etree.ElementTree as ET

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xps_to_thug import crc, read_skeleton, align, pack_weights, write_skin, write_tex  # noqa: E402

NS = {'c': 'http://www.collada.org/2005/11/COLLADASchema'}

# THPS2 part (name after the last 'PART_xxx_') -> THUG bone. None: dropped.
PARTS = {
    'PELVIS': 'Bone_Pelvis', 'STOMACH': 'Bone_Stomach_Upper', 'CHEST': 'Bone_Chest', 'HEAD': 'Bone_Head',
    'BOARD': None, 'FRONT_WHEEL': None, 'BACK_WHEEL': None, 'ROOT': 'Bone_Pelvis',
}
for s, S in (('LEFT', 'L'), ('RIGHT', 'R')):
    PARTS.update({
        s + '_BICEP': 'Bone_Bicep_' + S, s + '_FOREARM': 'Bone_Forearm_' + S, s + '_HAND': 'Bone_Palm_' + S,
        s + '_THIGH': 'Bone_Thigh_' + S, s + '_SHIN': 'Bone_Knee_' + S, s + '_SHOE': 'Bone_Ankle_' + S,
    })

# Part whose segment (its joint to the child's joint) is aimed along a THUG
# segment: (part, child part, THUG from, THUG to). Hands and shoes have no
# child joint: they keep their parent's turn, then get the THUG direction of
# palm->fingers / ankle->toe by their own extent (see AIM_EXTENT).
AIM = []
AIM_EXTENT = []
SNAP = set()
for s, S in (('LEFT', 'L'), ('RIGHT', 'R')):
    AIM += [
        (s + '_BICEP', s + '_FOREARM', 'Bone_Bicep_' + S, 'Bone_Forearm_' + S),
        (s + '_FOREARM', s + '_HAND', 'Bone_Forearm_' + S, 'Bone_Palm_' + S),
        (s + '_THIGH', s + '_SHIN', 'Bone_Thigh_' + S, 'Bone_Knee_' + S),
        (s + '_SHIN', s + '_SHOE', 'Bone_Knee_' + S, 'Bone_Ankle_' + S),
    ]
    AIM_EXTENT += [(s + '_HAND', 'Bone_Palm_' + S, 'Bone_Fingers_Base_' + S)]
    SNAP |= {s + '_BICEP', s + '_FOREARM', s + '_HAND'}


def part_name(joint):
    j = joint.upper()
    for k in sorted(PARTS, key=len, reverse=True):
        if j.endswith('_' + k) or j == k:
            return k
    return None


def floats(el):
    return np.array([float(x) for x in el.text.split()])


def read_dae(fn):
    t = ET.parse(fn).getroot()
    up = (t.findtext('.//c:asset/c:up_axis', default='Y_UP', namespaces=NS) or 'Y_UP').strip()
    # Z up -> THUG's Y up (x, y, z) -> (x, z, -y)
    if up == 'Z_UP':
        AX = np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], float)	# row vectors: v @ AX
    else:
        AX = np.eye(3)

    # images, effects, materials -> texture file per material id
    img = {i.get('id'): i.findtext('c:init_from', namespaces=NS) for i in t.findall('.//c:image', NS)}
    eff_img = {}
    for e in t.findall('.//c:effect', NS):
        surf = {}
        for p in e.findall('.//c:newparam', NS):
            s = p.find('c:surface/c:init_from', NS)
            if s is not None:
                surf[p.get('sid')] = s.text
            s2 = p.find('c:sampler2D/c:source', NS)
            if s2 is not None:
                surf[p.get('sid')] = surf.get(s2.text, s2.text)
        tx = e.find('.//c:diffuse/c:texture', NS)
        if tx is not None:
            k = tx.get('texture')
            eff_img[e.get('id')] = img.get(surf.get(k, k), img.get(k))
    mat_tex = {}
    for m in t.findall('.//c:material', NS):
        ie = m.find('c:instance_effect', NS)
        if ie is not None:
            mat_tex[m.get('id')] = eff_img.get(ie.get('url', '#')[1:])
    # instance_material symbol -> material id
    sym = {im.get('symbol'): im.get('target', '#')[1:] for im in t.findall('.//c:instance_material', NS)}

    # joints: world matrices from the node tree (column-vector Collada matrices)
    world = {}
    parent = {}

    def walk(n, M, par):
        for c in n.findall('c:node', NS):
            mt = c.find('c:matrix', NS)
            L = floats(mt).reshape(4, 4) if mt is not None else np.eye(4)
            W = M @ L
            if c.get('type') == 'JOINT':
                world[c.get('sid') or c.get('name')] = W
                parent[c.get('sid') or c.get('name')] = par
                walk(c, W, c.get('sid') or c.get('name'))
            else:
                walk(c, W, par)
    walk(t.find('.//c:visual_scene', NS), np.eye(4), None)

    sk = t.find('.//c:skin', NS)
    BSM = floats(sk.find('c:bind_shape_matrix', NS)).reshape(4, 4)
    srcs = {s.get('id'): s for s in sk.findall('c:source', NS)}
    jn = sk.find('c:joints', NS)
    jsrc = [i.get('source')[1:] for i in jn.findall('c:input', NS) if i.get('semantic') == 'JOINT'][0]
    names = srcs[jsrc].find('c:Name_array', NS).text.split()
    vw = sk.find('c:vertex_weights', NS)
    ins = {i.get('semantic'): (int(i.get('offset')), i.get('source')[1:]) for i in vw.findall('c:input', NS)}
    wts = floats(srcs[ins['WEIGHT'][1]].find('c:float_array', NS))
    vc = [int(x) for x in vw.findtext('c:vcount', namespaces=NS).split()]
    v = [int(x) for x in vw.findtext('c:v', namespaces=NS).split()]
    stride = len(ins)
    vjoint = []
    p = 0
    for n in vc:
        best = None
        for k in range(n):
            ji = v[p + ins['JOINT'][0]]; w = wts[v[p + ins['WEIGHT'][0]]]
            if best is None or w > best[1]:
                best = (names[ji], w)
            p += stride
        vjoint.append(best[0] if best else None)

    mesh = t.find('.//c:geometry/c:mesh', NS)
    src = {s.get('id'): floats(s.find('c:float_array', NS)) for s in mesh.findall('c:source', NS)}
    vin = mesh.find('c:vertices', NS)
    pos_id = [i.get('source')[1:] for i in vin.findall('c:input', NS) if i.get('semantic') == 'POSITION'][0]
    POS = src[pos_id].reshape(-1, 3)
    POS = (np.c_[POS, np.ones(len(POS))] @ BSM.T)[:, :3]
    groups = []
    for tri in mesh.findall('c:triangles', NS):
        ii = {i.get('semantic'): (int(i.get('offset')), i.get('source')[1:]) for i in tri.findall('c:input', NS)}
        st = max(o for o, _ in ii.values()) + 1
        idx = np.array([int(x) for x in tri.findtext('c:p', namespaces=NS).split()]).reshape(-1, st)
        NRM = src[ii['NORMAL'][1]].reshape(-1, 3) if 'NORMAL' in ii else None
        UV = src[ii['TEXCOORD'][1]].reshape(-1, 2) if 'TEXCOORD' in ii else None
        tex = mat_tex.get(sym.get(tri.get('material'), tri.get('material')))
        corners = []
        for r in idx:
            vi = r[ii['VERTEX'][0]]
            n = NRM[r[ii['NORMAL'][0]]] if NRM is not None else np.array([0, 0, 1.0])
            uv = UV[r[ii['TEXCOORD'][0]]] if UV is not None else np.zeros(2)
            corners.append((vi, n, uv))
        groups.append((tri.get('material'), tex, corners))
    joints = {j: (W[:3, 3] @ AX, parent[j]) for j, W in world.items()}
    return joints, POS @ AX, vjoint, groups, AX


def convert(data, out, name, dae_fn, sub='thps2'):
    index, tpos = read_skeleton(os.path.join(data, 'skeletons', 'thps5_human.ske.xbx'))
    joints, POS, vjoint, groups, AX = read_dae(dae_fn)
    part_of = {j: part_name(j) for j in joints}
    unknown = [j for j, p in part_of.items() if p is None]
    if unknown:
        raise ValueError('%s: joints not mapped: %s' % (dae_fn, unknown))
    by_part = {p: j for j, p in part_of.items()}

    # Height without the board: top of the head over the lowest shoe vertex.
    keep = [i for i, j in enumerate(vjoint) if j and PARTS[part_of[j]]]
    lo = POS[keep, 1].min(); hi = POS[keep, 1].max()
    scale = 72.6 / (hi - lo)
    off = np.array([0, -lo, 0])
    J = {j: (p + off) * scale for j, (p, _) in joints.items()}
    P0 = (POS + off) * scale

    # Re-pose: FK from the pelvis; each AIM part turned onto its THUG segment.
    aim = {a: (b, tf, tt) for a, b, tf, tt in AIM}
    ext = {a: (tf, tt) for a, tf, tt in AIM_EXTENT}
    R = {}; posed = {}
    order = []

    def visit(j):
        order.append(j)
        for k, (_, par) in joints.items():
            if par == j:
                visit(k)
    for j, (_, par) in joints.items():
        if par is None:
            visit(j)
    for j in order:
        par = joints[j][1]
        if par is None:
            R[j] = np.eye(3); posed[j] = J[j]; continue
        posed[j] = posed[par] + (J[j] - J[par]) @ R[par]
        p = part_of[j]
        if p in SNAP:
            # rigid part: moved onto THUG's joint so elbows and wrists bend
            # where the animations expect (THPS2 arms run ~3 in longer)
            posed[j] = tpos[PARTS[p]]
        if p in aim and aim[p][0] in by_part:
            c = by_part[aim[p][0]]
            R[j] = align(J[c] - J[j], tpos[aim[p][2]] - tpos[aim[p][1]])
        elif p in ext:
            # direction: from the joint to the far end of the part's vertices
            vs = P0[[i for i, jj in enumerate(vjoint) if jj == j]]
            if len(vs):
                far = vs[np.argmax(np.linalg.norm(vs - J[j], axis=1))]
                R[j] = align(far - J[j], tpos[ext[p][1]] - tpos[ext[p][0]])
            else:
                R[j] = R[par]
        else:
            R[j] = R[par]

    P = []; N = []; UV = []; W = []; BI = []
    out_meshes = []; textures = {}
    tex_dir = os.path.dirname(dae_fn)
    for gi, (mname, tex, corners) in enumerate(groups):
        tris = []
        for k in range(0, len(corners), 3):
            tri = corners[k:k + 3]
            js = [vjoint[c[0]] for c in tri]
            if any(j is None or PARTS[part_of[j]] is None for j in js):
                continue	# board, trucks, wheels
            ids = []
            for (vi, n, uv), j in zip(tri, js):
                q = (P0[vi] - J[j]) @ R[j] + posed[j]
                nq = (np.asarray(n) @ AX) @ R[j]
                P.append(q); N.append(nq / max(1e-6, np.linalg.norm(nq)))
                UV.append((uv[0], 1.0 - uv[1]))
                W.append(pack_weights([1.0, 0.0, 0.0]))
                BI.append([index[PARTS[part_of[j]]], 0, 0, 0])
                ids.append(len(P) - 1)
            tris.append((ids[0], ids[1], ids[2]))	# Collada winds CCW like THUG
        if not tris:
            continue
        tname = os.path.basename(tex) if tex else None
        tc = crc('%s/%s/%s' % (sub, name, tname or 'none'))
        if tname and tc not in textures:
            textures[tc] = Image.open(os.path.join(tex_dir, tname))
        out_meshes.append((crc('%s_%s_mat%d' % (sub, name, gi)), tc, tris))
    P = np.array(P); N = np.array(N); UV = np.array(UV)
    d = os.path.join(out, 'models', sub, name)
    os.makedirs(d, exist_ok=True)
    write_skin(os.path.join(d, name + '.skin.xbx'), crc(sub + '_' + name), P, N, UV, W, BI, out_meshes)
    write_tex(os.path.join(d, name + '.tex.xbx'), list(textures.items()))
    open(os.path.join(d, name + '.cas.xbx'), 'wb').write(struct.pack('<3I', 2, 0, 0))
    return len(P), sum(len(m[2]) for m in out_meshes), P.min(0), P.max(0)


if __name__ == '__main__':
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    nv, nt, a, b = convert(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
    print('%s: %d vertices, %d triangles, bbox %s %s' % (sys.argv[3], nv, nt, np.round(a, 1), np.round(b, 1)))
