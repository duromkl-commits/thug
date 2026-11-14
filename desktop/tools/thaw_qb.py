"""ReTHAWed / THAW PC .qb.wpc reader (item-tree qb, scripts LZSS compressed with THUG tokens)."""
import struct

def crc(s):
    c = 0xffffffff
    for ch in s.lower().replace('/', '\\').encode('latin-1'):
        c ^= ch
        for _ in range(8):
            c = (c >> 1) ^ (0xedb88320 if c & 1 else 0)
    return c

def lzss(src, n):
    # Ring starts full of spaces, as in Neversoft's encoder (Code/Core/compress.cpp).
    ring = bytearray(b" " * 4096); r = 4078; out = bytearray(); i = 0; flags = 0
    while len(out) < n and i < len(src):
        flags >>= 1
        if not flags & 0x100:
            flags = src[i] | 0xff00; i += 1
        if flags & 1:
            c = src[i]; i += 1; out.append(c); ring[r] = c; r = (r + 1) & 4095
        else:
            if i + 1 >= len(src): break
            a, b = src[i], src[i+1]; i += 2
            off = a | ((b & 0xf0) << 4); ln = (b & 15) + 3
            for k in range(ln):
                c = ring[(off + k) & 4095]; out.append(c); ring[r] = c; r = (r + 1) & 4095
    return bytes(out[:n])

class Key(int):
    pass

class Struct(list):
    pass

class Script:
    def __init__(s, crc_, code): s.crc = crc_; s.code = code

class QB:
    def __init__(s, data, root=True):
        s.d = data; s.items = []  # (type, crc, value)
        s.top = 0
        if not root:
            return
        o = 0x1c
        end = struct.unpack_from('<I', data, 4)[0]
        while o + 20 <= min(end, len(data)):
            t, i, f, do, nx = struct.unpack_from('<5I', data, o)
            s.file_id = f
            s.top = o + 20
            s.items.append(((t >> 16) & 0xff, i, s.value((t >> 16) & 0xff, do, root=True)))
            o = nx if nx else (s.top + 3) & ~3

    def u(s, o):
        s.top = max(s.top, o + 4)
        return struct.unpack_from('<I', s.d, o)[0]

    def mark(s, e): s.top = max(s.top, e)

    def value(s, tb, v, root=False):
        d = s.d
        if root:
            # root items: types are base types, v is offset to data
            if tb == 7:
                ucrc, usz, csz = struct.unpack_from('<3I', d, v)
                body = d[v+12:v+12+csz]
                code = body if usz == csz else lzss(body, usz)
                s.mark(v + 12 + csz)
                return Script(ucrc, code)
            if tb in (1, 2, 0x0d): return s.simple(tb, v)
            return s.data(tb, v)
        return s.simple(tb, v)

    def simple(s, tb, v):
        if tb == 1: return struct.unpack('<i', struct.pack('<I', v))[0]
        if tb == 2: return struct.unpack('<f', struct.pack('<I', v))[0]
        if tb == 0x0d: return Key(v)
        if tb in (0x1a, 0x1b, 0x1c): return Key(v)
        return s.data(tb, v)

    def data(s, tb, o):
        d = s.d
        if tb == 3:
            e = d.index(0, o); s.mark(e + 1)
            return d[o:e].decode('latin-1')
        if tb == 4:
            e = o
            while d[e:e+2] != b'\0\0': e += 2
            s.mark(e + 2)
            return d[o:e].decode('utf-16-le')
        if tb == 5:
            s.mark(o + 12); return struct.unpack_from('<2f', d, o + 4)
        if tb == 6:
            s.mark(o + 16); return struct.unpack_from('<3f', d, o + 4)
        if tb == 0x0a:
            return s.struct_(o)
        if tb == 0x0c:
            return s.array(o)
        raise ValueError('type %x at %x' % (tb, o))

    def struct_(s, o):
        res = []
        p = s.u(o + 4)
        while p:
            t, n, v, nx = struct.unpack_from('<4I', s.d, p); s.mark(p + 16)
            tb = ((t >> 8) & 0xff) >> 1
            res.append((n, s.simple(tb, v) if tb in (1, 2, 0x0d, 0x1a, 0x1b, 0x1c) else s.data(tb, v)))
            p = nx
        return Struct(res)

    def array(s, o):
        h, cnt = struct.unpack_from('<2I', s.d, o); s.mark(o + 12)
        et = (h >> 16) & 0xff
        if cnt == 0: return []
        if cnt == 1: offs = [s.u(o + 8)]
        else:
            lo = s.u(o + 8); offs = list(struct.unpack_from('<%dI' % cnt, s.d, lo)); s.mark(lo + 4 * cnt)
        if et in (1, 2, 0x0d, 0x1a, 0x1b, 0x1c):
            return [s.simple(et, x) for x in offs]
        return [s.data(et, x) for x in offs]


# ---- script bytecode -------------------------------------------------------
# THUG's tokens plus THAW's short-offset ones: 0x47 if, 0x48 else, 0x49 jump
# (u16 offsets, rewritten to THUG's plain if/else for THUG).

KW = {1: '\n', 3: '{', 4: '}', 5: '[', 6: ']', 7: '=', 8: '.', 9: ',', 10: '-', 11: '+', 12: '/', 13: '*', 14: '(', 15: ')',
      17: '==', 18: '<', 19: '<=', 20: '>', 21: '>=', 0x20: 'begin', 0x21: 'repeat', 0x22: 'break', 0x23: 'script',
      0x24: 'endscript', 0x25: 'if', 0x26: 'else', 0x27: 'elseif', 0x28: 'endif', 0x29: 'return', 0x2c: '<...>', 0x2d: '<arg>',
      0x31: '@', 0x32: '||', 0x33: '&&', 0x39: 'NOT', 0x3a: 'AND', 0x3b: 'OR', 0x3c: 'switch', 0x3d: 'endswitch',
      0x3e: 'case', 0x3f: 'default', 0x42: ':', 0x47: 'if', 0x48: 'else', 0x49: 'jump16'}


def packed_start(i):
    return (i + 3 + 3) & ~3


def tok_len(d, i):
    t = d[i]
    if t == 0x4a:				# inline packed struct: u16 size, pad to 4, qb struct
        n = struct.unpack_from('<H', d, i + 1)[0]
        return packed_start(i) + n - i
    if t == 0x4b: return 1			# argument pack marker
    if t == 0x4c: return 5 + struct.unpack_from('<I', d, i + 1)[0]	# wide string (bytes)
    if t in (0x16, 0x17, 0x18, 0x1a, 0x02, 0x2e, 0x43, 0x44, 0x19): return 5
    if t == 0x1e: return 13
    if t == 0x1f: return 9
    if t in (0x47, 0x48, 0x49): return 3
    if t in (0x1b, 0x1c): return 5 + struct.unpack_from('<I', d, i + 1)[0]
    if t == 0x2b:
        j = i + 5
        while d[j]: j += 1
        return j + 1 - i
    if t in (0x2f, 0x37, 0x40, 0x41):
        n = struct.unpack_from('<I', d, i + 1)[0]; return 5 + 6 * n
    if t <= 0x46: return 1
    raise ValueError('token %#x at %d' % (t, i))


def tokens(d):
    i = 0
    while i < len(d):
        n = tok_len(d, i)
        yield i, d[i], d[i:i + n]
        i += n


def decompile(d, names={}):
    nm = lambda c: names.get(c, '#%08x' % c)
    out = []
    for i, t, b in tokens(d):
        if t == 0x16: out.append(nm(struct.unpack_from('<I', b, 1)[0]))
        elif t == 0x17: out.append(str(struct.unpack_from('<i', b, 1)[0]))
        elif t == 0x18: out.append(hex(struct.unpack_from('<I', b, 1)[0]))
        elif t == 0x1a: out.append('%g' % struct.unpack_from('<f', b, 1)[0])
        elif t == 0x1e: out.append('(%g,%g,%g)' % struct.unpack_from('<3f', b, 1))
        elif t == 0x1f: out.append('(%g,%g)' % struct.unpack_from('<2f', b, 1))
        elif t in (0x1b, 0x1c): out.append(('"%s"' if t == 0x1b else "'%s'") % b[5:-1].decode('latin-1'))
        elif t in KW: out.append(KW[t])
        elif t == 0x4a: out.append(repr(packed_struct(d, i)))
        elif t == 0x4c: out.append('"%s"' % b[5:].decode('utf-16-le').rstrip('\0'))
        elif t == 0x2e: out.append('jump')
        elif t == 0x2f: out.append('Random(%d)' % struct.unpack_from('<I', b, 1)[0])
        else: out.append('<tok%#x>' % t)
    import re
    s = ' '.join(out)
    return re.sub(r' *\n *', '\n', s).strip()


def packed_struct(d, i):
    """The qb struct behind a 0x4a token (offsets relative to its start)."""
    a = packed_start(i)
    n = struct.unpack_from('<H', d, i + 1)[0]
    q = QB(d[a:a + n], root=False)
    return q.struct_(0)
