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
from scipy.sparse.csgraph import dijkstra
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
    for M in turns():
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
    for sd, sg in (('left', 1), ('right', -1)):
        S = B[names.index('arm %s shoulder 2' % sd)][2]
        ids = {i for i, n in enumerate(names) if n in [a % sd for a in ARM]}
        T0 = tip(Q[(Q[:, 1] > 0.25) & (Q[:, 1] < S[1]) & (sg * Q[:, 0] > 0)], sg)
        h = int(np.argmin(np.linalg.norm(Q - T0, axis=1)))
        geo = dijkstra(graph, indices=weld[h])[weld]
        eu = np.linalg.norm(Q - Q[h], axis=1)
        far = np.isfinite(geo) & (geo > 2.5 * eu + 0.1) & (Q[:, 1] < S[1])
        aw = np.array([sum(w for b, w in vw if b in ids) for vw in VW])
        far &= aw < 0.95		# wholly arm already: a sleeve, not a hem
        for i in np.where(far)[0]:
            keep = [(b, w) for b, w in VW[i] if b not in ids]
            t = sum(w for _, w in keep)
            VW[i] = [(b, w / t) for b, w in keep] if t > 0 else [(names.index('root hips'), 1.0)]
    norms = N @ M.T if N is not None else None
    r = dae.build(data, out, name, B, Q, norms, T, tris, VW, os.path.dirname(obj_fn), sub)
    return r + (os.path.basename(ref['fn']), scores[:3])


if __name__ == '__main__':
    if len(sys.argv) < 6:
        sys.exit(__doc__)
    refs = [ref_entry(f) for f in sys.argv[5:]]
    nv, nt, a, b, rn, sc = convert(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], refs)
    print('%s: %d vertices, %d triangles, bbox %s %s, rig from %s' % (sys.argv[3], nv, nt, np.round(a, 1),
                                                                    np.round(b, 1), rn))
