#!/usr/bin/env python3
"""Bail voice banks for the extra skaters (data/voices).

usage: voice_banks.py <Sound Assets for THUG dir>[;<more sound dirs>...] <out voices dir>

Reads Marcus's sound folders (GTA SA generic male/female pain sounds, Red Dead
Revolver character injury lines; Kingdom Hearts and Simpsons hit lines from
the zips he sent 2026-11-10, unpacked anywhere and listed after the first
folder with os.pathsep, ';' on Windows, ':' elsewhere) and writes:
  <out>/gta_male/*.wav, gta_female/*.wav, rdr/<character>/*.wav,
      own/<character>/*.wav
      mono 16-bit, silence trimmed, peak normalised
  <out>/<skater key>.txt
      'pitch N' then one clip per line (relative to <out>); the game plays a
      random one on each bail (custom_music.cpp / p_music.cpp).
Characters with their own lines use those at their own pitch; skaters in
skater_roster.SILENT get no bank; everyone else gets 14 GTA pain clips (male
or female) at their profile pitch."""
import array, os, random, re, sys, wave
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from skater_roster import R, SILENT

SRCS, OUT = sys.argv[1].split(os.pathsep), sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))

# Red Dead characters with their own injury clips: key -> (folder, file regex)
OWN = {
    'anniestokes': ('Red Dead Revolver/Annie Stokes', r'^lginj'),
    'bloodytom': ('Red Dead Revolver/Bloody Tom', r'^lginj'),
    'buffalosoldier': ('Red Dead Revolver/Buffalo Soldier', r'^lginj'),
    'fallingstar': ('Red Dead Revolver/Falling Star', r'^lginj'),
    'generaldiego': ('Red Dead Revolver/General Javier Diego', r'^lginj'),
    'jackswift': ('Red Dead Revolver/Jack Swift', r'^lginj'),
    'mrblack': ('Red Dead Revolver/Mr. Black', r'^lginj'),
    'redharlow': ('Red Dead Revolver/Red Harlow', r'^final_lginj'),
    'youngred': ('Red Dead Revolver/Red Harlow/Young', r'^lginj'),
}
# Marcus's 2026-11-10 zips (Sora, Riku, the Simpsons): those skaters are gone
OWN2 = {}	# (Kingdom Hearts and Simpsons skaters removed 2026-11-10, Marcus)
# Return to Castle Wolfenstein: the game's own pain/death sounds (sound/player/*,
# sound/Zombie/* pulled out of the pk3s into an RTCW folder listed after the first)
_SS = [('RTCW/player/infantryss/pain', r''), ('RTCW/player/infantryss/death', r'')]
_EG = [('RTCW/player/eliteguard/pain', r''), ('RTCW/player/eliteguard/death', r'')]
_ZO = [('RTCW/Zombie/pain', r''), ('RTCW/Zombie/death', r'')]
RTCW = {'rtcw_bj2': [('RTCW/player/bj2', r'^(pain|death|fall\d)')],
        'rtcw_heinrich': [('RTCW/player/heinrich/pain', r''), ('RTCW/player/heinrich/death', r'')],
        'rtcw_warrior': [('RTCW/player/warrior/death', r'')] + _ZO[:1],
        'rtcw_dark': [('RTCW/player/warrior/death', r'')] + _ZO[:1],
        'rtcw_zombie': _ZO, 'rtcw_femzombie': _ZO,
        'rtcw_protosoldier': [('RTCW/player/supersoldier/death', r'')] + _SS[:1],
        'rtcw_supersoldier': [('RTCW/player/supersoldier/death', r'')] + _SS[:1]}
for k in ('eliteguard', 'eva', 'helga', 'inge', 'priestess'):
    RTCW['rtcw_' + k] = _EG
for k in ('blackguard', 'deathshead1', 'director', 'drz', 'hans', 'higgs', 'himmler', 'infantryss', 'jack',
          'mechanic', 'murphy', 'officerss', 'partisan', 'trench', 'venom', 'zemph'):
    RTCW['rtcw_' + k] = _SS
MIN_S, MAX_S = 0.15, 1.8
MAX_OWN = 3.0	# a character's own lines: Skinner's only one runs 2.8 s

def charge(path):
    w = wave.open(path)
    assert w.getsampwidth() == 2, path
    ch, hz = w.getnchannels(), w.getframerate()
    a = array.array('h', w.readframes(w.getnframes()))
    if ch == 2:
        a = array.array('h', [(a[i] + a[i + 1]) // 2 for i in range(0, len(a), 2)])
    return a, hz

def prepare(path, dst, max_s=MAX_S):
    """Trim silence, normalise, write mono 16-bit; returns seconds or None."""
    a, hz = charge(path)
    if not a:
        return None
    pk = max(abs(x) for x in a)
    if pk < 200:
        return None
    seuil = pk * 0.04
    i = next(k for k, x in enumerate(a) if abs(x) > seuil)
    j = len(a) - next(k for k, x in enumerate(reversed(a)) if abs(x) > seuil)
    i = max(0, i - hz // 100)
    j = min(len(a), j + hz // 20)
    a = a[i:j]
    s = len(a) / hz
    if s < MIN_S or s > max_s:
        return None
    g = 0.89 * 32767 / pk
    a = array.array('h', [max(-32768, min(32767, int(x * g))) for x in a])
    n = max(1, hz // 200)  # 5 ms fades
    for k in range(min(n, len(a))):
        a[k] = int(a[k] * k / n)
        a[-1 - k] = int(a[-1 - k] * k / n)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    w = wave.open(dst, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(hz)
    w.writeframes(a.tobytes())
    w.close()
    return s

def find(folder):
    for src in SRCS:
        if os.path.isdir(os.path.join(src, folder)):
            return os.path.join(src, folder)
    sys.exit('no %r under %s' % (folder, SRCS))

def pool(folder, rx, sous, max_s=MAX_S):
    out = []
    d = find(folder)
    for f in sorted(os.listdir(d)):
        if f.lower().endswith('.wav') and re.search(rx, f, re.I):
            rel = '%s/%s' % (sous, re.sub(r'[^a-z0-9_.-]+', '_', f.lower()))
            if prepare(os.path.join(d, f), os.path.join(OUT, rel), max_s) is not None:
                out.append(rel)
    return out

male = pool('GTA SA/Generic Male Pain Sounds', r'', 'gta_male')
female = pool('GTA SA/Generic Female Pain Sounds', r'', 'gta_female')
print('GTA pools: %d male, %d female clips kept' % (len(male), len(female)))

# women and profile pitches, from the game tables
cpp = open(os.path.join(HERE, '..', 'src', 'custom_music.cpp')).read()
femme = {k for k, f in re.findall(r'\{ "([^"]+)", "(?:[^"\\]|\\.)*", ([01])', cpp) if f == '1'}
meta = open(os.path.join(HERE, '..', 'src', 'skater_meta.inc')).read()
pitch = {}
for l in meta.split('\n'):
    m = re.match(r'\t\{ "([^"]+)",', l)
    if m:
        cle = m.group(1)
    m = re.match(r'\t  "board = .*", (\d+) \},$', l)
    if m:
        pitch[cle] = int(m.group(1))
assert len(pitch) == len(R), len(pitch)

own_n = 0
for key, (name, first, age, town, tags) in R.items():
    rng = random.Random('thug-voice:' + key)
    if key in SILENT:
        continue
    if key in RTCW:
        clips = []
        for d, rx in RTCW[key]:
            clips += pool(d, rx, 'own/' + d.lower().replace('/', '_'), MAX_OWN)
        # the same clips for a whole group: their profile pitch tells them apart
        p = 100 if key in ('rtcw_bj2', 'rtcw_heinrich') else pitch.get(key, 0) or rng.randint(96, 104)
        own_n += 1
    elif key in OWN or key in OWN2:
        d, rx = OWN[key] if key in OWN else OWN2[key]
        clips = pool(d, rx, ('rdr/' if key in OWN else 'own/') + key, MAX_OWN if key in OWN2 else MAX_S)
        p = 100
        own_n += 1
    else:
        clips = rng.sample(female if key in femme else male, 14)
        p = pitch.get(key, 0) or rng.randint(96, 104)  # pro lookalikes: their own pitch
    assert clips, key
    with open(os.path.join(OUT, key + '.txt'), 'w') as f:
        f.write('# %s: bail voice clips, one picked at random per bail\npitch %d\n' % (name, p))
        f.write('\n'.join(clips) + '\n')
print('%d skaters (%d with their own voice, %d silent) -> %s' % (len(R), own_n, len(SILENT), OUT))
