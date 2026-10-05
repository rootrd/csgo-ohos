#!/usr/bin/env python3
"""Check every styles/scripts reference in loose panorama layouts resolves."""
import re, os, io

PANO = 'hap/entry/src/main/resources/rawfile/csgo/csgo/panorama'
refs = {}
for dirpath, _, files in os.walk(os.path.join(PANO, 'layout')):
    for fn in files:
        if not fn.endswith('.xml'):
            continue
        p = os.path.join(dirpath, fn)
        rel = os.path.relpath(p, os.path.join(PANO, 'layout')).replace(os.sep, '/')
        txt = io.open(p, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'(?:styles|scripts)/[A-Za-z0-9_/\.]+?\.(?:css|js)', txt):
            refs.setdefault(m.group(0), set()).add(rel)

missing = [r for r in sorted(refs) if not os.path.exists(os.path.join(PANO, r))]
print('referenced:', len(refs), 'missing:', len(missing))
for r in missing[:20]:
    print('MISSING', r, '<-', sorted(refs[r])[0])
