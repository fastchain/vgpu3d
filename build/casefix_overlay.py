#!/usr/bin/env python3
"""Create a flat overlay dir of symlinks so that #include "Foo.h" finds foo.h on a case-sensitive FS.
usage: casefix_overlay.py OUTDIR SRCROOT... ; scans SRCROOTs for includes whose exact spelling
does not exist anywhere under the SRCROOTs but whose lowercase form matches exactly one file."""
import os, re, sys
out = os.path.abspath(sys.argv[1]); roots = [os.path.abspath(r) for r in sys.argv[2:]]
by_lower = {}; exact = set()
for r in roots:
    for d, _, fs in os.walk(r):
        for f in fs:
            by_lower.setdefault(f.lower(), []).append(os.path.join(d, f)); exact.add(f)
inc = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
os.makedirs(out, exist_ok=True)
made = 0
for r in roots:
    for d, _, fs in os.walk(r):
        for f in fs:
            if not f.lower().endswith(('.h', '.hpp', '.c', '.cpp', '.inl')): continue
            for m in inc.finditer(open(os.path.join(d, f), 'rb').read()):
                name = m.group(1).decode('latin1').replace('\\', '/')
                base = os.path.basename(name)
                if base in exact: continue
                c = by_lower.get(base.lower())
                if c and len(set(c)) == 1:
                    link = os.path.join(out, base)
                    if not os.path.lexists(link):
                        os.symlink(c[0], link); made += 1
print("overlay symlinks:", made)
