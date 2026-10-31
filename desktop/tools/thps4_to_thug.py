#!/usr/bin/env python3
"""THPS4 (Aspyr PC) level -> THUG level files, for the desktop port.

    thps4_to_thug.py <THPS4 Game\\data> <level> <out Data folder>

reads  levels/<L>/<L>scn.dat, <L>col.dat, <L>tex.dat, <L>.qb, <L>_Scripts.qb
       (and levels/<L>_Sky/ the same way)
writes levels/<L>/<L>.scn.xbx, .col.xbx, .tex.xbx, <L>.qb, <L>_scripts.qb,
       models/animals/..., anims/anl_.../

THPS4 shares THUG's engine, so most of the data carries over as is:
 - textures: same format, copied.
 - scene: sectors and vertices are THUG's layout; materials lack the name,
   z-bias, back-face and specular fields (grass sits where no_bfc is), pass layout differs (no envmap tiling,
   36 extra bytes), and meshes have no bounding box or LOD list.
 - collision: version 8 -> 9. v8 vertices are 16 bytes (xyz + rgba), large
   faces 12 bytes, and its BSP nodes are another format: each object gets a
   single BSP leaf holding all its faces.
 - vertex colours: halved (Aspyr's PC data is 0-255, THUG's 0-128).
 - scripts: same tokens, except Random & co. have no weights (added). Objects the THUG data has no models for
   (peds, cars, game objects) lose CreatedAtStart and their model names.
 - animals (Pedestrian nodes with a Model and an animload_anl_ set): skins
   and shadow models converted like the scene (float bone weights -> THUG's
   packed ones) to models/, their animations re-keyed for THUG's key tables
   to anims/<set>/. THUG ships their skeletons and animload_ scripts.
 - sounds: the .wav files the level's LoadSound lines name are copied to
   sounds/wav/ (16-bit PCM; the port loads them when THUG has no .pcm of that name).
Personal use with your own copy of THPS4.
"""
import os, re, struct, sys, shutil

# ---------------------------------------------------------------- helpers

def crc(s):
    c = 0xFFFFFFFF
    for ch in s.lower().replace('/', '\\').encode('latin-1'):
        c ^= ch
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
    return c


class R:
    def __init__(s, d, o=0): s.d, s.o = d, o
    def t(s, f):
        v = struct.unpack_from('<' + f, s.d, s.o); s.o += struct.calcsize('<' + f); return v
    def one(s, f): return s.t(f)[0]
    def b(s, n):
        v = s.d[s.o:s.o + n]; s.o += n; return v


# ---------------------------------------------------------------- scene

def convert_scn(d, sectors=None):
    r = R(d)
    vers = r.t('3I')
    out = bytearray(struct.pack('<3I', *vers))
    n = r.one('I')
    out += struct.pack('<I', n)
    for _ in range(n):
        cs, passes, alpha_cutoff = r.t('3I')
        srt, draw_order, single_sided, grassify = r.t('BfBB')
        if grassify:				# grass height, layers (THUG's port draws no grass shells)
            r.t('fi')
        out += struct.pack('<4I', cs, cs, passes, alpha_cutoff)
        out += struct.pack('<BfBBiBf', srt, draw_order, single_sided, 0, 0, 0, 0.0)
        for p in range(passes):
            tex, fl, has_color = r.t('IIB')
            color = r.t('3f')
            reg_alpha = r.one('Q')
            ua, va = r.t('II')
            filt = r.one('I')
            r.b(36)
            uvw = r.b(32) if fl & 0x1 else b''
            if p == 0 and fl & 0x2:			# vertex colour wibble: not carried over
                nseq = r.one('I')
                for _ in range(nseq):
                    nk, _ph = r.t('Ii'); r.b(8 * nk)
            anim = b''
            if fl & 0x800:
                hdr = r.b(16)
                nk = struct.unpack_from('<i', hdr)[0]
                anim = hdr + r.b(8 * nk)
            tail = r.b(16)
            fl &= ~0x2
            out += struct.pack('<IIB3fQII2fI', tex, fl, has_color, *color, reg_alpha, ua, va, 1.0, 1.0, filt)
            out += uvw + anim + tail
    ns = r.one('i')
    out += struct.pack('<i', ns)
    for _ in range(ns):
        cs, bone, flags, nm = r.t('IiiI')
        out += struct.pack('<IiiI', cs, bone, flags, nm)
        if sectors is not None: sectors.add(cs)
        out += r.b(40)
        if flags & 0x800000:
            out += r.b(40)
        nv, stride = r.t('ii')
        out += struct.pack('<ii', nv, stride)
        pos_raw = r.b(12 * nv)
        pos = [struct.unpack_from('<3f', pos_raw, 12 * i) for i in range(nv)]
        out += pos_raw
        if flags & 0x04: out += r.b(12 * nv)
        if flags & 0x10:			# skinned: THPS4 has 4 float weights, THUG 3 packed 11:11:10
            ws = r.t(f'{4 * nv}f')
            bs = r.t(f'{4 * nv}H')
            pw, pb = [], []
            for v in range(nv):
                top = sorted(range(4), key=lambda k: -ws[4 * v + k])[:3]
                w = [max(ws[4 * v + k], 0.0) for k in top]
                tot = sum(w) or 1.0
                q = [int(w[0] / tot * 1023), int(w[1] / tot * 1023), int(w[2] / tot * 511)]
                pw.append(q[0] | q[1] << 11 | q[2] << 22)
                pb += [bs[4 * v + k] if w[n] > 0 else 0 for n, k in enumerate(top)] + [0]
            out += struct.pack(f'<{nv}I', *pw) + struct.pack(f'<{4 * nv}H', *pb)
        if flags & 0x01:
            sets = r.one('i')
            out += struct.pack('<i', sets) + r.b(8 * sets * nv)
        if flags & 0x02:
            # Aspyr's PC colours run 0-255; THUG's (Xbox) 0-128, doubled when drawn.
            # Skins: THUG's own (its THPS4 dog, Elissa's head) are flat 0x80 grey.
            cols = struct.unpack_from(f'<{nv}I', r.b(4 * nv))
            if flags & 0x10:
                out += struct.pack(f'<{nv}I', *[(c & 0xFF000000) | 0x808080 for c in cols])
            else:
                out += struct.pack(f'<{nv}I', *[(c & 0xFF000000) | ((c >> 1) & 0x007F7F7F) for c in cols])
        if flags & 0x800: out += r.b(nv)
        for _ in range(nm):
            mflags, mat, count = r.t('IIi')
            idx = r.b(2 * count)
            used = [pos[i] for i in struct.unpack_from(f'<{count}H', idx)] or pos or [(0, 0, 0)]
            lo = [min(v[k] for v in used) for k in range(3)]
            hi = [max(v[k] for v in used) for k in range(3)]
            c = [(lo[k] + hi[k]) / 2 for k in range(3)]
            rad = max(sum((v[k] - c[k]) ** 2 for k in range(3)) for v in used) ** 0.5
            out += struct.pack('<3ff3f3f', *c, rad, *lo, *hi)
            out += struct.pack('<IIIi', mflags, mat, 1, count) + idx
    nh = r.one('i')
    out += struct.pack('<i', nh) + d[r.o:]
    return bytes(out)


# ---------------------------------------------------------------- collision

def convert_col(d, drop=()):
    version, nobj, total_verts, nfl, nfs = struct.unpack_from('<5i', d, 0)
    if version != 8:
        raise ValueError(f'col version {version}, expected 8')
    vb = (32 + nobj * 64 + 15) & ~15
    fb = vb + total_verts * 16
    objs = []
    for i in range(nobj):
        o = 32 + 64 * i
        cs, fla, nv, nf, small, fixed, foff = struct.unpack_from('<IHHHBBI', d, o)
        bbox = d[o + 16:o + 48]
        voff, = struct.unpack_from('<I', d, o + 48)
        objs.append((cs, fla, nv, nf, small, foff, bbox, voff))

    # Dropped objects keep no faces: the header counts must match what is
    # written, or THUG looks for the BSP nodes past the real faces.
    nfl -= sum(nf for cs, fla, nv, nf, small, *_ in objs if cs in drop and not small)
    nfs -= sum(nf for cs, fla, nv, nf, small, *_ in objs if cs in drop and small)
    hdr = struct.pack('<8i', 9, nobj, total_verts, nfl, nfs, total_verts, 0, 0)
    obj_out = bytearray()
    verts = bytearray()
    inten = bytearray()
    for v in range(total_verts):
        x, y, z, rgba = struct.unpack_from('<3fI', d, vb + 16 * v)
        verts += struct.pack('<3f', x, y, z)
        rr, gg, bb = rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF
        inten.append(min(255, (rr + gg + bb) // 3))
    faces = bytearray()
    nodes = bytearray()
    fidx = bytearray()
    nidx = 0
    for i, (cs, fla, nv, nf, small, foff, bbox, voff) in enumerate(objs):
        if cs in drop:
            nf = 0
        new_foff = len(faces)
        for f in range(nf):
            if small:
                fl, terr, a, b, c, _ = struct.unpack_from('<HHBBBB', d, fb + foff + 8 * f)
                faces += struct.pack('<HHBBBB', fl, terr, a, b, c, 0)
            else:
                fl, terr, a, b, c = struct.unpack_from('<HHHHH', d, fb + foff + 12 * f)
                faces += struct.pack('<HHHHH', fl, terr, a, b, c)
        bsp_off = len(nodes)
        nodes += struct.pack('<BBHI', 3, 0, nf, nidx)
        fidx += struct.pack(f'<{nf}H', *range(nf))
        nidx += nf
        obj_out += struct.pack('<IHHHBBI', cs, fla, nv, nf, small, 0, new_foff) + bbox
        obj_out += struct.pack('<4I', voff * 12, bsp_off, voff, 0)

    out = bytearray(hdr + obj_out)
    out += bytes(((len(out) + 15) & ~15) - len(out))
    out += verts + inten
    out += bytes(((len(out) + 3) & ~3) - len(out))
    out += faces
    if nfl & 1:
        out += bytes(2)
    out += struct.pack('<i', len(nodes)) + nodes + fidx
    return bytes(out)


# ---------------------------------------------------------------- scripts

def skip(d, i, rnd=6):
    t = d[i]
    if t in (0x16, 0x17, 0x18, 0x1a, 0x02, 0x2e, 0x43, 0x44): return i + 5
    if t == 0x1e: return i + 13
    if t == 0x1f: return i + 9
    if t in (0x1b, 0x1c): return i + 5 + struct.unpack_from('<I', d, i + 1)[0]
    if t == 0x2b:
        j = i + 5
        while d[j]: j += 1
        return j + 1
    if t in RANDOMS:
        return i + 5 + rnd * struct.unpack_from('<I', d, i + 1)[0]
    return i + 1


RANDOMS = (0x2f, 0x37, 0x40, 0x41)


def fix_randoms(d):
    """THPS4's Random/Random2/RandomNoRepeat/RandomPermute carry a count and
    one jump offset per item; THUG's put a u16 weight per item between the
    two. The weights (all 1) go in, and every jump and random offset that
    spans an insertion is widened to match."""
    ins = []					# (position, bytes inserted)
    jumps = []					# (offset field position, end of field)
    i = 0
    while i < len(d) and d[i] != 0:
        t = d[i]
        if t == 0x2e:
            jumps.append((i + 1, i + 5))
        elif t in RANDOMS:
            n = struct.unpack_from('<I', d, i + 1)[0]
            ins.append((i + 5, 2 * n))
            for k in range(n):
                f = i + 5 + 4 * k
                jumps.append((f, f + 4))
        i = skip(d, i, 4)
    if not ins:
        return d, 0

    def moved(p):					# new position of old byte p
        return p + sum(s for q, s in ins if q <= p)

    out = bytearray()
    last = 0
    for q, s in ins:
        out += d[last:q]
        n = s // 2
        out += struct.pack(f'<{n}H', *([1] * n))
        last = q
    out += d[last:]
    for f, e in jumps:
        off = struct.unpack_from('<i', d, f)[0]
        t = e + off
        # a random's own weights sit before its offsets: moved() counts them
        # for the field and its targets alike
        struct.pack_into('<i', out, moved(f), moved(t) - moved(e))
    return bytes(out), len(ins)


NO_MODELS = {crc(c) for c in ('GameObject', 'Pedestrian', 'Vehicle')}
# proximity triggers: THPS4's run its goal scripts (College's keg trigger
# re-ran itself until the stack overflowed). THUG's proximity manager takes
# every ProximNode in the node array, so their Class is renamed: no
# proximity node is made at all.
C_PROXIM = crc('ProximNode')
NOT_CREATED = NO_MODELS
C_CLASS, C_CREATED = crc('Class'), crc('CreatedAtStart')
# model fields of those nodes: renamed, so the level load doesn't preload
# THPS4 models THUG's data doesn't have (PreloadModels, PreloadPedestrians)
MODEL_KEYS = {crc(k) for k in ('Model', 'ShadowModel', 'profile')}
C_UNUSED = crc('th4_unused')


def node_string(d, at):
    """The string value after a 'key =' at d[at] (0x1b token), or None."""
    if d[at] == 7 and d[at + 1] == 0x1b:
        n = struct.unpack_from('<I', d, at + 2)[0]
        return d[at + 6:at + 5 + n].decode('latin-1')
    return None


def convert_nodes(d, keep=None):
    """Node structs whose Class has no model in THUG: CreatedAtStart removed
    (the token is blanked to an end-of-line, so offsets stay put).
    keep(cls, fields) spares a node whose models were converted, by returning
    the model keys still to blank (None: not spared); fields maps key
    checksums to their string or checksum values."""
    d = bytearray(d)
    i, depth, start, removed = 0, 0, None, 0
    while i < len(d) and d[i] != 0:
        t = d[i]
        if t == 3:
            depth += 1
            if depth == 1: start = i
        elif t == 4:
            if depth == 1 and start is not None:
                j, cls, cls_at, created, models, fields = start, None, None, [], [], {}
                while j < i:
                    if d[j] == 0x16:
                        c = struct.unpack_from('<I', d, j + 1)[0]
                        nxt = skip(d, j, 4)
                        if c == C_CLASS and d[nxt] == 7 and d[nxt + 1] == 0x16:
                            cls = struct.unpack_from('<I', d, nxt + 2)[0]
                            cls_at = nxt + 2
                        elif c == C_CREATED:
                            created.append(j)
                        elif d[nxt] == 7:
                            if c in MODEL_KEYS:
                                models.append(j)
                            if d[nxt + 1] == 0x16:
                                fields[c] = struct.unpack_from('<I', d, nxt + 2)[0]
                            else:
                                fields[c] = node_string(d, nxt)
                    j = skip(d, j, 4)
                blank = keep(cls, fields) if cls in NO_MODELS and keep else None
                if blank is not None:
                    for j in models:
                        if struct.unpack_from('<I', d, j + 1)[0] in blank:
                            struct.pack_into('<I', d, j + 1, C_UNUSED)
                else:
                    if cls in NOT_CREATED:
                        for j in created:
                            d[j:j + 5] = b'\x01' * 5
                            removed += 1
                    for j in (models if cls in NO_MODELS else []):
                        struct.pack_into('<I', d, j + 1, C_UNUSED)
                    # their Class goes too: THPS4's scripts Create some of them
                    # later (London's buses), and THUG's car code crashed on them
                    if cls == C_PROXIM or cls in NO_MODELS:
                        struct.pack_into('<I', d, cls_at, C_UNUSED)
                        removed += 1
            depth -= 1
        i = skip(d, i, 4)
    return bytes(d), removed


def qb_names(d):
    """The checksum-name table at the end of a qb (0x2b entries)."""
    names, i = {}, 0
    while True:
        i = d.find(b'\x2b', i)
        if i < 0 or i + 5 > len(d):
            return names
        j = i + 5
        while j < len(d) and 32 <= d[j] < 127:
            j += 1
        if j < len(d) and d[j] == 0 and j > i + 5:
            names[struct.unpack_from('<I', d, i + 1)[0]] = d[i + 5:j].decode()
        i += 1


def model_stems(d):
    """Short names of the models of the nodes THUG gets no model for
    (anl_rhino.skin -> 'rhino')."""
    stems, i, depth, start = set(), 0, 0, None
    while i < len(d) and d[i] != 0:
        t = d[i]
        if t == 3:
            depth += 1
            if depth == 1: start = i
        elif t == 4:
            if depth == 1 and start is not None:
                j, cls, models = start, None, []
                while j < i:
                    if d[j] == 0x16:
                        c = struct.unpack_from('<I', d, j + 1)[0]
                        nxt = skip(d, j, 4)
                        if c == C_CLASS and d[nxt] == 7 and d[nxt + 1] == 0x16:
                            cls = struct.unpack_from('<I', d, nxt + 2)[0]
                        elif c in MODEL_KEYS and d[nxt] == 7 and d[nxt + 1] == 0x1b:
                            n = struct.unpack_from('<I', d, nxt + 2)[0]
                            models.append(d[nxt + 6:nxt + 5 + n].decode('latin-1'))
                    j = skip(d, j, 4)
                if cls in NO_MODELS:
                    for m in models:
                        b = os.path.splitext(m.replace('\\', '/').split('/')[-1])[0].lower()
                        for p in ('anl_', 'ped_', 'veh_', 'zoo_', 'goal_'):
                            if b.startswith(p): b = b[len(p):]
                        b = re.sub(r'(_shadow|_collision|\d+)$', '', b)
                        if len(b) >= 4 and b != 'none':
                            stems.add(b)
            depth -= 1
        i = skip(d, i, 4)
    return stems


def orphan_collision(col, qb, sectors):
    """Collision-only objects (no drawn sector) that stand in for a model THUG
    doesn't get: Zoo's rhinos and giraffes, Carnival's goats. With the model
    gone they were invisible walls. Named after the model, small, solid."""
    names, stems = qb_names(qb), model_stems(qb)
    version, nobj, total_verts = struct.unpack_from('<3i', col, 0)
    fb = ((32 + nobj * 64 + 15) & ~15) + total_verts * 16
    drop = set()
    for i in range(nobj):
        o = 32 + 64 * i
        cs, fla, nv, nf, small, fixed, foff = struct.unpack_from('<IHHHBBI', col, o)
        if not nf or cs in sectors:
            continue
        name = names.get(cs, '').lower()
        if name.startswith('inviso') or not any(st in name for st in stems):	# Inviso*: walls meant to be unseen
            continue
        lo, hi = struct.unpack_from('<3f', col, o + 16), struct.unpack_from('<3f', col, o + 32)
        if max(hi[k] - lo[k] for k in range(3)) >= 800:
            continue
        if all(struct.unpack_from('<H', col, fb + foff + (8 if small else 12) * f)[0] & 0x10 for f in range(nf)):
            continue
        drop.add(cs)
        print(f'  collision of {names[cs]} left out (its model is not converted)')
    return drop


# ---------------------------------------------------------------- animations

# THPS4's .ska is THUG's compressed format, but the two games' key tables
# (anims/standardkeyQ.bin, standardkeyT.bin) differ: keys that point into
# THPS4's table are written out in full, which THUG reads without a table.

def key_table(path):
    d = open(path, 'rb').read()
    return [struct.unpack_from('<3h', d, 8 * i) for i in range(256)]


def convert_ska(d, qt, tt):
    ver, fl, dur, nb, nq, nt, nc, qa, ta = struct.unpack_from('<IIf6I', d, 0)
    if not fl & (1 << 23):
        return d					# not table-compressed: nothing to translate
    o = 36
    qs = struct.unpack_from(f'<{nb}H', d, o); o += 2 * nb
    ts = struct.unpack_from(f'<{nb}H', d, o); o += 2 * nb
    o = (o + 3) & ~3
    partial = b''
    if fl & (1 << 19):
        n = struct.unpack_from('<i', d, o)[0]
        partial = d[o:o + 4 + 4 * ((n - 1) // 32 + 1)]
        o += len(partial)
    o = (o + 3) & ~3
    u = lambda a: d[a] | d[a + 1] << 8
    qo, to = o, o + qa
    qout, tout, qsz, tsz = bytearray(), bytearray(), [], []
    for b in range(nb):
        start, end = len(qout), qo + qs[b]
        while qo < end:
            data = u(qo); stamp, sign = data & 0x3FFF, data & 0x8000; qo += 2
            if data & 0x4000:
                stamp &= 0x7FF
                if not data & 0x3800:
                    q = qt[d[qo]]; qo += 1
                else:
                    q = []
                    for bit in (0x2000, 0x1000, 0x0800):
                        if data & bit: q.append(d[qo]); qo += 1
                        else: q.append(u(qo)); qo += 2
            else:
                q = [u(qo), u(qo + 2), u(qo + 4)]; qo += 6
            qout += struct.pack('<H3H', stamp | sign, *[v & 0xFFFF for v in q])
        qsz.append(len(qout) - start)
        start, end = len(tout), to + ts[b]
        while to < end:
            f = d[to]; to += 1
            if f & 0x40: stamp = f & 0x3F
            else: stamp = u(to); to += 2
            if f & 0x80: t = tt[d[to]]; to += 1
            else: t = [u(to), u(to + 2), u(to + 4)]; to += 6
            tout += struct.pack('<BH3H', 0, stamp, *[v & 0xFFFF for v in t])
        tsz.append(len(tout) - start)
    if max(qsz + tsz + [0]) > 0xFFFF:
        raise ValueError('bone track too long once expanded')
    out = bytearray(struct.pack('<IIf6I', ver, fl, dur, nb, nq, nt, nc, len(qout), len(tout)))
    out += struct.pack(f'<{nb}H', *qsz) + struct.pack(f'<{nb}H', *tsz)
    out += bytes(-len(out) % 4) + partial
    out += bytes(-len(out) % 4) + qout + tout
    out += bytes(-len(out) % 4) + d[(to + 3) & ~3:]	# custom keys, as they were
    return bytes(out)


# ---------------------------------------------------------------- models

def find_path(root, parts):
    for p in parts:
        root = find(root, p)
    return root


def convert_model(src_data, name, dst_data, prefix=()):
    """'animals\\anl_rhino\\anl_rhino.skin' (THPS4 node Model) ->
    models/animals/anl_rhino/anl_rhino.skin.xbx + .tex.xbx (+ .col.xbx for
    a .mdl), under models/<prefix>/ when given. False when
    THPS4 has no such model."""
    parts = name.replace('/', '\\').lower().split('\\')
    base, ext = os.path.splitext(parts[-1])
    if ext not in ('.skin', '.mdl'):
        return False
    try:
        folder = find_path(find(src_data, 'models'), parts[:-1])
        geo = find(folder, base + ext[1:] + '.dat')
        tex = find(folder, base + 'tex.dat')
    except FileNotFoundError:
        return False
    out = os.path.join(dst_data, 'models', *prefix, *parts[:-1])
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, base + ext + '.xbx'), 'wb').write(convert_scn(open(geo, 'rb').read()))
    shutil.copyfile(tex, os.path.join(out, base + '.tex.xbx'))
    if ext == '.mdl':
        # THUG loads <base>.col.xbx for every .mdl; without one it crashed.
        try:
            col = convert_col(open(find(folder, base + 'col.dat'), 'rb').read())
        except FileNotFoundError:
            return False
        open(os.path.join(out, base + '.col.xbx'), 'wb').write(col)
    return True


def convert_anims(src_data, anim_set, dst_data):
    """Every animation of THPS4's anims/<set> (animload_anl_rhino -> anl_rhino),
    where THUG's own animload_ scripts look for them."""
    try:
        folder = find(find(src_data, 'anims'), anim_set)
        a = find(src_data, 'anims')
        qt, tt = key_table(find(a, 'standardkeyQ.bin')), key_table(find(a, 'standardkeyT.bin'))
    except FileNotFoundError:
        return 0
    out = os.path.join(dst_data, 'anims', anim_set.lower())
    os.makedirs(out, exist_ok=True)
    n = 0
    for f in os.listdir(folder):
        if f.lower().endswith('ska.dat'):
            d = convert_ska(open(os.path.join(folder, f), 'rb').read(), qt, tt)
            open(os.path.join(out, f[:-7].lower() + '.ska.xbx'), 'wb').write(d)
            n += 1
    return n


# ---------------------------------------------------------------- cameras

# A level's LoadCameras script loads levels\<L>\<name>.ska: the goal start /
# success cameras and the intro. THPS4 PC keeps them as <name>ska.dat (Aspyr
# cut long names to 29 characters, so G_HS_CameraSuccessska.da happens).
# They are THUG's uncompressed camera format, except that THPS4 doesn't
# long-align the keys after the per-bone key counts; THUG's reader does.

def cam_files(qb, L):
    out, i, low = [], 0, f'levels\\{L.lower()}\\'
    while True:
        i = qb.find(b'\x1b', i)
        if i < 0 or i + 5 > len(qb):
            return out
        n = struct.unpack_from('<I', qb, i + 1)[0]
        if 0 < n < 200:
            t = qb[i + 5:i + 5 + n].rstrip(b'\0').decode('latin-1')
            if t.lower().startswith(low) and t.lower().endswith('.ska'):
                out.append(t.split('\\')[-1][:-4])
        i += 1


def custom_keys_end(d, o, n):
    for _ in range(n):
        if o + 12 > len(d):
            return -1
        size = struct.unpack_from('<I', d, o + 8)[0]
        if size < 12:
            return -1
        o += size
    return o


def convert_cam(d):
    ver, fl, dur, nb, nq, nt, nc = struct.unpack_from('<IIf4I', d, 0)
    if fl & ((1 << 23) | (1 << 24) | (1 << 19)):
        return d					# compressed / object / partial: not cameras
    o = 28 + (4 if fl & (1 << 22) else 2) * nb
    end = o + 16 * (nq + nt)
    if o % 4 == 0 or custom_keys_end(d, end, nc) != len(d):
        return d					# already laid out the way THUG reads it
    out = d[:o] + bytes(-o % 4) + d[o:end]
    return out + bytes(-len(out) % 4) + d[end:]


def convert_cameras(src_level, L, qb, dst):
    names = cam_files(qb, L)
    files = os.listdir(src_level)
    n, lost = 0, []
    for name in names:
        want = name.lower()
        src = None
        for f in files:
            fl = f.lower()
            if fl.startswith(want) and ('ska.dat'.startswith(fl[len(want):]) or
                                        '.ska'.startswith(fl[len(want):])) and len(fl) > len(want):
                src = f
                break
        if not src:
            lost.append(name)
            continue
        d = convert_cam(open(os.path.join(src_level, src), 'rb').read())
        open(os.path.join(dst, want + '.ska.xbx'), 'wb').write(d)
        n += 1
    print(f'{L}: {n}/{len(names)} cameras converted' + (f'; missing: {", ".join(lost)}' if lost else ''))
    return n


C_MODEL, C_SHADOW, C_PROFILE = crc('Model'), crc('ShadowModel'), crc('profile')
C_PED, C_ANIM, C_EXTRA = crc('Pedestrian'), crc('animName'), crc('Extra_Anims')


def animal_keeper(src_data, dst_data, names):
    """Pedestrian nodes with a THPS4 animal (Model + animload_anl_ set): its
    skin and animations are converted and the node stays. THUG still ships
    THPS4's animal skeletons and animload_anl_ scripts, only not the files."""
    done = {}
    def keep(cls, f):
        model, anim = f.get(C_MODEL), names.get(f.get(C_ANIM), '')
        if cls != C_PED or not isinstance(model, str) or C_PROFILE in f \
                or not anim.lower().startswith('animload_anl_'):
            return None
        if model not in done:
            done[model] = convert_model(src_data, model, dst_data)
        if not done[model]:
            return None
        for a in {anim, names.get(f.get(C_EXTRA), '')}:
            if a.lower().startswith('animload_') and a not in done:
                done[a] = convert_anims(src_data, a[len('animload_'):], dst_data)
                print(f'  {a}: {done[a]} animations')
        shadow = f.get(C_SHADOW)
        if isinstance(shadow, str):
            if shadow not in done:
                done[shadow] = convert_model(src_data, shadow, dst_data)
            if not done[shadow]:
                return {C_SHADOW}
        return set()
    keep.done = done
    return keep


# ---------------------------------------------------------------- human peds

# THUG keeps THPS4's 50-bone human skeleton but not its ped animations: its
# animload_human is a stub and its animload_Ped_ scripts load THUG's own
# (thps5_human) files from the same paths. So THPS4's human sets get their
# own names (animload_th4_...), their files go to anims/th4/, and the scripts
# loading them are appended to the level qb. Models go to models/th4/.

C_SETREF, C_LOADANIM = crc('SetReferenceChecksum'), crc('LoadAnim')
C_NAME, C_DESC, C_DESC_ID, C_MESH = crc('name'), crc('descChecksum'), crc('desc_id'), crc('mesh')
C_PED_BODY, C_SCALE, C_TRIGGER = crc('ped_body'), crc('scale'), crc('TriggerScript')
HUMAN_REFS = {crc('animload_human'): 'animload_human', crc('animload_Ped_F'): 'animload_Ped_F'}
PROFILE_EXTRAS = {crc(k) for k in ('ped_body', 'ped_board', 'scale', 'body_shape')}


def th4_set(name):
    return 'animload_th4_' + name[len('animload_'):]


def anim_scripts(src_data):
    """THPS4's human animload_ scripts: checksum -> (name, reference set,
    [(file, descChecksum)])."""
    d = open(find(find(src_data, 'scripts'), 'allanims.qb'), 'rb').read()
    names = qb_names(d)
    out, cur, i = {}, None, 0
    while i < len(d) and d[i]:
        if d[i] == 0x23 and d[i + 1] == 0x16:
            c = struct.unpack_from('<I', d, i + 2)[0]
            cur = out[c] = [names.get(c, ''), None, []]
        elif d[i] == 0x16 and cur is not None and d[i + 5] == 7:
            c = struct.unpack_from('<I', d, i + 1)[0]
            if c == C_NAME and d[i + 6] == 0x1b:
                cur[2].append([node_string(d, i + 5), None])
            elif c == C_DESC and d[i + 6] == 0x16 and cur[2]:
                cur[2][-1][1] = struct.unpack_from('<I', d, i + 7)[0]
        elif d[i] == 0x16 and cur is not None and d[i + 5] == 0x16 \
                and struct.unpack_from('<I', d, i + 1)[0] == C_SETREF:
            cur[1] = struct.unpack_from('<I', d, i + 6)[0]
        i = skip(d, i, 4)
    return {c: (n, ref, a) for c, (n, ref, a) in out.items() if ref in HUMAN_REFS}


def script_bytes(name, ref, anims):
    """script <name> SetReferenceChecksum <ref> LoadAnim ... SetReferenceChecksum 0 endscript"""
    b = bytearray(struct.pack('<BBI', 0x23, 0x16, crc(name)) + b'\x01')
    b += struct.pack('<BIBI', 0x16, C_SETREF, 0x16, crc(ref)) + b'\x01'
    for f, desc in anims:
        f = f.encode('latin-1') + b'\0'
        b += struct.pack('<BIBIBBI', 0x16, C_LOADANIM, 0x16, C_NAME, 7, 0x1b, len(f)) + f
        b += struct.pack('<BIBBI', 0x16, C_DESC, 7, 0x16, desc) + b'\x01'
    b += struct.pack('<BIBi', 0x16, C_SETREF, 0x17, 0) + b'\x01\x24\x01'
    return bytes(b)


def human_anims(src_data, dst_data, qbs, cache):
    """The THPS4 human sets the level's scripts call (and animload_human):
    renamed in the qbs, their files converted, their scripts returned."""
    scripts = cache.setdefault('scripts', anim_scripts(src_data))
    a = find(src_data, 'anims')
    qt, tt = key_table(find(a, 'standardkeyQ.bin')), key_table(find(a, 'standardkeyT.bin'))
    used = [c for c in scripts if c in HUMAN_REFS or any(b'\x16' + struct.pack('<I', c) in q for q in qbs)]
    code, total, missing = bytearray(), 0, 0
    for c in used:
        name, ref, anims = scripts[c]
        keep = []
        for f, desc in anims:
            parts = f.replace('/', '\\').split('\\')		# anims\Ped_M\Ped_M_Idle1.ska
            if len(parts) != 3:
                missing += 1
                continue
            out = os.path.join(dst_data, 'anims', 'th4', parts[1].lower(), parts[2].lower()[:-4] + '.ska.xbx')
            if not os.path.exists(out):
                try:
                    src = find(find(a, parts[1]), parts[2][:-4] + 'ska.dat')
                except FileNotFoundError:
                    missing += 1
                    cache.setdefault('missing_anims', set()).add(parts[1] + '\\' + parts[2])
                    continue
                os.makedirs(os.path.dirname(out), exist_ok=True)
                open(out, 'wb').write(convert_ska(open(src, 'rb').read(), qt, tt))
            keep.append(('anims\\th4\\' + parts[1].lower() + '\\' + parts[2].lower(), desc))
            total += 1
        code += script_bytes(th4_set(name), th4_set(HUMAN_REFS[ref]), keep)
    renames = {struct.pack('<I', c): struct.pack('<I', crc(th4_set(scripts[c][0]))) for c in used}
    print(f'  human anim sets: {len(used)}, {total} animations ({missing} not in THPS4 data or not staged)')
    return renames, bytes(code)


def rename_checksums(d, renames):
    d = bytearray(d)
    i = 0
    while i < len(d) and d[i]:
        if d[i] == 0x16 and bytes(d[i + 1:i + 5]) in renames:
            d[i + 1:i + 5] = renames[bytes(d[i + 1:i + 5])]
        i = skip(d, i, 4)
    return bytes(d)


def append_scripts(d, code):
    """Scripts go before the checksum-name table at the end of the qb."""
    i = 0
    while i < len(d) and d[i] and d[i] != 0x2b:
        i = skip(d, i, 4)
    return d[:i] + code + d[i:]


def ped_profiles(src_data, cache):
    """THPS4 ped profiles made of one ped_body (pros, kids, cops...): profile
    checksum -> that body's mesh, as a node Model ('peds\\pedpros\\...skin')."""
    if 'profiles' in cache:
        return cache['profiles']
    sc = find(src_data, 'scripts')
    mesh, last = {}, None
    for f in ('cas_ped_m.qb', 'cas_ped_f.qb'):
        d = open(find(sc, f), 'rb').read()
        i = 0
        while i < len(d) and d[i]:
            if d[i] == 0x16 and d[i + 5] == 7:
                c = struct.unpack_from('<I', d, i + 1)[0]
                if c == C_DESC_ID and d[i + 6] == 0x16:
                    last = struct.unpack_from('<I', d, i + 7)[0]
                elif c == C_MESH and d[i + 6] == 0x1b and last is not None:
                    m = node_string(d, i + 5).replace('/', '\\')
                    mesh.setdefault(last, m[len('models\\'):] if m.lower().startswith('models\\') else m)
            i = skip(d, i, 4)
    d = open(find(sc, 'profile_ped.qb'), 'rb').read()
    out, i, depth, cur, keys, body, key, scale = {}, 0, 0, None, set(), None, None, None
    while i < len(d) and d[i]:
        t = d[i]
        if t == 3:
            depth += 1
        elif t == 4:
            depth -= 1
            if depth == 0 and cur is not None:
                if keys <= PROFILE_EXTRAS and body in mesh:
                    out[cur] = (mesh[body], scale)
                cur = None
        elif t == 0x16 and d[i + 5] == 7:
            c = struct.unpack_from('<I', d, i + 1)[0]
            if depth == 0 and d[i + 6] == 3:
                cur, keys, body, scale = c, set(), None, None
            elif depth == 1:
                keys.add(c)
                key = c
                if c == C_SCALE and d[i + 6] == 0x1e:	# kids: scale = (x,y,z)
                    scale = bytes(d[i + 7:i + 19])
            elif depth == 2 and key == C_PED_BODY and c == C_DESC_ID and d[i + 6] == 0x16:
                body = struct.unpack_from('<I', d, i + 7)[0]
        i = skip(d, i, 4)
    cache['profiles'] = out
    return out


def profiles_to_models(d, profiles):
    """'profile = pedpro_Hawk_profile' -> 'Model = "th4\\peds\\...skin"' (and
    the profile's scale, which makes kids kid-sized) in the node array
    (before the first script, so no jump spans the change)."""
    out, i, last, n = bytearray(), 0, 0, 0
    while i < len(d) and d[i] and d[i] != 0x23:
        if d[i] == 0x16 and d[i + 5] == 7 and d[i + 6] == 0x16 \
                and struct.unpack_from('<I', d, i + 1)[0] == C_PROFILE:
            m, scale = profiles.get(struct.unpack_from('<I', d, i + 7)[0], (None, None))
            if m:
                m = m.encode('latin-1') + b'\0'
                out += d[last:i] + struct.pack('<BIBBI', 0x16, C_MODEL, 7, 0x1b, len(m)) + m
                if scale:
                    out += struct.pack('<BIBB', 0x16, C_SCALE, 7, 0x1e) + scale
                last = i + 11
                n += 1
        i = skip(d, i, 4)
    return bytes(out + d[last:]), n


def vehicle_scripts(qbs):
    """Scripts that use a car, tram or game object node (all left out), or
    call a script that does: Zoo's tram driver sat in mid-air on his route."""
    gone, bodies = set(), {}
    for d in qbs:
        i, depth, start = 0, 0, None
        while i < len(d) and d[i]:
            t = d[i]
            if t == 3:
                depth += 1
                if depth == 1: start = i
            elif t == 4:
                if depth == 1 and start is not None:
                    body = d[start:i]
                    cls = re.search(re.escape(struct.pack('<BIBB', 0x16, C_CLASS, 7, 0x16)) + b'(....)', body, re.S)
                    name = re.search(re.escape(struct.pack('<BIBB', 0x16, crc('name'), 7, 0x16)) + b'(....)', body, re.S)
                    if cls and name and struct.unpack('<I', cls.group(1))[0] in (crc('Vehicle'), crc('GameObject')):
                        gone.add(name.group(1))
                depth -= 1
            elif t == 0x23 and d[i + 1] == 0x16:
                j = i
                while j < len(d) and d[j] and d[j] != 0x24:
                    j = skip(d, j, 4)
                bodies[d[i + 2:i + 6]] = d[i + 6:j]
            i = skip(d, i, 4)
    used = set()
    while True:
        new = {c for c, b in bodies.items() if c not in used
               and any(b'\x16' + x in b for x in gone | used)}
        if not new:
            return {struct.unpack('<I', c)[0] for c in used}
        used |= new


def human_keeper(src_data, dst_data, names, animals, riders=()):
    """Pedestrian nodes with a THPS4 human model (after profiles_to_models):
    the model goes to models/th4/ and the node's Model is renamed to match
    by the caller; the node stays. Animals are left to animal_keeper, and
    peds whose TriggerScript rides a left-out vehicle are left out."""
    done = {}
    def keep(cls, f):
        r = animals(cls, f)
        if r is not None:
            return r
        model, anim = f.get(C_MODEL), f.get(C_ANIM)
        if cls != C_PED or not isinstance(model, str) or C_PROFILE in f or anim not in HUMAN_REFS \
                or f.get(C_TRIGGER) in riders:
            return None
        if model not in done:
            done[model] = convert_model(src_data, model, dst_data, ('th4',))
        return set() if done[model] else None
    keep.done = done
    keep.animals = animals
    return keep


def prefix_models(d, models):
    """Node Model strings of the converted human peds get the th4\\ prefix;
    strings change length, so only before the first script."""
    out, i, last = bytearray(), 0, 0
    want = {m.lower() for m in models}
    while i < len(d) and d[i] and d[i] != 0x23:
        if d[i] == 0x16 and d[i + 5] == 7 and d[i + 6] == 0x1b \
                and struct.unpack_from('<I', d, i + 1)[0] == C_MODEL:
            m = node_string(d, i + 5)
            if m.lower() in want:
                m = ('th4\\' + m).encode('latin-1') + b'\0'
                n = struct.unpack_from('<I', d, i + 7)[0]
                out += d[last:i] + struct.pack('<BIBBI', 0x16, C_MODEL, 7, 0x1b, len(m)) + m
                last = i + 11 + n
        i = skip(d, i, 4)
    return bytes(out + d[last:])


# ---------------------------------------------------------------- career goals
# THPS4's levels add their goals with AddGoal_<type> (<L>_Goals, called as
# the level struct's goals_script). THUG kept every THPS4 goal type and its
# Goal_*_GenericParams, but not the AddGoal_ wrappers (two lines each) nor
# the per-level texts and scores (goal_scores.qb, goal_descriptions.qb:
# sch_highscore_text...). Both are appended to the level's scripts qb.

def top_items(d):
    """Top-level statements of a qb: [(name checksum, bytes)], scripts included."""
    out, i, start, depth, last = [], 0, None, 0, 0
    while i < len(d) and d[i]:
        t = d[i]
        if t == 0x2b:
            break
        if t in (0x01, 0x02) and depth == 0:		# EOL, or EOL with line number
            if start is not None and last == 0x07:	# 'name =' with its value on the next line
                i = skip(d, i, 4)
                continue
            if start is not None:
                out.append((struct.unpack_from('<I', d, start + (2 if d[start] == 0x23 else 1))[0], d[start:i] + b'\x01'))
            start = None
        elif start is None:
            start = i
        if t not in (0x01, 0x02): last = t
        if t in (0x03, 0x05, 0x23): depth += 1
        elif t in (0x04, 0x06, 0x24): depth -= 1
        i = skip(d, i, 4)
    return out


def always_init(b):
    """THUG only starts goals listed in its story chapters, unless their
    params carry always_initialize_goal: added after each '<...>'."""
    out, i = bytearray(), 0
    while i < len(b):
        j = skip(b, i, 4)
        out += b[i:j]
        if b[i] == 0x2c:
            out += struct.pack('<BI', 0x16, crc('always_initialize_goal'))
        i = j
    return bytes(out)


def goal_code(src_data, L, cache):
    if 'goal_wrappers' not in cache:
        sc = find(src_data, 'scripts')
        u = open(find(sc, 'goal_utilities.qb'), 'rb').read()
        names = qb_names(u)
        cache['goal_wrappers'] = b''.join(always_init(b) for c, b in top_items(u)
                                         if b[0] == 0x23 and names.get(c, '').lower().startswith('addgoal_'))
        g = []
        for f in ('goal_scores.qb', 'goal_descriptions.qb'):
            q = open(find(sc, f), 'rb').read()
            n = qb_names(q)
            g += [(n.get(c, '').lower(), b) for c, b in top_items(q) if b[0] == 0x16]
        cache['goal_globals'] = g
    pre = L.lower() + '_'
    mine = b''.join(b for n, b in cache['goal_globals'] if n.startswith(pre))
    print(f'  goals: {len(cache["goal_wrappers"])} bytes of AddGoal_ wrappers, '
          f'{sum(1 for n, b in cache["goal_globals"] if n.startswith(pre))} {L} goal texts/scores')
    # skip_intro = 1: the level's <L>_Ped_ExplainsGoals intro (a ped walks up
    # while the camera plays CAM_INTRO) freezes the skater and the start
    # button, then waits on animations and a camera THUG doesn't have.
    skip_intro = struct.pack('<BIBBi', 0x16, crc('skip_intro'), 0x07, 0x17, 1) + b'\x01'
    return cache['goal_wrappers'] + mine + skip_intro + pro_anim_arrays(L)


# THUG's pro scripts (GenericProTalking & co) play anims from arrays such as
# generic_pro_anims_talk, whose THUG names (Ped_M_Talk2_...) THPS4's human set
# lacks: the missing anim fell back to 'default', the bind pose. The desktop
# build first looks for <level>_pro_anims_<type>; these use THPS4's own anims.
PRO_ANIMS = {
    'wait': ['Ped_M_Idle1', 'Ped_M_LookAround', 'Ped_M_LookAtWatch', 'Ped_M_LookAtWatch2',
             'Ped_M_LookLeftRight', 'Ped_M_Lookright', 'Ped_M_Idle2', 'Ped_M_Wave', 'Ped_M_Idle1'],
    'talking': ['Ped_M_Talk1', 'Ped_M_Talk2', 'Ped_M_Talk_Explain', 'Ped_M_Talk_Point',
                'Ped_M_Talk_PointOut', 'Ped_M_Talk_LikeThat', 'Ped_M_Talk_LikeThis', 'Ped_M_Talk_Nod',
                'Ped_M_Talk_Shrug', 'Ped_M_Talk_RaiseArn', 'Ped_M_Talk_TurnThink', 'Ped_M_Talk_Isupposeso'],
    'midgoal': ['Ped_M_Idle1', 'Ped_M_Clap', 'Ped_M_PlayerWave', 'Ped_M_Cheering', 'Ped_M_LookAround',
                'Ped_M_ThumbUp', 'Ped_M_Idle3'],
    'success': ['Ped_M_Cheer1', 'Ped_M_Cheer2', 'Ped_M_Cheer3', 'Ped_M_Cheer4', 'Ped_M_Cheer5',
                'Ped_M_Cheer6', 'Ped_M_Cheer7', 'Ped_M_Cheering', 'Ped_M_Clap', 'Ped_M_ThumbUp', 'Ped_M_PeaceBro'],
    'angry': ['Ped_M_Disgust', 'Ped_M_ShakeFist'],
}
PRO_ANIMS['talk'] = PRO_ANIMS['talking']
PRO_ANIMS['spectate'] = PRO_ANIMS['midgoal']
PRO_ANIMS['cheer'] = PRO_ANIMS['success']


def pro_anim_arrays(L):
    name = lambda n: struct.pack('<BI', 0x16, crc(n))
    out = b''
    for t, anims in PRO_ANIMS.items():
        out += name(f'{L}_pro_anims_{t}') + b'\x07\x05'
        for a in anims:
            out += b'\x03' + name('anims') + b'\x07\x05\x03' + name('anim') + b'\x07' + name(a) + b'\x04\x06\x04'
        out += b'\x06\x01'
    return out


# ---------------------------------------------------------------- main

def find(folder, name):
    want = name.lower()
    for f in os.listdir(folder):
        if f.lower() == want:
            return os.path.join(folder, f)
    raise FileNotFoundError(os.path.join(folder, name))


def copy_sounds(src_data, qb, dst):
    """Every string in the script that names a THPS4 sound file is copied."""
    try:
        wav = find(find(src_data, 'sounds'), 'wav')
    except FileNotFoundError:
        print('no THPS4 sounds folder, level sounds skipped')
        return
    files = {}
    for root, _, names in os.walk(wav):
        for f in names:
            rel = os.path.relpath(os.path.join(root, f), wav).replace(os.sep, '\\')
            files[rel.lower()] = os.path.join(root, f)
    n, i = 0, 0
    while True:
        i = qb.find(b'\x1b', i)
        if i < 0 or i + 5 > len(qb):
            break
        ln = struct.unpack_from('<I', qb, i + 1)[0]
        name = qb[i + 5:i + 4 + ln].decode('latin-1') if 0 < ln < 128 else ''
        src = files.get(name.replace('/', '\\').lower() + '.wav')
        if src:
            out = os.path.join(dst, 'sounds', 'wav', *name.replace('/', '\\').split('\\')) + '.wav'
            os.makedirs(os.path.dirname(out), exist_ok=True)
            shutil.copyfile(src, out)
            n += 1
            i += 5 + ln
        else:
            i += 1
    print(f'{n} sounds copied')


CACHE = {}


def convert_level(src_levels, L, dst_levels, scripts=True):
    s = find(src_levels, L)
    o = os.path.join(dst_levels, L)
    os.makedirs(o, exist_ok=True)
    rd = lambda n: open(find(s, n), 'rb').read()
    sectors = set()
    open(os.path.join(o, f'{L}.scn.xbx'), 'wb').write(convert_scn(rd(f'{L}scn.dat'), sectors))
    col = rd(f'{L}col.dat')
    drop = orphan_collision(col, rd(f'{L}.qb'), sectors) if scripts else ()
    open(os.path.join(o, f'{L}.col.xbx'), 'wb').write(convert_col(col, drop))
    shutil.copyfile(find(s, f'{L}tex.dat'), os.path.join(o, f'{L}.tex.xbx'))
    if scripts:
        src_data, dst_data = os.path.dirname(src_levels), os.path.dirname(dst_levels)
        q, np = profiles_to_models(rd(f'{L}.qb'), ped_profiles(src_data, CACHE))
        animals = animal_keeper(src_data, dst_data, qb_names(q))
        try:
            sq = rd(f'{L}_Scripts.qb')
        except FileNotFoundError:
            sq = None
        riders = vehicle_scripts([q] + ([sq] if sq else []))
        keep = human_keeper(src_data, dst_data, qb_names(q), animals, riders)
        q, n = convert_nodes(q, keep)
        kept = sorted(m for m, ok in animals.done.items() if ok and '.' in m)
        if kept: print(f'{L}: models converted: {", ".join(kept)}')
        humans = sorted(m for m, ok in keep.done.items() if ok)
        lost = sorted(m for m, ok in keep.done.items() if not ok)
        print(f'{L}: {np} ped profiles made single models; human peds: {", ".join(humans)}')
        if lost: print(f'{L}: human ped models THPS4 data lacks: {", ".join(lost)}')
        q = prefix_models(q, humans)
        renames, code = human_anims(src_data, dst_data, [q] + ([sq] if sq else []), CACHE)
        q = append_scripts(rename_checksums(q, renames), code)
        q, r = fix_randoms(q)
        open(os.path.join(o, f'{L}.qb'), 'wb').write(q)
        convert_cameras(s, L, q, o)
        if sq:
            copy_sounds(os.path.dirname(src_levels), sq, os.path.dirname(dst_levels))
            sq = append_scripts(sq, goal_code(src_data, L, CACHE))
            q, r2 = fix_randoms(rename_checksums(sq, renames))
            r += r2
            open(os.path.join(o, f'{L}_scripts.qb'), 'wb').write(q)
        print(f'{L}: {r} random operators given weights')
        print(f'{L}: {n} objects (peds, cars, game objects, proximity triggers) left out')
    print(f'{L}: done -> {o}')


if __name__ == '__main__':
    src, L, dst = sys.argv[1:4]
    lv_src = find(src, 'levels')
    lv_dst = os.path.join(dst, 'levels')
    convert_level(lv_src, L, lv_dst)
    try:
        convert_level(lv_src, L + '_Sky', lv_dst, scripts=False)
    except FileNotFoundError as e:
        print('no sky:', e)
