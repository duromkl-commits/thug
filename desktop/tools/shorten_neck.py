#!/usr/bin/env python3
"""Shorten a converted skater's neck in place (GTA Vice City models: their
shoulders sit ~3-5 in lower under the head than a THUG pro's).

    shorten_neck.py <skin.xbx> [target inches, default 2]

Measures chin (lowest front head vertex) to shoulder top (torso vertices at
x = +-6 in); if that's over target + 0.5, the head and neck move down so it's
target, the neck squashed linearly in between. Only head, neck and torso
vertices move; arms are left alone."""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import thug_scene as TS  # noqa: E402

TORSO = [1, 2, 3, 4, 5, 17]
UPPER = set(range(29, 39)) | {4, 5, 17}	# neck, head, head props, chest, collars


def dom(w):
    a = [(w & 0x7ff) / 1023, ((w >> 11) & 0x7ff) / 1023, (w >> 22) / 511]
    return int(np.argmax(a))


def shorten(fn, target=2.0):
    d = bytearray(open(fn, 'rb').read())
    r = TS.R(bytes(d)); r.t('3I'); TS.read_materials(r)
    assert r.one('i') == 1
    st = r.o
    _, _, flags, _ = r.t('IiiI'); r.t('6f'); r.t('4f')
    if flags & 0x800000: r.t('I9f')
    nv, _ = r.t('ii'); po = r.o
    s = TS.read_sector(TS.R(bytes(d), st))
    P = np.array(s['pos']); D = np.array([bi[dom(w)] for w, bi in zip(s['w'], s['bi'])])
    head = P[D == 30]; front = head[(head[:, 2] > 2) & (abs(head[:, 0]) < 2)]
    if not len(front):
        return None
    yc = front[:, 1].min()
    body = P[np.isin(D, TORSO)]
    ys = body[abs(abs(body[:, 0]) - 6) < 1.5][:, 1].max()
    neck = yc - ys
    if neck <= target + 0.5:
        return neck, 0.0
    dy = neck - target
    for i in range(nv):
        if D[i] not in UPPER or P[i, 1] <= ys:
            continue
        y = P[i, 1]
        P[i, 1] = y - dy if y >= yc else y - dy * (y - ys) / (yc - ys)
    d[po:po + 12 * nv] = P.astype('<f4').tobytes()
    open(fn, 'wb').write(d)
    return neck, dy


if __name__ == '__main__':
    print(sys.argv[1], shorten(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 2.0))
