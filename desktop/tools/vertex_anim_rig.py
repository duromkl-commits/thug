"""Rig a vertex-animated model (Quake .mdl / .md2 style) from its own animation.

Every frame of the model's animation is a snapshot of a rigid-part skeleton
(id's artists animated hierarchical parts), so the vertices split into parts
that each move rigidly: those parts are found by an SSDR-style alternation
(fit each part's rigid motion per frame, re-assign each vertex to the part
that explains its whole trajectory best). Joints between neighbouring parts
are the points both parts' motions keep together (least-squares pivot); the
skeleton is the minimum spanning tree over the pivot error, and its limbs are
named from where the leaves sit in the rest frame (head on top, feet lowest,
hands the remaining two)."""
import numpy as np


def kabsch(A, B):
    ca, cb = A.mean(0), B.mean(0)
    U, S, Vt = np.linalg.svd((A - ca).T @ (B - cb))
    d = np.sign(np.linalg.det(Vt.T @ U.T))
    R = Vt.T @ np.diag([1, 1, d]) @ U.T
    return R, cb - ca @ R.T


def motions(F, rest, idx):
    return [kabsch(rest[idx], F[f, idx]) for f in range(len(F))]


def ssdr(F, rest, K, it=20, seed=0):
    nf, nv, _ = F.shape
    X = F.transpose(1, 0, 2).reshape(nv, -1)
    rng = np.random.default_rng(seed)
    C = [X[rng.integers(nv)]]
    for _ in range(K - 1):
        d = np.min([((X - c) ** 2).sum(1) for c in C], axis=0)
        C.append(X[rng.choice(nv, p=d / d.sum())])
    lab = np.argmin(((X[:, None] - np.array(C)[None]) ** 2).sum(2), 1)
    for _ in range(it):
        err = np.full((nv, K), np.inf)
        for k in range(K):
            idx = np.where(lab == k)[0]
            if len(idx) < 3:
                continue
            e = np.zeros(nv)
            for f, (R, t) in enumerate(motions(F, rest, idx)):
                e += ((rest @ R.T + t - F[f]) ** 2).sum(1)
            err[:, k] = e
        nl = np.argmin(err, 1)
        if (nl == lab).all():
            break
        lab = nl
    return lab, err[np.arange(nv), lab].sum()


def edges_of(tris):
    E = set()
    for t in tris:
        for i in range(3):
            a, b = t[i], t[(i + 1) % 3]
            E.add((min(a, b), max(a, b)))
    return E


def clean(lab, E, nv, passes=3):
    """Re-label vertices whose part is not connected to their part's main body,
    and lone vertices, by their neighbours' majority."""
    nb = [[] for _ in range(nv)]
    for a, b in E:
        nb[a].append(b); nb[b].append(a)
    for _ in range(passes):
        # largest connected piece per label
        seen = -np.ones(nv, int); keep = np.zeros(nv, bool)
        for k in set(lab):
            comps = []
            for s in np.where(lab == k)[0]:
                if seen[s] >= 0:
                    continue
                stack = [s]; seen[s] = len(comps); comp = [s]
                while stack:
                    x = stack.pop()
                    for y in nb[x]:
                        if lab[y] == k and seen[y] < 0:
                            seen[y] = len(comps); stack.append(y); comp.append(y)
                comps.append(comp)
            keep[max(comps, key=len)] = True
        changed = False
        for v in np.where(~keep)[0]:
            ls = [lab[y] for y in nb[v] if keep[y]]
            if ls:
                lab[v] = max(set(ls), key=ls.count); changed = True
        if not changed:
            break
    return lab


def pivot(Ma, Mb, meet, lam):
    """Point p minimising sum_f |Ra p + ta - (Rb p + tb)|^2 + lam |p - meet|^2
    (pulled toward where the parts meet when their relative turn is small,
    which leaves the pivot undetermined) -> (p, rms error)."""
    A = np.concatenate([Ra - Rb for (Ra, ta), (Rb, tb) in zip(Ma, Mb)])
    y = np.concatenate([tb - ta for (Ra, ta), (Rb, tb) in zip(Ma, Mb)])
    p = np.linalg.solve(A.T @ A + lam * np.eye(3), A.T @ y + lam * meet)
    r = A @ p - y
    return p, float(np.sqrt((r ** 2).mean() * 3))


def segment(F, rest, tris, K=16, seeds=6):
    nv = rest.shape[0]
    E = edges_of(tris)
    best = None
    for s in range(seeds):
        lab, e = ssdr(F, rest, K, seed=s)
        if best is None or e < best[1]:
            best = (lab, e)
    lab = clean(best[0].copy(), E, nv)
    ks = sorted(set(lab))
    lab = np.array([ks.index(x) for x in lab])
    return lab, E


def skeleton(F, rest, lab, E):
    K = lab.max() + 1
    M = [motions(F, rest, np.where(lab == k)[0]) for k in range(K)]
    adj = {}
    for a, b in E:
        if lab[a] != lab[b]:
            key = (min(lab[a], lab[b]), max(lab[a], lab[b]))
            adj[key] = adj.get(key, 0) + 1
    J = {}
    for (a, b), n in adj.items():
        # a pivot far from where the parts meet is not a joint
        meet = np.mean([(rest[x] + rest[y]) / 2 for x, y in E
                        if (lab[x], lab[y]) in ((a, b), (b, a))], axis=0)
        p, err = pivot(M[a], M[b], meet, 0.05 * len(F))
        J[(a, b)] = (p, err + 0.5 * np.linalg.norm(p - meet), meet)
    # minimum spanning tree (Prim) over the joint cost
    inT = {0}; T = []
    while len(inT) < K:
        cand = [(J[(a, b)][1], a, b) for (a, b) in J if (a in inT) != (b in inT)]
        if not cand:	# disconnected part: hang it on the nearest part
            out = [k for k in range(K) if k not in inT][0]
            c = rest[lab == out].mean(0)
            near = min(inT, key=lambda k: np.linalg.norm(rest[lab == k].mean(0) - c))
            p = c
            J[(min(near, out), max(near, out))] = (p, 1e9, p)
            T.append((near, out)); inT.add(out); continue
        _, a, b = min(cand)
        T.append((a, b)); inT.add(a); inT.add(b)
    return T, J, M


def _path(T, a, b):
    nb = {}
    for x, y in T:
        nb.setdefault(x, []).append(y); nb.setdefault(y, []).append(x)
    prev = {a: None}; st = [a]
    while st:
        x = st.pop()
        for y in nb.get(x, []):
            if y not in prev:
                prev[y] = x; st.append(y)
    p = [b]
    while p[-1] != a:
        p.append(prev[p[-1]])
    return p[::-1]


def _proj(P, poly):
    """Arc-length parameter of each point's closest point on a polyline."""
    seg = np.diff(poly, axis=0); L = np.linalg.norm(seg, axis=1); cum = np.r_[0, np.cumsum(L)]
    best = np.full(len(P), np.inf); s = np.zeros(len(P))
    for i in range(len(seg)):
        t = np.clip(((P - poly[i]) @ seg[i]) / max(L[i] ** 2, 1e-9), 0, 1)
        d = np.linalg.norm(P - (poly[i] + t[:, None] * seg[i]), axis=1)
        m = d < best; best[m] = d[m]; s[m] = cum[i] + t[m] * L[i]
    return s, cum[-1]


def _along(poly, s):
    seg = np.diff(poly, axis=0); L = np.linalg.norm(seg, axis=1); cum = np.r_[0, np.cumsum(L)]
    i = min(np.searchsorted(cum, s, 'right') - 1, len(seg) - 1)
    return poly[i] + seg[i] * (s - cum[i]) / max(L[i], 1e-9)


def rig(F, rest, tris, up=2, fwd=0, K=16, seeds=6, verbose=False):
    """-> (B [(xps name, parent index, joint)], VW [[(bone, weight)]], labels, tree info).
    rest: (nv,3) with axis `up` vertical and `fwd` pointing forward."""
    lab, E = segment(F, rest, [tuple(t) for t in tris], K, seeds)
    T, J, M = skeleton(F, rest, lab, E)
    K = lab.max() + 1
    deg = np.zeros(K, int)
    for a, b in T:
        deg[a] += 1; deg[b] += 1
    cen = np.array([rest[lab == k].mean(0) for k in range(K)])
    low = np.array([rest[lab == k][:, up].min() for k in range(K)])
    leaves = [k for k in range(K) if deg[k] == 1]
    feet = sorted(leaves, key=lambda k: low[k])[:2]
    rest_l = [k for k in leaves if k not in feet]
    head = max(rest_l, key=lambda k: cen[k][up])
    rest_l = [k for k in rest_l if k != head]
    pf = [_path(T, feet[0], feet[1])]
    # pelvis: on the feet-to-feet path, the part nearest the head's path
    ph = _path(T, head, feet[0])
    pelvis = next(k for k in ph if k in pf[0])
    hp = _path(T, pelvis, head)
    # hands: the two remaining leaves furthest (in parts) from the pelvis
    hands = sorted(rest_l, key=lambda k: -len(_path(T, pelvis, k)))[:2]
    if len(hands) < 2:
        raise ValueError('found %d hands' % len(hands))
    pa = [_path(T, pelvis, h) for h in hands]
    # chest: last common part of the two hand paths
    chest = pelvis
    for x, y in zip(*pa):
        if x == y:
            chest = x
    jt = lambda a, b: J[(min(a, b), max(a, b))][0]
    side = lambda pts: 'left' if pts[:, 1 if up == 2 else 0].mean() > 0 else 'right'
    X = {}; Vseg = {}

    def limb(path_from, names, end_frac):
        """path parts after the junction; names: 3 joint names (root, mid, end)."""
        ch = path_from
        q = [jt(ch[i], ch[i + 1]) for i in range(len(ch) - 1)]
        leafP = rest[lab == ch[-1]]
        d = leafP - q[-1]
        tip = leafP[np.argmax(np.linalg.norm(d, axis=1))]
        poly = np.array(q + [tip])
        s_j = np.r_[0, np.cumsum(np.linalg.norm(np.diff(poly, axis=0), axis=1))]
        tot = s_j[-1]
        # end joint (ankle/wrist): the last real joint if it sits in the last 40%
        if len(q) >= 2 and s_j[len(q) - 1] > 0.6 * tot:
            s_end = s_j[len(q) - 1]
        else:
            s_end = end_frac * tot
        mids = [s for s in s_j[1:len(q) - 1] if 0.3 * s_end < s < 0.7 * s_end]
        s_mid = min(mids, key=lambda s: abs(s - 0.5 * s_end)) if mids else 0.5 * s_end
        X[names[0]] = poly[0]; X[names[1]] = _along(poly, s_mid); X[names[2]] = _along(poly, s_end)
        idx = np.where(np.isin(lab, ch[1:]))[0]
        s, _ = _proj(rest[idx], poly)
        for v, sv in zip(idx, s):
            Vseg[v] = names[0] if sv < s_mid else names[1] if sv < s_end else names[2]
        return poly, s_end, tot, tip

    for k, foot in enumerate(feet):
        ch = _path(T, pelvis, foot)
        sd = side(rest[lab == foot])
        names = ['leg %s thigh' % sd, 'leg %s knee' % sd, 'leg %s ankle' % sd]
        poly, s_end, tot, tip = limb(ch, names, 0.85)
        A = rest[[v for v, n in Vseg.items() if n == names[2]]] if any(n == names[2] for n in Vseg.values()) \
            else rest[lab == foot]
        front = A[A[:, fwd] >= np.percentile(A[:, fwd], 85)].mean(0)
        toe = front.copy(); ank = X[names[2]]
        toe[up] = A[:, up].min() + 0.3 * (ank[up] - A[:, up].min())
        X['leg %s toes' % sd] = toe
    for hnd in hands:
        ch = _path(T, chest, hnd)
        sd = side(rest[lab == hnd])
        names = ['arm %s shoulder 2' % sd, 'arm %s elbow' % sd, 'arm %s wrist' % sd]
        poly, s_end, tot, tip = limb(ch, names, 0.8)
        X['arm %s finger 2a' % sd] = _along(poly, s_end + 0.5 * (tot - s_end))
        X['arm %s finger 2b' % sd] = tip
    # head: joint into the head chain from the chest side, top part
    hc = _path(T, chest, head)
    if len(hc) >= 2:
        X['head neck upper'] = jt(hc[-2], hc[-1])
        X['head neck lower'] = jt(hc[0], hc[1]) if len(hc) >= 3 else None
    else:
        X['head neck upper'] = rest[lab == head][:, :].min(0)
    for v in np.where(np.isin(lab, hc[1:]))[0]:
        Vseg[v] = 'head neck upper'
    hips = (X['leg left thigh'] + X['leg right thigh']) / 2
    sh = (X['arm left shoulder 2'] + X['arm right shoulder 2']) / 2
    if X.get('head neck lower') is None:
        X['head neck lower'] = sh + (X['head neck upper'] - sh) * 0.5
    X['root hips'] = hips
    X['spine lower'] = hips + (sh - hips) * 0.2
    X['belly'] = hips + (sh - hips) * 0.45
    X['spine upper'] = hips + (sh - hips) * 0.7
    for sd in ('left', 'right'):
        s2 = X['arm %s shoulder 2' % sd]
        X['arm %s shoulder 1' % sd] = (s2 + X['spine upper']) / 2
    par = {'root hips': None, 'spine lower': 'root hips', 'belly': 'spine lower', 'spine upper': 'belly',
           'head neck lower': 'spine upper', 'head neck upper': 'head neck lower'}
    for sd in ('left', 'right'):
        par.update({'arm %s shoulder 1' % sd: 'spine upper', 'arm %s shoulder 2' % sd: 'arm %s shoulder 1' % sd,
                    'arm %s elbow' % sd: 'arm %s shoulder 2' % sd, 'arm %s wrist' % sd: 'arm %s elbow' % sd,
                    'arm %s finger 2a' % sd: 'arm %s wrist' % sd, 'arm %s finger 2b' % sd: 'arm %s finger 2a' % sd,
                    'leg %s thigh' % sd: 'root hips', 'leg %s knee' % sd: 'leg %s thigh' % sd,
                    'leg %s ankle' % sd: 'leg %s knee' % sd, 'leg %s toes' % sd: 'leg %s ankle' % sd})
    order = list(par)
    ix = {n: i for i, n in enumerate(order)}
    B = [(n, ix[par[n]] if par[n] else -1, X[n]) for n in order]
    spine = [('root hips', hips[up]), ('spine lower', X['spine lower'][up]), ('belly', X['belly'][up]),
             ('spine upper', X['spine upper'][up]), ('head neck lower', X['head neck lower'][up])]
    VW = []
    for v in range(len(rest)):
        if v in Vseg:
            VW.append([(ix[Vseg[v]], 1.0)]); continue
        z = rest[v][up]; w = None
        if z <= spine[0][1]:
            w = [(ix['root hips'], 1.0)]
        for (a, za), (b, zb) in zip(spine, spine[1:]):
            if w is None and z <= zb:
                t = float(np.clip((z - za) / max(zb - za, 1e-6), 0, 1))
                w = [(ix[a], 1 - t), (ix[b], t)]
        VW.append(w or [(ix['head neck lower'], 1.0)])
    info = dict(lab=lab, T=T, feet=feet, hands=hands, head=head, pelvis=pelvis, chest=chest)
    if verbose:
        print('parts %d, leaves %d, feet %s hands %s head %d pelvis %d chest %d' %
              (K, len(leaves), feet, hands, head, pelvis, chest))
    return B, VW, info


def _segdist(P, a, b):
    ab = b - a
    t = np.clip(((P - a) @ ab) / max(ab @ ab, 1e-9), 0, 1)
    return np.linalg.norm(P - (a + t[:, None] * ab), axis=1)


def rig_landmarks(F, rest, L, up=2, fwd=0, lat=1, refine=3, tris=None):
    """Rig from hand-placed landmarks (rest-frame coords): L = {'shoulder R': p, 'shoulder L',
    'elbow R/L', 'wrist R/L', 'neck': z, 'crotch': z}; legs are found from the mesh
    (below the crotch, split left/right), and each vertex goes to the bone whose rigid motion
    over the animation fits it best among the bones near it."""
    P = rest
    X = {}
    zc = L['crotch']
    # split the legs with 2-means on the lateral axis (models don't all stand centred)
    inleg = P[:, up] < zc
    if 'legbox' in L:	# (fwd min, fwd max, lat min, lat max): keeps held props out of the legs
        a, b, c0, d0 = L['legbox']
        inleg &= (P[:, fwd] >= a) & (P[:, fwd] <= b) & (P[:, lat] >= c0) & (P[:, lat] <= d0)
    for box in L.get('propbox', ()):	# (fwd, lat, up) min/max boxes of held props near the legs
        a, b, c0, d0, e0, f0 = box
        inleg &= ~((P[:, fwd] >= a) & (P[:, fwd] <= b) & (P[:, lat] >= c0) & (P[:, lat] <= d0) &
                   (P[:, up] >= e0) & (P[:, up] <= f0))
    lo = P[inleg]
    c = np.array([lo[:, lat].min(), lo[:, lat].max()])
    for _ in range(20):
        side = np.abs(lo[:, lat, None] - c[None]).argmin(1)
        c = np.array([lo[side == k, lat].mean() for k in (0, 1)])
    cut = c.mean()
    for S, sd, sg in (('R', 'right', -1), ('L', 'left', 1)):
        X['arm %s shoulder 2' % sd] = np.array(L['shoulder ' + S], float)
        X['arm %s elbow' % sd] = np.array(L['elbow ' + S], float)
        X['arm %s wrist' % sd] = np.array(L['wrist ' + S], float)
        lg = P[inleg & (np.sign(P[:, lat] - cut) == sg)]
        zmin = lg[:, up].min(); zt = lg[:, up].max(); ll = zt - zmin
        top = lg[lg[:, up] > zt - 0.2 * ll].mean(0)
        hip = top.copy(); hip[up] = zt - 0.05 * ll
        dz = np.abs(lg[:, up] - (zmin + 0.5 * ll))
        midv = lg[dz <= max(0.1 * ll, np.sort(dz)[min(3, len(dz) - 1)])].mean(0)
        midv[up] = zmin + 0.5 * ll
        bot = lg[lg[:, up] < zmin + 0.12 * ll]
        ank = bot.mean(0); ank[up] = zmin + 0.08 * ll
        # ankle sits over the back half of the foot
        ank[fwd] = np.percentile(bot[:, fwd], 35)
        X['leg %s thigh' % sd] = hip; X['leg %s knee' % sd] = midv; X['leg %s ankle' % sd] = ank
        front = bot[bot[:, fwd] >= np.percentile(bot[:, fwd], 80)].mean(0)
        toe = front.copy(); toe[up] = zmin + 0.04 * ll
        X['leg %s toes' % sd] = toe
    hips = (X['leg left thigh'] + X['leg right thigh']) / 2
    sh = (X['arm left shoulder 2'] + X['arm right shoulder 2']) / 2
    neck = sh.copy(); neck[up] = L['neck']
    X['root hips'] = hips
    X['spine lower'] = hips + (sh - hips) * 0.2
    X['belly'] = hips + (sh - hips) * 0.45
    X['spine upper'] = hips + (sh - hips) * 0.7
    X['head neck lower'] = sh + (neck - sh) * 0.5
    X['head neck upper'] = neck
    top = P[np.argmax(P[:, up])]
    segs = {'root hips': (hips, X['spine lower']), 'spine lower': (X['spine lower'], X['belly']),
            'belly': (X['belly'], X['spine upper']), 'spine upper': (X['spine upper'], X['head neck lower']),
            'head neck lower': (X['head neck lower'], neck), 'head neck upper': (neck, neck + np.eye(3)[up] * (top[up] - neck[up]))}
    for sd in ('left', 'right'):
        s2, el, wr = (X['arm %s %s' % (sd, n)] for n in ('shoulder 2', 'elbow', 'wrist'))
        X['arm %s shoulder 1' % sd] = (s2 + X['spine upper']) / 2
        segs['arm %s shoulder 1' % sd] = (X['arm %s shoulder 1' % sd], s2)
        segs['arm %s shoulder 2' % sd] = (s2, el)
        segs['arm %s elbow' % sd] = (el, wr)
        d = (wr - el) / np.linalg.norm(wr - el)
        segs['arm %s wrist' % sd] = (wr, wr + d * np.linalg.norm(wr - el) * 0.5)
        segs['leg %s thigh' % sd] = (X['leg %s thigh' % sd], X['leg %s knee' % sd])
        segs['leg %s knee' % sd] = (X['leg %s knee' % sd], X['leg %s ankle' % sd])
        segs['leg %s ankle' % sd] = (X['leg %s ankle' % sd], X['leg %s toes' % sd])
    names = list(segs)
    D = np.stack([_segdist(P, *segs[n]) for n in names], 1)
    lab = np.argmin(D, 1)
    near = D <= D.min(1, keepdims=True) * 1.6 + 1.0
    # legs only below the crotch, nothing else below it
    legb = np.array([n.startswith('leg') for n in names])
    below = inleg
    near &= np.where(below[:, None], legb[None, :], ~legb[None, :])
    D2 = np.where(near, D, np.inf)
    lab = np.argmin(D2, 1)
    for _ in range(refine):
        err = np.full(D.shape, np.inf)
        for k in range(len(names)):
            idx = np.where(lab == k)[0]
            if len(idx) < 3:
                continue
            e = np.zeros(len(P))
            for f in range(len(F)):
                R, t = kabsch(P[idx], F[f, idx]); e += ((P @ R.T + t - F[f]) ** 2).sum(1)
            err[:, k] = e
        err[~near] = np.inf
        nl = np.where(np.isfinite(err.min(1)), np.argmin(err, 1), lab)
        if (nl == lab).all():
            break
        lab = nl
    # small separate pieces (guns, blades, pads) are rigid: the whole piece goes to the one
    # bone whose motion carries it best, so a two-handed gun doesn't stretch between the hands
    if tris is not None:
        par = list(range(len(P)))

        def root(x):
            while par[x] != x:
                par[x] = par[par[x]]; x = par[x]
            return x
        for a, b, c in tris:
            par[root(a)] = root(b); par[root(b)] = root(c)
        comp = np.array([root(i) for i in range(len(P))])
        motion = {}
        for k in set(lab.tolist()):
            idx = np.where(lab == k)[0]
            if len(idx) >= 3:
                motion[k] = [kabsch(P[idx], F[f, idx]) for f in range(len(F))]
        for c in set(comp.tolist()):
            idx = np.where(comp == c)[0]
            if len(idx) > 0.15 * len(P) or len(set(lab[idx].tolist())) < 2:
                continue
            cand = [k for k in set(lab[idx].tolist()) if k in motion]
            if not cand:
                continue
            best = min(cand, key=lambda k: sum(((P[idx] @ R.T + t - F[f, idx]) ** 2).sum()
                                               for f, (R, t) in enumerate(motion[k])))
            lab[idx] = best
    # fingers along the hand
    for sd in ('left', 'right'):
        k = names.index('arm %s wrist' % sd); wr = X['arm %s wrist' % sd]
        d = segs['arm %s wrist' % sd][1] - wr; d /= np.linalg.norm(d)
        H = P[lab == k]
        reach = max(((H - wr) @ d).max(), 1.0) if len(H) else 3.0
        X['arm %s finger 2a' % sd] = wr + d * reach * 0.5
        X['arm %s finger 2b' % sd] = wr + d * reach
    par = {'root hips': None, 'spine lower': 'root hips', 'belly': 'spine lower', 'spine upper': 'belly',
           'head neck lower': 'spine upper', 'head neck upper': 'head neck lower'}
    for sd in ('left', 'right'):
        par.update({'arm %s shoulder 1' % sd: 'spine upper', 'arm %s shoulder 2' % sd: 'arm %s shoulder 1' % sd,
                    'arm %s elbow' % sd: 'arm %s shoulder 2' % sd, 'arm %s wrist' % sd: 'arm %s elbow' % sd,
                    'arm %s finger 2a' % sd: 'arm %s wrist' % sd, 'arm %s finger 2b' % sd: 'arm %s finger 2a' % sd,
                    'leg %s thigh' % sd: 'root hips', 'leg %s knee' % sd: 'leg %s thigh' % sd,
                    'leg %s ankle' % sd: 'leg %s knee' % sd, 'leg %s toes' % sd: 'leg %s ankle' % sd})
    order = list(par)
    ix = {n: i for i, n in enumerate(order)}
    B = [(n, ix[par[n]] if par[n] else -1, X[n]) for n in order]
    spine = [('root hips', hips[up]), ('spine lower', X['spine lower'][up]), ('belly', X['belly'][up]),
             ('spine upper', X['spine upper'][up]), ('head neck lower', X['head neck lower'][up])]
    VW = []
    for v in range(len(P)):
        n = names[lab[v]]
        if n not in ('spine lower', 'belly', 'spine upper', 'root hips') or n == 'root hips' and P[v][up] < hips[up]:
            VW.append([(ix[n], 1.0)]); continue
        z = P[v][up]; w = None
        if z <= spine[0][1]:
            w = [(ix['root hips'], 1.0)]
        for (a, za), (b, zb) in zip(spine, spine[1:]):
            if w is None and z <= zb:
                t = float(np.clip((z - za) / max(zb - za, 1e-6), 0, 1))
                w = [(ix[a], 1 - t), (ix[b], t)]
        VW.append(w or [(ix['spine upper'], 1.0)])
    return B, VW, lab
