#!/usr/bin/env python3
"""THPS4 (Aspyr PC) level -> THUG level files, for the desktop port.

    thps4_to_thug.py <THPS4 Game\\data> <level> <out Data folder>

reads  levels/<L>/<L>scn.dat, <L>col.dat, <L>tex.dat, <L>.qb, <L>_Scripts.qb
       (and levels/<L>_Sky/ the same way)
writes levels/<L>/<L>.scn.xbx, .col.xbx, .tex.xbx, <L>.qb, <L>_scripts.qb

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
        if flags & 0x10: out += r.b(12 * nv)
        if flags & 0x01:
            sets = r.one('i')
            out += struct.pack('<i', sets) + r.b(8 * sets * nv)
        if flags & 0x02:
            # Aspyr's PC colours run 0-255; THUG's (Xbox) 0-128, doubled when drawn.
            cols = struct.unpack_from(f'<{nv}I', r.b(4 * nv))
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


def convert_nodes(d):
    """Node structs whose Class has no model in THUG: CreatedAtStart removed
    (the token is blanked to an end-of-line, so offsets stay put)."""
    d = bytearray(d)
    i, depth, start, removed = 0, 0, None, 0
    while i < len(d) and d[i] != 0:
        t = d[i]
        if t == 3:
            depth += 1
            if depth == 1: start = i
        elif t == 4:
            if depth == 1 and start is not None:
                j, cls, cls_at, created, models = start, None, None, [], []
                while j < i:
                    if d[j] == 0x16:
                        c = struct.unpack_from('<I', d, j + 1)[0]
                        nxt = skip(d, j, 4)
                        if c == C_CLASS and d[nxt] == 7 and d[nxt + 1] == 0x16:
                            cls = struct.unpack_from('<I', d, nxt + 2)[0]
                            cls_at = nxt + 2
                        elif c == C_CREATED:
                            created.append(j)
                        elif c in MODEL_KEYS and d[nxt] == 7:
                            models.append(j)

                    j = skip(d, j, 4)
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
        q, n = convert_nodes(rd(f'{L}.qb'))
        q, r = fix_randoms(q)
        open(os.path.join(o, f'{L}.qb'), 'wb').write(q)
        try:
            sq = rd(f'{L}_Scripts.qb')
            copy_sounds(os.path.dirname(src_levels), sq, os.path.dirname(dst_levels))
            q, r2 = fix_randoms(sq)
            r += r2
            open(os.path.join(o, f'{L}_scripts.qb'), 'wb').write(q)
        except FileNotFoundError:
            pass
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
