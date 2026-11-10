#!/usr/bin/env python3
"""Red Dead Revolver (PS2) characters -> THUG skater bodies for the desktop port.

    rdr_to_thug.py <thug data dir> <out data dir> <SWAT.DAT> <code> <name> [nohat]

Writes models/rdr/<name>/<name>.skin.xbx, .tex.xbx and .cas.xbx under <out
data dir>. Same pipeline as xps_to_thug.py / dae_to_thug.py (shared writers):
the model is turned to face +Z, scaled so the top of the head (hat left out)
is a THUG pro's 72.6 in, and reposed by FK from Red Dead's arms-down pose to
THUG's T-pose, each limb aimed along its THUG bone and the arm joints moved
onto THUG's so elbows and wrists bend where the animations expect. Red Dead
vertices hang off one bone, or two ("reskin") at joints; both carry over.
"""

import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xps_to_thug import crc, read_skeleton, align, pack_weights, write_skin, write_tex  # noqa: E402
import thug_fit  # noqa: E402
import rdr_formats as RF  # noqa: E402

FLIP = np.array([-1.0, 1.0, -1.0])	# Red Dead faces -Z; turned round, its _l side lands on THUG's _L (+x)


def thug_bone(nm, x):
    """Red Dead bone -> [(THUG bone, share)]. x: joint x after the turn."""
    S = 'L' if nm.endswith('_l') else 'R'
    if nm in ('root', 'pelvis'): return [('Bone_Pelvis', 1)]
    if nm == 'spine_01': return [('Bone_Stomach_Upper', 1)]
    if nm == 'spine_02': return [('Bone_Chest', 1)]
    if nm == 'neck_01': return [('Bone_Neck', 1)]
    if nm == 'head': return [('Bone_Head', 1)]
    if nm.startswith('clavicle'): return [('Bone_Collar_' + S, 1)]
    if nm.startswith('shoulder'): return [('Bone_Bicep_' + S, 1)]
    if nm.startswith('elbow'): return [('Bone_Forearm_' + S, 1)]
    if nm.startswith('wrist'): return [('Bone_Palm_' + S, 1)]
    if nm.startswith('thumb'): return [('Bone_Thumb_' + S, 1)]
    if nm.startswith('finger'):
        return [('Bone_Fingers_Base_' + S if nm[-3] == '1' else 'Bone_Fingers_Tip_' + S, 1)]
    if nm.startswith('hip'): return [('Bone_Thigh_' + S, 1)]
    if nm.startswith('knee'): return [('Bone_Knee_' + S, 1)]
    if nm.startswith(('ankle', 'toe')): return [('Bone_Ankle_' + S if nm.startswith('ankle') else 'Bone_Toe_' + S, 1)]
    if nm.startswith('larm'):	# Daren's cannon arm
        return [('Bone_Bicep_L' if nm == 'larm01' else 'Bone_Forearm_L', 1)]
    if nm == 'tail_root': return [('Bone_Pelvis', 1)]
    if nm.startswith('t0'):	# coat tails: half the hips, half the thigh on their side
        if abs(x) < 0.05: return [('Bone_Pelvis', 1)]
        return [('Bone_Pelvis', 0.5), ('Bone_Thigh_' + ('L' if x > 0 else 'R'), 0.5)]
    return None


def aims(names):
    """Red Dead bone -> (child joint it aims with, THUG from, THUG to)."""
    a = {}
    for s, S in (('l', 'L'), ('r', 'R')):
        a.update({
            'shoulder_' + s: ('elbow_' + s, 'Bone_Bicep_' + S, 'Bone_Forearm_' + S),
            'elbow_' + s: ('wrist_' + s, 'Bone_Forearm_' + S, 'Bone_Palm_' + S),
            'wrist_' + s: ('finger_21_' + s, 'Bone_Palm_' + S, 'Bone_Fingers_Base_' + S),
            'hip_' + s: ('knee_' + s, 'Bone_Thigh_' + S, 'Bone_Knee_' + S),
            'knee_' + s: ('ankle_' + s, 'Bone_Knee_' + S, 'Bone_Ankle_' + S),
            'ankle_' + s: ('toe_' + s, 'Bone_Ankle_' + S, 'Bone_Toe_' + S),
        })
    return {k: v for k, v in a.items() if k in names and v[0] in names}


SNAP = {'shoulder_l': 'Bone_Bicep_L', 'elbow_l': 'Bone_Forearm_L', 'wrist_l': 'Bone_Palm_L',
        'shoulder_r': 'Bone_Bicep_R', 'elbow_r': 'Bone_Forearm_R', 'wrist_r': 'Bone_Palm_R'}


def convert(data, out, dat, code, name, hat=True, sub='rdr'):
    index, tpos = read_skeleton(os.path.join(data, 'skeletons', 'thps5_human.ske.xbx'))
    dave = RF.Dave(dat)
    names, parent, J0, VB, UV, NR, TRI, TEX = RF.character(dave, code, hat)

    # Height: top of the body and face (hats left out) -> 72.6 in.
    def world(lst):
        return sum(w * (J0[b] + l) for b, w, l in lst)
    hat_free = RF.character(dave, code, False)[3]
    top = max(world(v)[1] for v in hat_free)
    scale = 72.6 / top
    J = J0 * FLIP * scale

    # FK repose
    aim = aims(names)
    R = [None] * len(names); posed = [None] * len(names)
    for i, nm in enumerate(names):
        p = parent[i]
        if p < 0:
            R[i] = np.eye(3); posed[i] = J[i]; continue
        posed[i] = posed[p] + (J[i] - J[p]) @ R[p]
        if nm in SNAP and not names[p].startswith('larm'):
            posed[i] = tpos[SNAP[nm]]
        if nm in aim:
            c, tf, tt = aim[nm]
            R[i] = align(J[names.index(c)] - J[i], tpos[tt] - tpos[tf])
        else:
            R[i] = R[p]

    maps = {}
    for i, nm in enumerate(names):
        m = thug_bone(nm, J[i][0]); k = i
        while m is None:	# unknown bone (props: Mr. Black's coffin...): its nearest known ancestor's
            k = parent[k]
            m = thug_bone(names[k], J[k][0]) if k >= 0 else [('Bone_Pelvis', 1)]
        maps[i] = [(index[b], s) for b, s in m]

    P = []; N = []; WD = []
    for lst, nrm in zip(VB, NR):
        q = np.zeros(3); nq = np.zeros(3); per = {}
        for b, w, loc in lst:
            q += w * ((loc * FLIP * scale) @ R[b] + posed[b])
            nq += w * ((nrm * FLIP) @ R[b])
            for t, s in maps[b]:
                per[t] = per.get(t, 0.0) + w * s
        P.append(q); N.append(nq / max(1e-6, np.linalg.norm(nq))); WD.append(per)
    P = np.array(P); N = np.array(N)
    lift = P[:, 1].min(); P[:, 1] -= lift	# soles on the ground
    # same limb fit as the GTA rips: legs under the hips, feet levelled and
    # pro-sized, the pros' weights, no trouser hem on the other ankle
    J = {}
    for i, nm in enumerate(names):
        m = thug_bone(nm, J0[i][0])
        if m and len(m) == 1 and m[0][0] not in J and not nm.startswith(('larm', 't0', 'tail', 'finger', 'thumb')):
            J[m[0][0]] = posed[i] - [0, lift, 0]
    if all(b % s_ in J for s_ in 'LR' for b in thug_fit.ARM + thug_fit.LEG) and 'Bone_Neck' in J:
        P, WD = thug_fit.fit(P, WD, J, index, tpos, thug_fit.pro_reference(data), log=print)
    else:
        print('fit: skipped, missing joints')
    W = []; BI = []
    for w in WD:
        t3 = thug_fit.top3(w)
        W.append(pack_weights([x for _, x in t3])); BI.append([t for t, _ in t3] + [0])

    meshes = []; textures = {}
    for tn in dict.fromkeys(TEX):
        tris = [t for t, x in zip(TRI, TEX) if x == tn]
        tc = crc('%s/%s/%s' % (sub, name, tn or 'none'))
        if tn and tn + '.tex' in dave and tc not in textures:
            textures[tc] = RF.read_tex(dave.get(tn + '.tex'))
        meshes.append((crc('%s_%s_%s' % (sub, name, tn)), tc, tris))
    if len(P) > 65535:
        raise ValueError('%s: too many vertices' % code)
    d = os.path.join(out, 'models', sub, name)
    os.makedirs(d, exist_ok=True)
    write_skin(os.path.join(d, name + '.skin.xbx'), crc(sub + '_' + name), P, N, UV, W, BI, meshes)
    write_tex(os.path.join(d, name + '.tex.xbx'), list(textures.items()))
    open(os.path.join(d, name + '.cas.xbx'), 'wb').write(struct.pack('<3I', 2, 0, 0))
    return len(P), len(TRI), P.min(0), P.max(0)


if __name__ == '__main__':
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    nv, nt, a, b = convert(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5],
                           hat=not (len(sys.argv) > 6 and sys.argv[6] == 'nohat'))
    print('%s: %d vertices, %d triangles, bbox %s %s' % (sys.argv[5], nv, nt, np.round(a, 1), np.round(b, 1)))
