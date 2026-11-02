#!/usr/bin/env python3
"""Case-insensitive includes for building THUG on Linux.

The engine was written on Windows: it includes <core/defines.h> for
Code/Core/Defines.h and so on. Run on a COPY of the sources (it adds
symlinks to it): every #include is resolved the way the compiler will look
for it; when only a different spelling exists, a symlink with the spelling
the code uses is created where the compiler looks first (next to the
including file for a relative hit, or in <out_dir>, which goes first on the
include path). Headers reached through a new link are scanned in turn, since
their own relative includes are looked up from the link's folder.

Windows builds don't need this.

usage: case_shadow.py <out_dir> <include_root>...
"""
import os, re, sys

out = os.path.abspath(sys.argv[1])
roots = [os.path.abspath(r) for r in sys.argv[2:]]
inc_re = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
os.makedirs(out, exist_ok=True)
search = roots + [out]


def ci_resolve(base, rel):
    cur = base
    for comp in rel.replace('\\', '/').split('/'):
        if comp in ('', '.'):
            continue
        if comp == '..':
            cur = os.path.dirname(cur)
            continue
        cand = os.path.join(cur, comp)
        if not os.path.exists(cand):
            try:
                names = os.listdir(cur)
            except OSError:
                return None
            low = comp.lower()
            m = sorted(n for n in names if n.lower() == low)
            if not m:
                return None
            cand = os.path.join(cur, m[0])
        cur = cand
    return os.path.realpath(cur) if os.path.isfile(cur) else None


def link(path, target):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if not os.path.lexists(path):
        os.symlink(target, path)


work = []
for root in roots:
    for dp, dn, fn in os.walk(root):
        for f in fn:
            if f.lower().endswith(('.c', '.cpp', '.h', '.hpp', '.inl', '.inc')):
                work.append(os.path.join(dp, f))

done = set()
made_rel = made_out = 0
while work:
    vpath = work.pop()
    if vpath in done:
        continue
    done.add(vpath)
    try:
        data = open(vpath, 'rb').read()
    except OSError:
        continue
    vdir = os.path.dirname(vpath)
    rdir = os.path.dirname(os.path.realpath(vpath))
    for m in inc_re.finditer(data):
        rel = m.group(1).decode('latin-1').replace('\\', '/')
        found = None
        for base in [vdir] + search:
            c = os.path.normpath(os.path.join(base, rel))
            if os.path.isfile(c):
                found = c
                break
        if found:
            if found not in done and found.startswith(tuple([out] + roots)):
                work.append(found)
            continue
        tgt = ci_resolve(rdir, rel) or ci_resolve(vdir, rel)
        if tgt:
            p = os.path.normpath(os.path.join(vdir, rel))
            link(p, tgt)
            made_rel += 1
            work.append(p)
            continue
        for r in roots:
            tgt = ci_resolve(r, rel)
            if tgt:
                p = os.path.normpath(os.path.join(out, rel))
                link(p, tgt)
                made_out += 1
                work.append(p)
                break

print(f"case_shadow: {made_out} links in {out}, {made_rel} links beside includers")
