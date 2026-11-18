"""Quake 1 .mdl ('IDPO' v6) reader: palette skins, seam-aware UVs, every frame."""
import struct
import numpy as np


def read(d, pal):
    """-> dict(skins [HxWx3 uint8], uv [(s,t) per (vert, back)], tris [(facesfront, (v0,v1,v2))],
    frames [(name, (nV,3) float)])"""
    assert d[:4] == b'IDPO', d[:4]
    (ver, sx, sy, sz, ox, oy, oz, rad, ex, ey, ez, nsk, sw, sh, nv, nt, nf, sync, flags, size) = \
        struct.unpack('<i10f7iif', d[4:84])
    o = 84
    skins = []
    for _ in range(nsk):
        typ, = struct.unpack('<i', d[o:o + 4]); o += 4
        if typ == 0:
            px = np.frombuffer(d, np.uint8, sw * sh, o); o += sw * sh
            skins.append(pal[px].reshape(sh, sw, 3))
        else:
            n, = struct.unpack('<i', d[o:o + 4]); o += 4 + 4 * n
            for k in range(n):
                px = np.frombuffer(d, np.uint8, sw * sh, o); o += sw * sh
                if k == 0:
                    skins.append(pal[px].reshape(sh, sw, 3))
    st = [struct.unpack('<3i', d[o + 12 * i:o + 12 * i + 12]) for i in range(nv)]; o += 12 * nv
    tris = [struct.unpack('<4i', d[o + 16 * i:o + 16 * i + 16]) for i in range(nt)]; o += 16 * nt
    scale = np.array([sx, sy, sz]); orig = np.array([ox, oy, oz])
    frames = []

    def simple(o):
        name = d[o + 8:o + 24].split(b'\0')[0].decode('latin1')
        v = np.frombuffer(d, np.uint8, nv * 4, o + 24).reshape(nv, 4)[:, :3].astype(float)
        return name, v * scale + orig, o + 24 + nv * 4
    for _ in range(nf):
        typ, = struct.unpack('<i', d[o:o + 4]); o += 4
        if typ == 0:
            name, P, o = simple(o)
            frames.append((name, P))
        else:
            n, = struct.unpack('<i', d[o:o + 4]); o += 4 + 8 + 4 * n
            for k in range(n):
                name, P, o = simple(o)
                frames.append((name, P))
    T = []
    for ff, a, b, c in tris:
        corners = []
        for v in (a, b, c):
            onseam, s, t = st[v]
            if onseam and not ff:
                s += sw // 2
            corners.append((v, (s + 0.5) / sw, (t + 0.5) / sh))
        T.append(corners)
    return dict(skins=skins, tris=T, frames=frames, size=(sw, sh))


def palette(lmp):
    return np.frombuffer(lmp, np.uint8, 768).reshape(256, 3)


def read_md2(d):
    """Quake 2 .md2 ('IDP2' v8) -> dict(skins [names], tris [[(v, s, t) x3]], frames, size).
    Skins are separate .pcx files, so only their names come back."""
    assert d[:4] == b'IDP2', d[:4]
    (ver, sw, sh, fsize, nsk, nv, nst, nt, ngl, nf, osk, ost, otr, ofr, ogl, oend) = struct.unpack('<16i', d[4:68])
    skins = [d[osk + 64 * i:osk + 64 * i + 64].split(b'\0')[0].decode('latin1') for i in range(nsk)]
    st = np.frombuffer(d, '<i2', nst * 2, ost).reshape(nst, 2).astype(float)
    tr = np.frombuffer(d, '<i2', nt * 6, otr).reshape(nt, 6)
    frames = []
    for i in range(nf):
        o = ofr + fsize * i
        sc = np.array(struct.unpack('<3f', d[o:o + 12])); tl = np.array(struct.unpack('<3f', d[o + 12:o + 24]))
        name = d[o + 24:o + 40].split(b'\0')[0].decode('latin1')
        v = np.frombuffer(d, np.uint8, nv * 4, o + 40).reshape(nv, 4)[:, :3].astype(float)
        frames.append((name, v * sc + tl))
    T = [[(int(t[k]), st[t[3 + k], 0] / sw, st[t[3 + k], 1] / sh) for k in range(3)] for t in tr]
    return dict(skins=skins, tris=T, frames=frames, size=(sw, sh))
