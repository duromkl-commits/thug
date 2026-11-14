#!/usr/bin/env python3
"""Builds desktop/src/skater_meta.inc: a full pro-style profile for every
extra playable skater listed in skater_roster.py (stats, stance, push style,
trick style, a default trick list of its own, 4 specials, a real deck +
griptape and a bail voice pitch).

usage: gen_skater_meta.py <decompiled qb dir (holds game/skater/*.txt)> <out .inc>

Everything random is seeded by the skater's key, so a rerun gives the same
skaters. Pro clones (tag pro:<name>) copy their pro's stats, specials, trick
list, stance and deck instead."""
import os, random, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from skater_roster import R, SILENT

QB = sys.argv[1]
OUT = sys.argv[2]
SK = os.path.join(QB, 'game', 'skater')

STATS = ['air', 'run', 'ollie', 'speed', 'spin', 'switch', 'flip_speed',
         'rail_balance', 'lip_balance', 'manual_balance']

# --- pros from skater_profile.txt ------------------------------------------
prof = open(os.path.join(SK, 'skater_profile.txt'), encoding='latin-1').read()
PROS = {}
for blk in re.split(r'\n\{\n', prof):
    m = re.search(r'^name = (\w+)$', blk, re.M)
    if not m:
        continue
    d = {'specials': re.findall(r'trickslot = (Sp\w+) trickname = (\w+)', blk)}
    for k in STATS + ['stance', 'pushstyle', 'trickstyle', 'default_trick_mapping']:
        mm = re.search(r'^%s = (\S+)$' % k, blk, re.M)
        if mm:
            d[k] = mm.group(1)
    PROS[m.group(1).lower()] = d

# decks the real pros' appearances use (cas_skater_m.qb)
# (campbell's own 'Campbell 1' graphic isn't in decks.qb: T.H.U.G. Starz instead)
PRO_DECK = {
    'hawk': ('BH Deck 3', 'Hawk 1'), 'koston': ('GR Deck 3', 'Koston 1'),
    'lasek': ('BH Deck 7', 'Generic 2'), 'margera': ('EL Deck 10', 'Generic 2'),
    'mullen': ('PR Deck 2', 'Mullen 1'), 'muska': ('PR Deck 17', 'Muska 1'),
    'reynolds': ('PR Deck 4', 'Generic 2'), 'thomas': ('ZR Deck 8', 'Thomas 1'),
    'vallely': ('EL Deck 9', 'Generic 1'), 'rodriguez': ('GR Deck 7', 'Generic 1'),
    'burnquist': ('PR Deck 11', 'Generic 2'), 'caballero': ('PR Deck 16', 'Caballero 1'),
    'campbell': ('CAS 12', 'Generic 2'), 'glifberg': ('FL Deck 10', 'Glifberg 1'),
}

# --- deck ids that exist -----------------------------------------------------
decks = open(os.path.join(QB, 'game', 'decks.txt'), encoding='latin-1').read()
DECK_IDS = set(re.findall(r'desc_id = ([^\n]+)\n', decks))
for p, (d, g) in PRO_DECK.items():
    assert d in DECK_IDS, d
    assert g in DECK_IDS, g

# Deck graphics by theme. 'CAS n' are the create-a-skater graphics (part
# cad_graphic), the rest are team decks (part deck_graphic).
THEMES = {
    'street': ['PR Deck 1', 'PR Deck 3', 'PR Deck 7', 'PR Deck 12', 'PR Deck 13', 'PR Deck 14',
               'PR Deck 15', 'GR Deck 1', 'GR Deck 2', 'GR Deck 4', 'GR Deck 6', 'CAS 10', 'CAS 23'],
    'punk': ['ZR Deck 4', 'ZR Deck 1', 'PR Deck 19', 'CAS 8', 'CAS 20', 'CAS 22'],
    'comic': ['BH Deck 4', 'BH Deck 6', 'BH Deck 8', 'FL Deck 4', 'FL Deck 5', 'CAS 13',
              'CAS 28', 'CAS 29', 'CAS 30'],
    'money': ['PR Deck 10', 'ZR Deck 10', 'CAS 12', 'CAS 25', 'CAS 35', 'CAS 15'],
    'dark': ['ZR Deck 2', 'ZR Deck 3', 'ZR Deck 5', 'BH Deck 5', 'FL Deck 9', 'CAS 16',
             'CAS 24', 'CAS 34', 'CAS 36'],
    'plain': ['GR Deck 3', 'GR Deck 10', 'EL Deck 7', 'PR Deck 8', 'CAS 26', 'CAS 27'],
    'army': ['CAS 19', 'CAS 33', 'ZR Deck 6', 'ZR Deck 8'],
    'classic': ['PR Deck 16', 'PR Deck 5', 'PR Deck 6', 'BH Deck 1', 'BH Deck 2', 'ZR Deck 7'],
    'beach': ['CAS 25', 'CAS 32', 'GR Deck 9', 'EL Deck 2', 'PR Deck 12'],
    'nature': ['EL Deck 2', 'EL Deck 4', 'ZR Deck 9', 'GR Deck 9', 'CAS 17'],
    'russia': ['CAS 1', 'CAS 33', 'CAS 19'],
    'music': ['CAS 4', 'CAS 15', 'CAS 18', 'CAS 31', 'GR Deck 5'],
    'fire': ['CAS 5', 'CAS 9', 'CAS 21', 'CAS 37', 'FL Deck 10', 'CAS 6'],
    'alien': ['CAS 7', 'CAS 3', 'CAS 6', 'FL Deck 1'],
    'western': ['CAS 27', 'GR Deck 10', 'ZR Deck 8', 'FL Deck 3', 'BH Deck 9', 'CAS 16',
                'CAS 24', 'CAS 31', 'FL Deck 8'],
    'asia': ['CAS 2', 'BH Deck 3', 'FL Deck 3', 'CAS 17'],
    'fast': ['CAS 11', 'BH Deck 10', 'FL Deck 7'],
    'hero': ['ZR Deck 7', 'ZR Deck 8', 'CAS 25', 'CAS 33', 'CAS 14'],
    'female': ['CAS 32', 'CAS 35', 'CAS 18', 'GR Deck 1', 'GR Deck 2', 'PR Deck 14'],
}
for t, l in THEMES.items():
    for d in l:
        assert d in DECK_IDS, (t, d)
GRIPS = ['Generic %d' % i for i in range(1, 16) if 'Generic %d' % i in DECK_IDS]

# --- regular tricks by slot kind (airtricks / liptricks) ---------------------
def tricks(kind_re):
    out = []
    for f in os.listdir(SK):
        for l in open(os.path.join(SK, f), encoding='latin-1'):
            m = re.match(r'^(Trick_\w+) = \{ Scr = (%s)\b.*?Score = (\d+)' % kind_re, l)
            if m and 'IsSpecial' not in l and 'Pressure' not in m.group(1) and int(m.group(3)) < 1000 \
               and m.group(1) not in ('Trick_BSToeFlip',):
                out.append(m.group(1))
    return sorted(set(out))
FLIPS, GRABS, LIPS = tricks('FlipTrick'), tricks('GrabTrick'), tricks('LipMacro2')
assert len(FLIPS) > 15 and len(GRABS) > 10 and len(LIPS) > 10, (FLIPS, GRABS, LIPS)

LIP_SLOTS = ['Lip_Triangle' + d for d in ('UL', 'UR', 'DL', 'DR', 'L', 'R', 'D', 'U')]
GRAB_SLOTS = ['Air_Circle' + d for d in ('UL', 'UR', 'DL', 'DR', 'U', 'D', 'R', 'L')]
FLIP_SLOTS = ['Air_Square' + d for d in ('UL', 'UR', 'DL', 'DR', 'U', 'D', 'L', 'R')] + \
             ['Air_U_U_Square', 'Air_D_D_Square']
# The ones every skater keeps where THUG's default lists put them, so the
# pad still feels like THUG (kickflip left, heelflip right, ...).
FIXED = {'Air_SquareL': 'Trick_Kickflip', 'Air_SquareR': 'Trick_Heelflip',
         'Air_CircleU': 'Trick_Nosegrab', 'Air_CircleL': 'Trick_Melon'}
VERT_LIPS = ['Trick_Invert', 'Trick_OneFootInvert', 'Trick_MuteInvert', 'Trick_Eggplant',
             'Trick_GymnastPlant', 'Trick_AndrectInvert', 'Trick_Switcheroo']

def mapping(rng, style):
    m = {}
    m.update(FIXED)
    fl = [t for t in FLIPS if t not in FIXED.values()]
    gr = [t for t in GRABS if t not in FIXED.values()]
    rng.shuffle(fl); rng.shuffle(gr)
    for s in FLIP_SLOTS:
        if s not in m:
            m[s] = fl.pop()
    for s in GRAB_SLOTS:
        if s not in m:
            m[s] = gr.pop()
    li = LIPS[:]
    rng.shuffle(li)
    if style == 'vert':  # vert skaters get the inverts first
        li.sort(key=lambda t: (t not in VERT_LIPS))
        li = li[:8]; rng.shuffle(li)
    for s in LIP_SLOTS:
        m[s] = li.pop()
    m['ExtraSlot1'] = m['ExtraSlot2'] = 'Trick_Revert'
    m['JumpSlot'] = rng.choice(['Trick_Boneless', 'Trick_Beanplant', 'Trick_Fastplant'])
    return '{ ' + ' '.join('%s = %s' % kv for kv in m.items()) + ' }'

# --- specials ----------------------------------------------------------------
BANNED = {'Trick_LickItUp', 'Trick_Flames', 'Trick_Scanning', 'Trick_BootBurst',
          'Trick_BlastGrind', 'Trick_3DSwimAir', 'Trick_3DScaryGrind'}
SPECIALS = {}  # kind -> [trick]
SLOTS = {}     # kind -> [slot]
for p, d in PROS.items():
    for slot, t in d['specials']:
        if slot == 'Unassigned' or t in BANNED:
            continue
        k = slot.split('_')[0]
        SPECIALS.setdefault(k, [])
        if t not in SPECIALS[k]:
            SPECIALS[k].append(t)
        s = slot[:-6] + 'Circle' if slot.endswith('circle') else slot
        SLOTS.setdefault(k, [])
        if s not in SLOTS[k]:
            SLOTS[k].append(s)

MIX = {'street': ['SpGrind', 'SpGrind', 'SpMan', 'SpAir'],
       'vert': ['SpAir', 'SpAir', 'SpLip', 'SpGrind'],
       'allround': ['SpAir', 'SpGrind', 'SpMan', 'SpAir']}

def specials(rng, style):
    kinds = MIX[style][:]
    if rng.random() < 0.35:  # not everyone fits the mould
        kinds[rng.randrange(4)] = rng.choice(['SpAir', 'SpGrind', 'SpMan', 'SpLip'])
    used_t, used_s, out = set(), set(), []
    for k in kinds:
        t = rng.choice([x for x in SPECIALS[k] if x not in used_t])
        s = rng.choice([x for x in SLOTS[k] if x not in used_s])
        used_t.add(t); used_s.add(s)
        out.append((s, t))
    return out

# --- stats -------------------------------------------------------------------
W_STYLE = {
    'street': dict(air=0.6, run=1, ollie=1.3, speed=1, spin=0.8, switch=1.3, flip_speed=1.3,
                   rail_balance=1.5, lip_balance=0.5, manual_balance=1.5),
    'vert': dict(air=1.6, run=0.8, ollie=1, speed=1.3, spin=1.6, switch=0.8, flip_speed=1,
                 rail_balance=0.7, lip_balance=1.6, manual_balance=0.6),
    'allround': {k: 1 for k in STATS},
}
BUILD = {  # stat total (pros are all 85), per-stat multipliers
    'athletic': (85, dict(speed=1.3, air=1.2, ollie=1.2)),
    'normal': (80, {}),
    'skinny': (80, dict(flip_speed=1.4, spin=1.3, speed=0.8)),
    'heavy': (74, dict(speed=0.7, spin=0.6, air=0.7, rail_balance=1.4, manual_balance=1.3, run=0.7)),
    'old': (70, dict(speed=0.7, air=0.6, spin=0.7, manual_balance=1.4, lip_balance=1.2, run=0.6)),
    'kid': (78, dict(spin=1.4, flip_speed=1.4, ollie=0.8, speed=0.9, run=1.4)),
}

def stats(rng, style, build):
    total, mul = BUILD[build]
    total += rng.randint(-2, 2)
    w = {k: W_STYLE[style][k] * mul.get(k, 1) * rng.uniform(0.6, 1.4) for k in STATS}
    v = {k: 3 for k in STATS}
    hi = 11 if build == 'athletic' else 10
    while sum(v.values()) < total:
        k = rng.choices(STATS, [w[s] if v[s] < hi else 0 for s in STATS])[0]
        v[k] += 1
    return [v[k] for k in STATS]

# --- voice pitch (percent; bail vocals are THUG's create-a-skater ones) -------
PITCH = {'kid': (124, 136), 'old': (86, 93), 'heavy': (86, 94), 'skinny': (101, 108),
         'athletic': (96, 103), 'normal': (95, 105)}

def qbcrc(s):  # Crc::GenerateCRCFromString
    import zlib
    return (zlib.crc32(s.lower().encode()) ^ 0xffffffff) & 0xffffffff

def esc(s):
    assert '"' not in s
    return s.replace('\\', '\\\\')

# Variants of one person (same display name) are one skater (Marcus): they all
# take the rolls of the first one listed (or its pro, if one is a pro clone),
# so they share stats, stance, tricks, specials, board and voice.
CANON = {}
for key, (name, first, age, town, tags) in R.items():
    if name not in CANON or (tags.startswith('pro:') and not CANON[name][1].startswith('pro:')):
        CANON[name] = (key, tags)

rows = []
for key, (name, first, age, town, tags) in R.items():
    ckey, tags = CANON[name]
    rng = random.Random('thug-skater:' + ckey)
    if tags.startswith('pro:'):
        p = tags[4:]
        d = PROS[p]
        st = [int(d[k]) for k in STATS]
        deck, grip = PRO_DECK[p]
        sp = [x for x in d['specials'] if x[0] != 'Unassigned']
        rows.append(dict(key=key, name=name, first=first, age=age, town=town,
                         stance=d['stance'], push=d['pushstyle'], style=d['trickstyle'],
                         stats=st, tricks=d['default_trick_mapping'], specials=sp,
                         deck=deck, grip=grip, pitch=0))
        continue
    style, build, theme = tags.split()
    st = stats(rng, style, build)
    trickstyle = style if style != 'allround' else rng.choice(['street', 'vert'])
    stance = 'goofy' if rng.random() < 0.4 else 'regular'
    r = rng.random()
    if build in ('old', 'heavy') or theme == 'comic':
        push = 'always_mongo' if r < 0.3 else ('mongo_when_switch' if r < 0.6 else 'never_mongo')
    else:
        push = 'always_mongo' if r < 0.07 else ('mongo_when_switch' if r < 0.35 else 'never_mongo')
    deck = rng.choice(THEMES[theme])
    grip = rng.choice(GRIPS)
    lo, hi = PITCH[build]
    rows.append(dict(key=key, name=name, first=first, age=age, town=town,
                     stance=stance, push=push, style=trickstyle, stats=st,
                     tricks=mapping(rng, trickstyle), specials=specials(rng, style),
                     deck=deck, grip=grip, pitch=0 if key in SILENT or ckey in SILENT else rng.randint(lo, hi)))

with open(OUT, 'w') as f:
    f.write('// Generated by desktop/tools/gen_skater_meta.py from skater_roster.py - do not edit.\n')
    f.write('// nom, display name, first name, age, hometown, stance, pushstyle, trickstyle,\n')
    f.write('// stats (%s), default trick list (a global or a struct),\n' % ' '.join(STATS))
    f.write('// specials, board parts, bail voice pitch (0 = none, like the real pros).\n')
    f.write('static const MetaSkater s_meta[] = {\n')
    for r in rows:
        sp = ' '.join('{ trickslot = %s trickname = %s }' % x for x in r['specials'])
        part = 'cad_graphic' if r['deck'].startswith('CAS ') else 'deck_graphic'
        # desc ids with spaces go in as their checksum (compile_q splits on spaces)
        board = 'board = { desc_id = Default } %s = { desc_id = #%08x } griptape = { desc_id = #%08x }' % (
            part, qbcrc(r['deck']), qbcrc(r['grip']))
        f.write('\t// %s: %s, griptape %s\n' % (r['name'], r['deck'], r['grip']))
        f.write('\t{ "%s", "%s", "%s", %d, "%s", "%s", "%s", "%s", { %s },\n\t  "%s",\n\t  "%s",\n\t  "%s", %d },\n' % (
            r['key'], esc(r['name']), esc(r['first']), r['age'], esc(r['town']), r['stance'], r['push'],
            r['style'], ', '.join(map(str, r['stats'])), r['tricks'], sp, board, r['pitch']))
    f.write('};\n')
print('%d skaters -> %s' % (len(rows), OUT))
print('flips', len(FLIPS), 'grabs', len(GRABS), 'lips', len(LIPS),
      'specials', {k: len(v) for k, v in SPECIALS.items()}, 'slots', {k: len(v) for k, v in SLOTS.items()})
