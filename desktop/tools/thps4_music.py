#!/usr/bin/env python3
"""THPS4 (Aspyr PC) soundtrack -> tagged .ogg files for the port's custom_music folder.

    thps4_music.py <THPS4 Game\\data> <out folder>

THPS4's music\\*.smo files are audio-only Bink. ffmpeg decodes them; each song
is written as Ogg Vorbis with artist, title and a genre tag that puts it in
THUG's Punk, Hip Hop or Rock/Other playlist group (THPS4 had no genres, so the
grouping is ours). The amb_* level ambience tracks are left out.
Personal use with your own copy of THPS4.
"""
import os, subprocess, sys

PUNK, HIPHOP, ROCK = 'Punk', 'Hip-Hop', 'Rock'

SONGS = {
    'ACDC':        ('AC/DC', 'T.N.T.', ROCK),
    'Aesop':       ('Aesop Rock', 'Labor', HIPHOP),
    'Agent':       ('Agent Orange', 'Bloodstains (Darkness Version)', PUNK),
    'Avail':       ('Avail', 'Simple Song', PUNK),
    'BizMarkie':   ('Muskabeatz feat. Biz Markie', 'Bodyrock', HIPHOP),
    'Bouncing':    ('The Bouncing Souls', 'Manthem', PUNK),
    'CityStars':   ('City Stars', 'Bad Dreams', PUNK),
    'DeLaSoul':    ('De La Soul', "Oodles of O's", HIPHOP),
    'Delinquenth': ('Delinquent Habits', 'House of the Rising Drum', HIPHOP),
    'distillers':  ('The Distillers', 'Seneca Falls', PUNK),
    'Eyedea':      ('Eyedea & Abilities', 'Big Shots', HIPHOP),
    'faction':     ('The Faction', 'Skate and Destroy', PUNK),
    'Floggingm':   ('Flogging Molly', 'Drunken Lullabies', PUNK),
    'GangStarr':   ('Gang Starr', 'Mass Appeal', HIPHOP),
    'Goldfinger':  ('Goldfinger', 'Spokesman', PUNK),
    'Haiku':       ("Haiku D'Etat", 'Non Compos Mentis', HIPHOP),
    'Hotwater':    ('Hot Water Music', 'Freightliner', PUNK),
    'IronM':       ('Iron Maiden', 'The Number of the Beast', ROCK),
    'Jeru':        ('Muskabeatz feat. Jeru the Damaja', 'Verses of Doom', HIPHOP),
    'JFA':         ('JFA', 'Beach Blanket Bongout', PUNK),
    'lessthanjake':('Less Than Jake', 'All My Best Friends Are Metalheads', PUNK),
    'Lootpack':    ('Lootpack', 'Whenimondamic', HIPHOP),
    'Lunchbox1':   ('Lunchbox Avenue', 'Everything and Anything', PUNK),
    'Lunchbox2':   ('Lunchbox Avenue', 'Standing Still', PUNK),
    'MelleMel':    ('Muskabeatz feat. Grandmaster Melle Mel', "I'm a Star", HIPHOP),
    'Nebula':      ('Nebula', 'Giant', ROCK),
    'NWA':         ('N.W.A', 'Express Yourself', HIPHOP),
    'Offspring':   ('The Offspring', 'Blackball', PUNK),
    'peArizona':   ('Public Enemy', 'By the Time I Get to Arizona', HIPHOP),
    'Rocket':      ('Rocket from the Crypt', 'Savoir Faire', PUNK),
    'rundmc':      ('Run-DMC', 'My Adidas', HIPHOP),
    'SexPistols':  ('Sex Pistols', 'Anarchy in the U.K.', PUNK),
    'System':      ('System of a Down', 'Shimmy', ROCK),
    'TheCult':     ('The Cult', 'Bad Fun', ROCK),
    'Toydolls':    ('Toy Dolls', 'Dig That Groove Baby', PUNK),
    'USBombs':     ('U.S. Bombs', 'Yer Country', PUNK),
    'zeke':        ('Zeke', 'Death Alley', PUNK),
}

if __name__ == '__main__':
    src, dst = sys.argv[1:3]
    music = next(os.path.join(src, d) for d in os.listdir(src) if d.lower() == 'music')
    files = {os.path.splitext(f)[0].lower(): os.path.join(music, f) for f in os.listdir(music)}
    os.makedirs(dst, exist_ok=True)
    n = 0
    for key, (artist, title, genre) in SONGS.items():
        f = files.get(key.lower())
        if not f:
            print('missing', key)
            continue
        out = os.path.join(dst, f'{artist} - {title}.ogg'.replace('/', '-').replace(':', ''))
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', f, '-vn', '-c:a', 'libvorbis', '-q:a', '5',
                        '-metadata', f'artist={artist}', '-metadata', f'album_artist={artist}',
                        '-metadata', f'title={title}', '-metadata', f'genre={genre}',
                        '-metadata', "album=Tony Hawk's Pro Skater 4", out], check=True)
        n += 1
    print(f'{n} songs written to {dst}')
