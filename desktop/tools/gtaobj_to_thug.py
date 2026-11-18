#!/usr/bin/env python3
"""GTA San Andreas peds with no rig (plain .obj + .mtl rips) -> THUG skater
bodies for the desktop port.

    gtaobj_to_thug.py <thug data dir> <out data dir> <name> <model.obj> <reference.dae>...

The rig is borrowed: every GTA SA ped is built on the same skeleton in the
same A-pose, so the .obj is turned upright and to face +Z (whichever of the
24 axis turns makes it lie closest over the rigged references, so a model
exported lying down or facing away still comes out facing forward), the
closest reference by shape is picked, and each vertex takes the bone weights
of the reference vertices nearest it. The reference's joints, stretched to
the model's height and width, become the model's; from there it's the same
repose and writers as gtadae_to_thug.py.
"""

import itertools
import os
import sys

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components, dijkstra
from scipy.spatial import cKDTree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gtadae_to_thug as dae  # noqa: E402
from xps_to_thug import align  # noqa: E402


def load_obj(fn):
    V = []; T = []; N = []; F = []
    mtl = {}; cur = None
    d = os.path.dirname(fn)
    for line in open(fn, errors='replace'):
        s = line.split()
        if not s:
            continue
        if s[0] == 'v':
            V.append([float(x) for x in s[1:4]])
        elif s[0] == 'vt':
            T.append([float(x) for x in s[1:3]])
        elif s[0] == 'vn':
            N.append([float(x) for x in s[1:4]])
        elif s[0] == 'mtllib':
            m = os.path.join(d, line.split(None, 1)[1].strip())
            if os.path.exists(m):
                name = None
                for l2 in open(m, errors='replace'):
                    t = l2.split(None, 1)
                    if not t:
                        continue
                    if t[0] == 'newmtl':
                        name = t[1].strip()
                    elif t[0].lower() == 'map_kd' and name:
                        mtl[name] = os.path.basename(t[1].strip().replace('\\', '/'))
        elif s[0] == 'usemtl':
            cur = line.split(None, 1)[1].strip()
        elif s[0] == 'f':
            c = [tuple(int(x) - 1 if x else -1 for x in (k.split('/') + ['', ''])[:3]) for k in s[1:]]
            c = [(v, n, t) for v, t, n in c]	# OBJ is v/vt/vn; build() takes (v, n, t)
            for k in range(1, len(c) - 1):
                F.append((cur, (c[0], c[k], c[k + 1])))
    low = {f.lower(): f for f in os.listdir(d)}
    pngs = [f for f in os.listdir(d) if f.lower().endswith('.png')]
    stem = os.path.splitext(os.path.basename(fn))[0].lower()

    def tex(m):
        # usemtl names often don't match the .mtl's (exporters rename them):
        # then the .mtl's only map, else the .png named after the model
        t = mtl.get(m)
        if t is None and len(set(mtl.values())) == 1:
            t = list(mtl.values())[0]
        if t is None or t.lower() not in low:
            t = next((p for p in pngs if p.lower().startswith(stem)), None) or (pngs[0] if len(pngs) == 1 else t)
        return low.get((t or '').lower(), t)
    tris = [(tex(m), t) for m, t in F]
    return np.array(V), np.array(T), np.array(N) if N else None, tris


def turns():
    """The 24 proper rotations that map axes onto axes."""
    out = []
    for perm in itertools.permutations(range(3)):
        for signs in itertools.product((1, -1), repeat=3):
            M = np.zeros((3, 3))
            for i, (p, s) in enumerate(zip(perm, signs)):
                M[i, p] = s
            if np.linalg.det(M) > 0:
                out.append(M)
    return out


def normalise(P):
    """Feet on 0, height 1, chest centred: returns (points, shift, height)."""
    floor = P[:, 1].min(); h = P[:, 1].max() - floor
    Q = (P - [0, floor, 0]) / h
    # x: halfway between the hands (the A-pose arms are the widest part and
    # symmetric; a badge or holster pulls a median off), z: the chest's median
    band = Q[(Q[:, 1] > 0.3) & (Q[:, 1] < 0.8)]
    chest = Q[(Q[:, 1] > 0.6) & (Q[:, 1] < 0.75)]
    c = np.array([(np.percentile(band[:, 0], 99.5) + np.percentile(band[:, 0], 0.5)) / 2, 0,
                  np.median(chest[:, 2])])
    return Q - c, np.array([0, floor, 0]) + c * h, h


def chamfer(A, B, ta=None, tb=None):
    ta = ta or cKDTree(A); tb = tb or cKDTree(B)
    return tb.query(A)[0].mean() + ta.query(B)[0].mean()


def fit(V, refs):
    """Best turn and reference for the .obj vertices."""
    best = None
    # OBJ_UP=x|y|z forces which model axis is up (shapes too far from a GTA
    # ped for the fit to tell, like cartoon characters)
    up = os.environ.get('OBJ_UP')
    for M in turns():
        if up and M[1, 'xyz'.index(up)] != 1:
            continue
        Q, _, _ = normalise(V @ M.T)
        tq = cKDTree(Q)
        for k, r in enumerate(refs[:3]):		# turn chosen against a few refs
            d = chamfer(Q, r['Q'], tq, r['tree'])
            if best is None or d < best[0]:
                best = (d, M)
    M = best[1]
    Q, shift, h = normalise(V @ M.T)
    # front and back can lie about as close over a reference; the feet
    # settle it: toes stick out further ahead of the ankles than heels behind
    f = Q[Q[:, 1] < 0.03]; a = Q[(Q[:, 1] > 0.06) & (Q[:, 1] < 0.12)]
    if len(a) == 0:		# low-poly legs: no vertex that low up the shin
        a = Q[(Q[:, 1] > 0.04) & (Q[:, 1] < 0.25)]
    if (f[:, 2].max() - a[:, 2].max()) + (f[:, 2].min() - a[:, 2].min()) < 0:
        M = np.diag([-1.0, 1, -1]) @ M
        Q, shift, h = normalise(V @ M.T)
    tq = cKDTree(Q)
    scores = sorted((chamfer(Q, r['Q'], tq, r['tree']), k) for k, r in enumerate(refs))
    return M, Q, shift, h, scores


def ref_entry(fn):
    B, pos, norms, uvs, tris, VW, tex_dir, _ = dae.load(fn)
    Q, shift, h = normalise(pos)
    J = [(n, p, (x - shift) / h) for n, p, x in B]
    return {'fn': fn, 'Q': Q, 'tree': cKDTree(Q), 'B': J, 'VW': VW}


ARM = ('arm %s shoulder 2', 'arm %s elbow', 'arm %s wrist', 'arm %s finger 2a', 'arm %s finger 2b')


def transfer(ref, Rq, Q):
    """Per model vertex: the bone weights of the 4 nearest reference vertices."""
    d, idx = cKDTree(Rq).query(Q, k=4)
    w = 1.0 / np.maximum(d, 1e-4) ** 2
    VW = []
    for i in range(len(Q)):
        acc = {}
        for j, wj in zip(idx[i], w[i]):
            for b, bw in ref['VW'][j]:
                acc[b] = acc.get(b, 0.0) + wj * bw
        t = sum(acc.values())
        VW.append([(b, x / t) for b, x in acc.items()] if t > 0 else [])
    return VW


def arm_weight(VW, bones, side):
    ids = {i for i, b in enumerate(bones) if b[0] in [a % side for a in ARM]}
    return np.array([sum(w for b, w in vw if b in ids) for vw in VW])


def tip(X, sg):
    """The hand's far end: mean of the points past the 98th percentile out
    (one stray vertex mustn't swing a whole arm)."""
    o = sg * X[:, 0]
    return X[o >= np.percentile(o, 98)].mean(0)


def adapt(ref, Q):
    """Bend the reference onto the model: torso widened or narrowed to the
    model's, each arm turned and stretched about its shoulder so its hand
    lands on the model's (GTA rips differ in how far the A-pose arms hang).
    Returns the reference's vertices and joints moved."""
    P = ref['Q'].copy()
    J = [np.array(b[2]) for b in ref['B']]
    names = [b[0] for b in ref['B']]
    arm = {s: arm_weight(ref['VW'], ref['B'], s) for s in ('left', 'right')}
    body = 1 - np.clip(arm['left'] + arm['right'], 0, 1)
    jarm = {s: np.array([1.0 if n in [a % s for a in ARM] else 0.0 for n in names]) for s in arm}
    for it in range(2):
        # torso width, at the belly (under the armpits, above the hands)
        VW = transfer(ref, P, Q)
        oa = arm_weight(VW, ref['B'], 'left') + arm_weight(VW, ref['B'], 'right')
        band = lambda X: (X[:, 1] > 0.5) & (X[:, 1] < 0.62)
        r_hw = np.percentile(np.abs(P[band(P) & (body > 0.95), 0]), 95)
        o_sel = band(Q) & (oa < 0.05)
        if o_sel.sum() > 10:
            k = np.percentile(np.abs(Q[o_sel, 0]), 95) / r_hw
            k = float(np.clip(k, 0.8, 1.5))
            P[:, 0] *= 1 + (k - 1) * body
            for i, n in enumerate(names):
                if not (n and n.startswith('arm')):
                    J[i] = J[i] * [k, 1, 1]
            # shoulders move out with the torso; arms ride along
            for sd in arm:
                S = J[names.index('arm %s shoulder 1' % sd)]
                dx = S[0] * (k - 1) / k
                P[:, 0] += arm[sd] * dx
                for i in np.where(jarm[sd] > 0)[0]:
                    J[i] = J[i] + [dx, 0, 0]
                J[names.index('arm %s shoulder 1' % sd)] = S
        # arms: hand tip to the model's
        for sd, sg in (('left', 1), ('right', -1)):
            S = J[names.index('arm %s shoulder 2' % sd)]
            ra = arm[sd] > 0.5
            rt = tip(P[ra], sg)
            ot = tip(Q[(Q[:, 1] > 0.25) & (Q[:, 1] < S[1]) & (sg * Q[:, 0] > 0)], sg)
            Rm = align(rt - S, ot - S)
            sc = np.linalg.norm(ot - S) / np.linalg.norm(rt - S)
            moved = (P - S) @ Rm * sc + S
            P = P + arm[sd][:, None] * (moved - P)
            for i in np.where(jarm[sd] > 0)[0]:
                J[i] = (J[i] - S) @ Rm * sc + S
    return P, [(b[0], b[1], j) for b, j in zip(ref['B'], J)]


def segdist(p, lines):
    best = np.inf
    for a, b in lines:
        t = np.clip(np.dot(p - a, b - a) / max(1e-12, np.dot(b - a, b - a)), 0, 1)
        best = min(best, np.linalg.norm(a + t * (b - a) - p))
    return best


def segments(VW, names, weld, graph, Q, B):
    """Old segmented models (GTA III, Half-Life: every limb part its own
    piece) move each piece wholly with the arm or wholly with the body, so
    the torso doesn't stretch with the arms like a web; a body piece mostly
    on one bone goes rigid with it. The main skin (over half the model) is
    left alone. Returns the arm pieces' vertices."""
    _, lab = connected_components(graph, directed=False)
    lab = lab[weld]
    arm = {i for i, n in enumerate(names) if n in [a % sd for a in ARM for sd in ('left', 'right')]}
    protect = np.zeros(len(VW), bool)
    J = {b[0]: np.array(b[2]) for b in B}
    line = lambda *c: [(J[c[i]], J[c[i + 1]]) for i in range(len(c) - 1) if c[i] in J and c[i + 1] in J]
    arm_lines = []; body_lines = line('root hips', 'spine upper', 'head neck lower', 'head neck upper')
    for sd in ('left', 'right'):
        arm_lines += line(*[a % sd for a in ARM])
        body_lines += line('leg %s thigh' % sd, 'leg %s knee' % sd, 'leg %s ankle' % sd, 'leg %s toes' % sd)
    # each arm mirrored too: one side's chain can be fitted off (a hand
    # holding something), the model's two hands still hang alike
    arm_lines += [(a * [-1, 1, 1], b * [-1, 1, 1]) for a, b in arm_lines]
    sx = np.mean([abs(b[2][0]) for b in B if b[0] in ('arm left shoulder 2', 'arm right shoulder 2')])
    for c in np.unique(lab):
        ids = np.where(lab == c)[0]
        if len(ids) > len(VW) / 2:
            continue
        mass = {}
        for i in ids:
            for b, w in VW[i]:
                mass[b] = mass.get(b, 0.0) + w
        tot = sum(mass.values())
        if tot <= 0:
            continue
        top = max(mass, key=mass.get)
        af = sum(w for b, w in mass.items() if b in arm) / tot
        # where the piece lies, whatever bones the A-pose hand hanging next
        # to it lent it: out past the shoulders and nearer the arm's bones
        # than the spine's or a leg's (a hand, a sleeve), or not (a belt, a
        # coat flap, a chest plate)
        cen = Q[ids].mean(0)
        outside = abs(cen[0]) >= 0.8 * sx and segdist(cen, arm_lines) < segdist(cen, body_lines)
        if outside:
            # arm piece: arm bones only (still bending at the elbow), and the
            # hem test below leaves it alone
            protect[ids] = True
            for i in ids:
                keep = [(b, w) for b, w in VW[i] if b in arm]
                t = sum(w for _, w in keep)
                if t > 0:
                    VW[i] = [(b, w / t) for b, w in keep]
                else:	# a vertex the arm lent nothing: the piece's main arm bone
                    VW[i] = [(names.index('arm %s wrist' % ('left' if cen[0] > 0 else 'right')), 1.0)]
            continue
        # body piece: no arm bones, and rigid to its bone if it mostly has one
        for i in ids:
            keep = [(b, w) for b, w in VW[i] if b not in arm]
            t = sum(w for _, w in keep)
            VW[i] = [(b, w / t) for b, w in keep] if t > 0 else [(names.index('root hips'), 1.0)]
        m2 = {b: w for b, w in mass.items() if b not in arm}
        if m2:
            t2 = max(m2, key=m2.get)
            if m2[t2] / sum(m2.values()) >= 0.6:
                for i in ids:
                    VW[i] = [(t2, 1.0)]
    return protect


def rebind_arms(VW, names, B, Q, protect):
    """Arm pieces' weights from where they lie along the arm (upper arm,
    forearm, hand, blended a little across each joint), not from the
    reference's nearest vertices, which round a stiff GTA III sleeve hanging
    off the shoulder give to the collarbone."""
    for sd, sg in (('left', 1), ('right', -1)):
        chain = ['arm %s shoulder 2' % sd, 'arm %s elbow' % sd, 'arm %s wrist' % sd, 'arm %s finger 2b' % sd]
        if not all(c in names for c in chain):
            continue
        bi = [names.index(c) for c in chain]
        J = [np.array(B[i][2]) for i in bi]
        sel = np.where(protect & (sg * Q[:, 0] > 0))[0]
        for v in sel:
            p = Q[v]
            # position along the chain, in segments (0-1 upper arm, 1-2 forearm, 2+ hand)
            best = None
            for k in range(3):
                a, b = J[k], J[k + 1]
                t = np.dot(p - a, b - a) / max(1e-9, np.dot(b - a, b - a))
                tc = min(max(t, 0.0), 1.0) if k < 2 else max(t, 0.0)
                d = np.linalg.norm(a + tc * (b - a) - p)
                if best is None or d < best[0]:
                    best = (d, k + (min(t, 1.0) if k < 2 else min(t, 1.0)))
            u = max(best[1], 0.0)
            k = min(int(u), 2)
            f = u - k
            w = {bi[k]: 1.0}
            blend = 0.15	# of a segment, either side of a joint
            if k < 2 and f > 1 - blend:
                x = (f - (1 - blend)) / (2 * blend)
                w = {bi[k]: 1 - x, bi[k + 1]: x}
            elif k > 0 and f < blend:
                x = (blend - f) / (2 * blend)
                w = {bi[k]: 1 - x, bi[k - 1]: x}
            VW[v] = [(b, x) for b, x in w.items() if x > 0]


def untear(data, B, Q, tris, VW, rounds=8):
    """Last pass, on the reposed result: a vertex that moves far from where
    its neighbours go when the arms go up (a coat side holding on to a
    forearm, a sleeve end stuck at the hip) takes their weights."""
    index, tpos = dae.read_skeleton(os.path.join(data, 'skeletons', 'thps5_human.ske.xbx'))
    sc = 72.6 / Q[:, 1].max()
    R, rest, posed = dae.repose(B, tpos, sc)
    E = np.unique(np.sort(np.array([(c[a][0], c[(a + 1) % 3][0]) for _, c in tris for a in range(3)]), axis=1), axis=0)
    E = E[E[:, 0] != E[:, 1]]
    L0 = np.linalg.norm(Q[E[:, 0]] - Q[E[:, 1]], axis=1) * sc
    fixed = 0
    for _ in range(rounds):
        P = np.array([sum(w * ((Q[i] * sc - rest[b]) @ R[b] + posed[b]) for b, w in vw) / sum(w for _, w in vw)
                      if vw else Q[i] * sc for i, vw in enumerate(VW)])
        D = P - Q * sc	# how far each vertex moved
        nbr = [[] for _ in range(len(Q))]
        for (x, y), l0 in zip(E, L0):
            if l0 > 1e-6:
                nbr[x].append((y, l0)); nbr[y].append((x, l0))
        torn = []
        for v, nl in enumerate(nbr):
            if nl:
                o = [x for x, _ in nl]
                off = np.linalg.norm(D[v] - D[o].mean(0))
                if off > 0.6 * np.mean([l for _, l in nl]) + 0.5:
                    torn.append(v)
        if not torn:
            break
        new = {}
        for v in torn:
            others = [x for x, _ in nbr[v]]
            acc = {}
            for o in others:
                for b, w in VW[o]:
                    acc[b] = acc.get(b, 0.0) + w / len(others)
            t = sum(acc.values())
            if t > 0:
                new[v] = [(b, w / t) for b, w in acc.items()]
        for v, vw in new.items():
            VW[v] = vw
        fixed += len(new)
    return fixed


def convert(data, out, name, obj_fn, refs, sub='gta', force_ref=None):
    V, T, N, tris = load_obj(obj_fn)
    M, Q, shift, h, scores = fit(V, refs)
    k = force_ref if force_ref is not None else scores[0][1]
    ref = refs[k]
    P, B = adapt(ref, Q)
    VW = transfer(ref, P, Q)
    # nothing the hand only reaches over the shoulder moves with an arm: a
    # shirt hem or baggy trousers beside the A-pose hand are close in a
    # straight line, far along the mesh
    names = [b[0] for b in B]
    # mesh graph over welded positions (rips split vertices along UV seams)
    _, weld = np.unique(np.round(Q, 5), axis=0, return_inverse=True)
    weld = weld.ravel()
    E = np.array([(weld[c[a][0]], weld[c[(a + 1) % 3][0]]) for _, c in tris for a in range(3)])
    Qw = np.zeros((weld.max() + 1, 3)); Qw[weld] = Q
    L = np.linalg.norm(Qw[E[:, 0]] - Qw[E[:, 1]], axis=1) + 1e-9
    graph = coo_matrix((np.r_[L, L], (np.r_[E[:, 0], E[:, 1]], np.r_[E[:, 1], E[:, 0]])),
                       shape=(weld.max() + 1, weld.max() + 1)).tocsr()
    protect = segments(VW, names, weld, graph, Q, B)
    rebind_arms(VW, names, B, Q, protect)
    for sd, sg in (('left', 1), ('right', -1)):
        S = B[names.index('arm %s shoulder 2' % sd)][2]
        ids = {i for i, n in enumerate(names) if n in [a % sd for a in ARM]}
        aw = np.array([sum(w for b, w in vw if b in ids) for vw in VW])
        T0 = tip(Q[(Q[:, 1] > 0.25) & (Q[:, 1] < S[1]) & (sg * Q[:, 0] > 0)], sg)
        # seeds along the arm: the hand's tip, and the arm vertices nearest
        # the wrist and elbow (the hand is often a piece of its own)
        seeds = [int(np.argmin(np.linalg.norm(Q - T0, axis=1)))]
        armv = np.where(aw > 0.5)[0]
        if len(armv):
            for j in ('arm %s wrist' % sd, 'arm %s elbow' % sd):
                seeds.append(int(armv[np.argmin(np.linalg.norm(Q[armv] - B[names.index(j)][2], axis=1))]))
        geo = dijkstra(graph, indices=sorted({int(weld[x]) for x in seeds}), min_only=True)[weld]
        eu = np.min([np.linalg.norm(Q - Q[x], axis=1) for x in seeds], axis=0)
        # beside the arm in a straight line, far along the mesh (or on
        # another piece altogether): a hem, a hip, a trouser leg
        far = (~np.isfinite(geo) | (geo > 2.5 * eu + 0.1)) & (Q[:, 1] < S[1])
        far &= (aw < 0.95) & ~protect	# wholly arm already, or an arm piece: a sleeve, not a hem
        for i in np.where(far)[0]:
            keep = [(b, w) for b, w in VW[i] if b not in ids]
            t = sum(w for _, w in keep)
            VW[i] = [(b, w / t) for b, w in keep] if t > 0 else [(names.index('root hips'), 1.0)]
    untear(data, B, Q, tris, VW)
    norms = N @ M.T if N is not None else None
    r = dae.build(data, out, name, B, Q, norms, T, tris, VW, os.path.dirname(obj_fn), sub, guess=True)
    return r + (os.path.basename(ref['fn']), scores[:3])


if __name__ == '__main__':
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    refs = [ref_entry(f) for f in sys.argv[5:]]
    nv, nt, a, b, rn, sc = convert(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], refs)
    print('%s: %d vertices, %d triangles, bbox %s %s, rig from %s' % (sys.argv[3], nv, nt, np.round(a, 1),
                                                                    np.round(b, 1), rn))
