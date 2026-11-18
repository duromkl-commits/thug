"""Kingpin (1999) model reader: MDX, Xatrix's MD2 with sub-objects.
Vertex-animated, no skeleton. Coordinates stay in Quake units and axes."""
import struct
import numpy as np

HDR = '<4s22i'


def header(d):
    v = struct.unpack_from(HDR, d, 0)
    k = ('ident version skinW skinH frameSize nSkins nVerts nTris nGl nFrames nSfxDef nSfxEnt nSub '
         'oSkins oTris oFrames oGl oVInfo oSfxDef oSfxEnt oBBox oDummy oEnd').split()
    return dict(zip(k, v))


def frame_names(d):
    h = header(d)
    return [struct.unpack_from('<16s', d, h['oFrames'] + h['frameSize'] * f + 24)[0].split(b'\0')[0].decode('latin-1')
            for f in range(h['nFrames'])]


def skins(d):
    h = header(d)
    return [struct.unpack_from('<64s', d, h['oSkins'] + 64 * i)[0].split(b'\0')[0].decode('latin-1') for i in range(h['nSkins'])]


def read(d, frame=0):
    """-> positions (nV,3) for that frame, triangles [((v,s,t) x3)] from the GL commands
    (strips/fans, which carry the texture coordinates), and each vertex's sub-object
    (Kingpin's rigid pieces: torso, upper arm, forearm, hand / pelvis, thigh, shin, foot)."""
    h = header(d)
    o = h['oFrames'] + h['frameSize'] * frame
    sc = np.array(struct.unpack_from('<3f', d, o)); tr = np.array(struct.unpack_from('<3f', d, o + 12))
    raw = np.frombuffer(d, np.uint8, h['nVerts'] * 4, o + 40).reshape(-1, 4)[:, :3].astype(float)
    P = raw * sc + tr
    tris = []
    sub = np.zeros(h['nVerts'], int)
    p = h['oGl']; end = p + 4 * h['nGl']
    while p < end:
        n, = struct.unpack_from('<i', d, p); p += 4
        if n == 0:
            break
        so, = struct.unpack_from('<i', d, p); p += 4
        fan = n < 0; n = abs(n)
        pts = []
        for i in range(n):
            s, t, vi = struct.unpack_from('<ffi', d, p); p += 12
            pts.append((vi, s, t)); sub[vi] = so
        for i in range(n - 2):
            if fan:
                a, b, c = pts[0], pts[i + 1], pts[i + 2]
            elif i % 2 == 0:
                a, b, c = pts[i], pts[i + 1], pts[i + 2]
            else:
                a, b, c = pts[i + 1], pts[i], pts[i + 2]
            tris.append((a, b, c))
    return P, tris, sub
