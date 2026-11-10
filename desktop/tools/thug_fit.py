"""Last step of every converter that builds skater bodies from other games'
models (xps_to_thug, gtadae_to_thug, gtaobj_to_thug, tpose_to_thug): make
the mesh fit THUG's own skeleton the way a THUG pro's does.

THUG skins every body against its one T-pose skeleton (thps5_human.ske), so
a model whose elbow sits two inches from THUG's elbow bends two inches off,
and long or short arms stay long or short (orangutan arms, tucked-in arms).

1. Warp: each limb segment of the model (its own joints, as its converter
   found them) is stretched onto THUG's matching segment: upper arm,
   forearm, thigh, shin; hands and feet move with their wrist and ankle,
   the head with the neck; the torso is mapped by height between hips and
   shoulders. Thickness is kept: only lengths and positions change.
2. Weights: once on THUG's joints, each vertex takes the bone weights of
   the nearest vertices of THUG's pro skaters (Hawk, Koston, Campbell,
   Vallely), so shoulders, wrists, fingers and twist bones blend like
   theirs. Parts far from any pro body (long coats, hair, hats) keep their
   own weights.
3. Head: everything above THUG's neck joint and within the head's width
   (faces, skull caps, hats, cartoon heads) goes rigid with the head bone,
   so nothing stretches when the head turns.
"""

import os
import sys

import numpy as np
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PRO_SKINS = ('skater_hawk', 'skater_koston', 'skater_campbell', 'skater_vallely')

SIDES = ('L', 'R')
ARM = ['Bone_Bicep_%s', 'Bone_Forearm_%s', 'Bone_Palm_%s']
LEG = ['Bone_Thigh_%s', 'Bone_Knee_%s', 'Bone_Ankle_%s']
ARM_ALL = ARM + ['Bone_Shoulder_%s', 'Bone_Wrist_%s', 'Cloth_Cuff_%s', 'Bone_Fingers_Base_%s', 'Bone_Fingers_Tip_%s',
                 'Bone_Forefinger_Base_%s', 'Bone_Forefinger_Tip_%s', 'Bone_Thumb_%s']
LEG_ALL = LEG + ['Bone_Toe_%s', 'Cloth_Trouser_%s']


def read_skin(fn):
    """Positions and per vertex {bone index: weight} of a THUG .skin.xbx
    (first sector)."""
    from thug_scene import read_scene
    sec = read_scene(open(fn, 'rb').read())['secs'][0]
    W = []
    for w, b in zip(sec['w'], sec['bi']):
        u = [(w & 1023) / 1023.0, ((w >> 11) & 1023) / 1023.0, ((w >> 22) & 511) / 511.0]
        acc = {}
        for k in range(3):
            if u[k] > 0:
                acc[int(b[k])] = acc.get(int(b[k]), 0.0) + u[k]
        W.append(acc)
    return np.array(sec['pos'], float), W


def pro_reference(data):
    """Pooled pro vertices (T-pose) and their weights, from the extracted
    skaterparts next to the skeletons dir; None if they aren't there."""
    base = os.path.join(data, os.pardir, 'skaterparts', 'models', 'skater_male')
    Ps = []; Ws = []
    for n in PRO_SKINS:
        fn = os.path.join(base, n + '.skin.xbx')
        if not os.path.exists(fn):
            continue
        P, W = read_skin(fn)
        Ps.append(P); Ws += W
    if not Ps:
        return None
    return np.vstack(Ps), Ws


def _segment_map(v, a, b, A, B):
    """Points v on segment a->b moved onto A->B: along-bone distance scaled,
    offsets from the bone kept (turned with it)."""
    from xps_to_thug import align
    d = b - a; L = np.linalg.norm(d); dh = d / L
    D = B - A; L2 = np.linalg.norm(D); Dh = D / L2
    u = (v - a) @ dh
    perp = (v - a) - np.outer(u, dh)
    R = align(dh, Dh)
    return A + np.outer(u * (L2 / L), Dh) + perp @ R


def _pw(y, xs, ys):
    """Piecewise linear, slope 1 past both ends."""
    out = np.interp(y, xs, ys)
    lo = y < xs[0]; hi = y > xs[-1]
    out[lo] = ys[0] + (y[lo] - xs[0])
    out[hi] = ys[-1] + (y[hi] - xs[-1])
    return out


def guess_limbs(P, J, tpos):
    """Arm and leg joints measured on the T-posed mesh itself, for models
    whose rig was borrowed from another model (gtaobj_to_thug): the borrowed
    wrist can sit inches up the forearm, and the hand bone then bends half
    the forearm (tilted, dainty hands). Each arm runs from its shoulder joint
    to its fingertips, and its elbow and wrist go where a pro's are along
    that span; knees and ankles likewise between hip and sole. The joint
    sits mid-limb (centre of the mesh around it)."""
    t = {b: np.asarray(p, float) for b, p in tpos.items()}
    J = dict(J)
    for s, sg in (('L', 1), ('R', -1)):
        b0 = np.asarray(J['Bone_Bicep_' + s], float)
        arm = P[(sg * P[:, 0] > sg * b0[0]) & (np.abs(P[:, 1] - b0[1]) < 12)]
        if len(arm) < 10:
            continue
        tip = np.percentile(sg * arm[:, 0], 99.5) * sg
        pro_tip = sg * 37.9
        for b in ('Bone_Forearm_', 'Bone_Palm_'):
            f = (t[b + s][0] - t['Bone_Bicep_' + s][0]) / (pro_tip - t['Bone_Bicep_' + s][0])
            x = b0[0] + f * (tip - b0[0])
            near = arm[np.abs(arm[:, 0] - x) < 1.0]
            yz = near[:, 1:].mean(0) if len(near) else b0[1:]
            J[b + s] = np.array([x, yz[0], yz[1]])
        h0 = np.asarray(J['Bone_Thigh_' + s], float)
        leg = P[(sg * P[:, 0] > 0) & (P[:, 1] < h0[1])]
        if len(leg) < 10:
            continue
        for b in ('Bone_Knee_', 'Bone_Ankle_'):
            y = h0[1] * t[b + s][1] / t['Bone_Thigh_' + s][1]
            near = leg[np.abs(leg[:, 1] - y) < 1.0]
            if len(near):
                J[b + s] = np.array([near[:, 0].mean(), y, near[:, 2].mean()])
    return J


def fit(P, W, J, index, tpos, ref=None, log=None, guess=False):
    """P: (n, 3) T-pose vertices in THUG space (inches). W: per vertex
    {THUG bone index: weight}. J: {THUG bone name: the model's joint}, at
    least the arm, leg, thigh, neck and head joints. Returns new P and W."""
    if guess:
        J = guess_limbs(P, J, tpos)
    name = {i: b for b, i in index.items()}
    t = {b: np.asarray(p, float) for b, p in tpos.items()}
    J = {b: np.asarray(p, float) for b, p in J.items()}
    n = len(P)
    # each vertex's share per region, from its own weights
    groups = {}
    for i, w in enumerate(W):
        for b, x in w.items():
            groups.setdefault(name[b], np.zeros(n))[i] += x
    tot = sum(groups.values())
    tot[tot <= 0] = 1

    def share(*names):
        s = np.zeros(n)
        for g in names:
            if g in groups:
                s += groups[g]
        return s / tot

    # leg joints centred in the mesh's own legs (a rig whose hips are wider
    # or narrower than its trousers left legs splayed or knock-kneed)
    for s_ in SIDES:
        own = share(*[g % s_ for g in LEG_ALL]) > 0.5
        for b in LEG:
            j = J[b % s_]
            near = P[own & (np.abs(P[:, 1] - (j[1] - (3 if b == LEG[0] else 0))) < 1.0)]
            if len(near) >= 6:
                J[b % s_] = np.array([near[:, 0].mean(), j[1], near[:, 2].mean() if b != LEG[0] else j[2]])

    out = np.zeros_like(P)
    done = np.zeros(n)
    # torso (and anything without a better place): height mapped piecewise
    hip = (J['Bone_Thigh_L'][1] + J['Bone_Thigh_R'][1]) / 2
    sh = (J['Bone_Bicep_L'][1] + J['Bone_Bicep_R'][1]) / 2
    th_hip = t['Bone_Thigh_L'][1]; th_sh = t['Bone_Bicep_L'][1]
    T = P.copy()
    T[:, 1] = _pw(P[:, 1], [hip, sh], [th_hip, th_sh])
    cx = (J['Bone_Thigh_L'][0] + J['Bone_Thigh_R'][0]) / 2
    T[:, 0] -= cx
    T[:, 2] += (t['Bone_Chest'][2] - J.get('Bone_Chest', t['Bone_Chest'])[2]) if 'Bone_Chest' in J else 0
    # head: moves with the neck, keeps its size
    hd = share('Bone_Neck', 'Bone_Head', 'Bone_Jaw', 'Bone_Brow', 'Cloth_Hat', 'Bone_PonyTail_1', 'Bone_PonyTail_2')
    H = P + (t['Bone_Neck'] - J['Bone_Neck'])
    def split(sh_, coord, j1, j2, soft=1.0):
        """A limb's share cut into its three segments by where each vertex
        lies along it (x for an arm, height for a leg), not by its weights:
        a borrowed rig's weights put the hand bone on half the forearm."""
        c = coord
        f1 = np.clip((c - j1) / (2 * soft) + 0.5, 0, 1)	# past the first joint
        f2 = np.clip((c - j2) / (2 * soft) + 0.5, 0, 1)
        return sh_ * (1 - f1), sh_ * (f1 - f2), sh_ * f2

    segs = []
    feet = []
    for s, sg in (('L', 1), ('R', -1)):
        a = [J[x % s] for x in ARM]; A = [t[x % s] for x in ARM]
        armsh = share(*[g % s for g in ARM_ALL])
        up, fore, hand = split(armsh, sg * P[:, 0], sg * a[1][0], sg * a[2][0])
        segs.append((up, a[0], a[1], A[0], A[1]))
        segs.append((fore, a[1], a[2], A[1], A[2]))
        segs.append((hand, None, a[2], None, A[2]))
        l = [J[x % s] for x in LEG]; Lt = [t[x % s] for x in LEG]
        legsh = share(*[g % s for g in LEG_ALL])
        th, shin, foot = split(legsh, -P[:, 1], -l[1][1], -l[2][1])
        segs.append((th, l[0], l[1], Lt[0], Lt[1]))
        segs.append((shin, l[1], l[2], Lt[1], Lt[2]))
        segs.append((foot, None, l[2], None, Lt[2]))
        feet.append((s, foot, Lt[2]))
    limb = np.zeros(n)
    for sh_, a, b, A, B in segs:
        if not sh_.any():
            continue
        if a is None:	# hand, foot: carried by their joint
            M = P + (B - b)
        else:
            M = _segment_map(P, a, b, A, B)
        out += sh_[:, None] * M
        limb += sh_
    rest = np.clip(1 - limb - hd, 0, 1)
    out += hd[:, None] * H + rest[:, None] * T
    norm = limb + hd + rest
    out /= norm[:, None]
    Q = out

    # feet: soles levelled (some came out toes-up) and as long as a pro's
    # (some were tiny on the board), turned and scaled about THUG's ankle
    if ref is not None:
        RP, RW = ref
        for s, fs, ank in feet:
            fb = {index[b % s] for b in ('Bone_Ankle_%s', 'Bone_Toe_%s') if b % s in index}
            pf = RP[[i for i, w in enumerate(RW) if w and max(w, key=w.get) in fb]]
            m = fs > 0.5
            if m.sum() < 8 or len(pf) < 8:
                continue
            F = Q[m]
            sole = F[F[:, 1] < F[:, 1].min() + 1.0]
            ang = 0.0
            if len(sole) >= 4 and np.ptp(sole[:, 2]) > 1.0:
                a = np.polyfit(sole[:, 2], sole[:, 1], 1)[0]
                ang = float(np.arctan(a))
                if abs(ang) > np.radians(40):
                    ang = 0.0
            c, s_ = np.cos(ang), np.sin(ang)
            R = np.array([[1, 0, 0], [0, c, -s_], [0, s_, c]])	# about x: y/z plane
            V = (Q - ank) @ R.T
            ln = np.ptp(np.percentile(V[m][:, 2], [2, 98])); pl = np.ptp(np.percentile(pf[:, 2], [2, 98]))
            k = float(np.clip(pl / ln, 0.8, 1.6)) if ln > 1e-3 else 1.0
            V[:, 0] *= k; V[:, 2] *= k
            Q = Q + fs[:, None] * ((V + ank) - Q)
            if log:
                log('fit: foot %s levelled %.0f deg, length %.1f -> %.1f in' % (s, np.degrees(ang), ln, ln * k))

    # weights from the pros
    newW = [dict(w) for w in W]
    if ref is not None:
        RP, RW = ref
        tree = cKDTree(RP)
        d, idx = tree.query(Q, k=6)
        for i in range(n):
            wd = 1.0 / np.maximum(d[i], 0.3) ** 2
            acc = {}
            for j, x in zip(idx[i], wd):
                for b, y in RW[j].items():
                    acc[b] = acc.get(b, 0.0) + x * y
            s = sum(acc.values())
            pro = {b: y / s for b, y in acc.items()}
            a = float(np.clip((d[i, 0] - 5.0) / 5.0, 0, 1))	# 5-10 in off the pros: fade to its own
            if a <= 0:
                newW[i] = pro
            elif a < 1:
                m = {b: (1 - a) * y for b, y in pro.items()}
                for b, y in W[i].items():
                    m[b] = m.get(b, 0.0) + a * y
                newW[i] = m

    # the chest's sides under the armpit stay on the chest: a model wider
    # than the pros had them on the bicep, and they poked out when the arms
    # came down. Anything lower than the T-posed sleeve's underside (sleeve
    # measured 5-7 in out from the shoulder) is torso: its arm weight fades
    # out over 1.5 in above that, as on the pros.
    armb1 = {index[b % x] for x in SIDES for b in ARM_ALL if b % x in index}
    bx, by = t['Bone_Bicep_L'][0], t['Bone_Bicep_L'][1]
    slv = Q[(np.abs(Q[:, 0]) > bx + 4) & (np.abs(Q[:, 0]) < bx + 9) & (np.abs(Q[:, 1] - by) < 8)]
    low = Q[(Q[:, 1] > by - 13) & (Q[:, 1] < by - 7)]
    if len(slv) > 3 and len(low) > 10:
        under = slv[:, 1].min() - 0.5
        moved = 0
        # inside the chest's own width (taken well below the armpit) or the
        # shoulder joint: torso below the armpit
        inner = np.abs(Q[:, 0]) < max(bx + 2.5, np.percentile(np.abs(low[:, 0]), 98) + 1.5)
        near = np.abs(Q[:, 0]) < bx + 4	# the armpit, not the arm further out
        for i in np.where(((near & (Q[:, 1] < under + 1.5)) | (inner & (Q[:, 1] < by - 1))) & (Q[:, 1] > by - 16))[0]:
            w = newW[i]
            if not any(b in armb1 for b in w):
                continue
            f = float(np.clip((Q[i, 1] - under) / 1.5, 0, 1))
            if inner[i]:
                f = min(f, float(np.clip((Q[i, 1] - (by - 4)) / 3, 0, 1)))
            if f >= 1:
                continue
            k = {b: (y * f if b in armb1 else y) for b, y in w.items()}
            if sum(y for b, y in k.items() if b not in armb1) <= 1e-6:
                k[index['Bone_Chest']] = k.get(index['Bone_Chest'], 0.0) + (1 - f)
            sm = sum(k.values())
            newW[i] = {b: y / sm for b, y in k.items() if y > 0}
            moved += 1
        if log:
            log('fit: %d chest-side vertices kept off the arms below %.1f in' % (moved, under))

    # a leg's vertices never follow the other leg (trouser hems stuck to
    # the opposite ankle when the legs part)
    legb = {x: {index[b % x] for b in LEG_ALL if b % x in index} for x in SIDES}
    hy = t['Bone_Thigh_L'][1]
    for i in range(n):
        if Q[i, 1] >= hy or abs(Q[i, 0]) < 0.5:
            continue
        other = legb['R' if Q[i, 0] > 0 else 'L']
        w = newW[i]
        if any(b in other for b in w):
            k = {b: y for b, y in w.items() if b not in other}
            sm = sum(k.values())
            if sm > 0:
                newW[i] = {b: y / sm for b, y in k.items()}

    # head and everything worn on it: rigid with the head
    ih = index['Bone_Head']
    armb = {index[b % x] for x in SIDES for b in ARM_ALL}
    arm = np.array([sum(y for b, y in w.items() if b in armb) for w in newW])
    # the head's width, measured just above the neck (off the arms: a
    # T-pose's arms are under the head, but a big hand can reach that high)
    y0 = t['Bone_Neck'][1]
    band = (Q[:, 1] > y0 + 3) & (Q[:, 1] < y0 + 7) & (arm < 0.5)
    hw = np.percentile(np.abs(Q[band, 0]), 95) * 1.15 if band.sum() > 10 else 6.0
    inside = (np.abs(Q[:, 0]) < hw) & (arm < 0.5)
    lo, hi = y0 - 1.0, y0 + 1.0	# a short blend over the neck joint
    up = inside & (Q[:, 1] >= hi)
    for i in np.where(up)[0]:
        newW[i] = {ih: 1.0}
    for i in np.where(inside & (Q[:, 1] > lo) & (Q[:, 1] < hi))[0]:
        f = (Q[i, 1] - lo) / (hi - lo)
        w = {b: y * (1 - f) for b, y in newW[i].items()}
        w[ih] = w.get(ih, 0.0) + f
        newW[i] = w
    if log:
        log('fit: joints L bicep %s elbow %s palm %s, mesh reach %.1f' % (
            J['Bone_Bicep_L'].round(1), J['Bone_Forearm_L'].round(1), J['Bone_Palm_L'].round(1), P[:, 0].max()))
        log('fit: %d head-rigid, arms %.1f/%.1f in -> %.1f in' % (
            up.sum(), np.linalg.norm(J['Bone_Palm_L'] - J['Bone_Bicep_L']),
            np.linalg.norm(J['Bone_Palm_R'] - J['Bone_Bicep_R']),
            np.linalg.norm(t['Bone_Palm_L'] - t['Bone_Bicep_L'])))
    return Q, newW


# THUG bone -> the GTA joint at the same place
GTA_JOINT = {'Bone_Pelvis': 'root hips', 'Bone_Chest': 'spine upper', 'Bone_Neck': 'head neck lower',
             'Bone_Head': 'head neck upper'}
for _s, _S in (('left', 'L'), ('right', 'R')):
    GTA_JOINT.update({'Bone_Bicep_' + _S: 'arm %s shoulder 2' % _s, 'Bone_Forearm_' + _S: 'arm %s elbow' % _s,
                      'Bone_Palm_' + _S: 'arm %s wrist' % _s, 'Bone_Thigh_' + _S: 'leg %s thigh' % _s,
                      'Bone_Knee_' + _S: 'leg %s knee' % _s, 'Bone_Ankle_' + _S: 'leg %s ankle' % _s,
                      'Bone_Toe_' + _S: 'leg %s toes' % _s})


def joints(bones, posed):
    """THUG bone -> model joint, from a GTA rig reposed by xps_to_thug.repose."""
    names = [b[0] for b in bones]
    return {t: posed[names.index(g)] for t, g in GTA_JOINT.items() if g in names}


def top3(w):
    """{bone: weight} -> three (bone, weight) pairs summing to 1."""
    it = sorted(w.items(), key=lambda kv: -kv[1])[:3]
    s = sum(x for _, x in it) or 1.0
    it = [(b, x / s) for b, x in it]
    return it + [(0, 0.0)] * (3 - len(it))
