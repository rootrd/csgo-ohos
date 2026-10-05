#!/usr/bin/env python3
"""Compare deployed rawfile panorama layouts against the real pbin extraction.
Report: (a) identical, (b) different, (c) only-in-pbin, (d) only-in-rawfile."""
import os, io, hashlib

RAW = 'hap/entry/src/main/resources/rawfile/csgo/csgo/panorama/layout'
REAL = 'night-logs/pbin-real'

def load_map(root, strip):
    m = {}
    for dirpath, _, files in os.walk(root):
        for fn in files:
            if not fn.endswith('.xml'):
                continue
            p = os.path.join(dirpath, fn)
            rel = os.path.relpath(p, root).replace('\\', '/')
            key = rel if not strip else strip + rel
            m[key] = hashlib.md5(io.open(p, 'rb').read()).hexdigest()
    return m

raw = load_map(RAW, '')
real_raw = load_map(REAL, '')
real = {}
for k, v in real_raw.items():
    # names look like panorama_layout[_hud]_file.xml or panorama_layout_popups_x.xml
    n = k[len('panorama_layout_'):]
    if n.startswith('hud_'):
        rel = 'hud/' + n[len('hud_'):]
    elif n.startswith('popups_'):
        rel = 'popups/' + n[len('popups_'):]
    elif n.startswith('settings_'):
        rel = 'settings/' + n[len('settings_'):]
    else:
        rel = n
    real[rel] = v

same, diff, only_pbin, only_raw = [], [], [], []
for k in sorted(set(raw) | set(real)):
    if k in raw and k in real:
        (same if raw[k] == real[k] else diff).append(k)
    elif k in real:
        only_pbin.append(k)
    else:
        only_raw.append(k)

print('IDENTICAL (%d):' % len(same))
for k in same: print('  =', k)
print('DIFFERENT (%d):' % len(diff))
for k in diff: print('  D', k)
print('ONLY IN PBIN/REAL (%d):' % len(only_pbin))
for k in only_pbin: print('  P', k)
print('ONLY IN RAWFILE (%d):' % len(only_raw))
for k in only_raw: print('  R', k)
