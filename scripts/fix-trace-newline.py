#!/usr/bin/env python3
"""修复 trace20 补丁被 shell 转义层破坏的字符串字面量（\n 变成了真实换行）"""
import io
import re

paths = [
    '/root/csgo-src/CSGO-Source-Linux-20260928/src/vguimatsurface/MatSystemSurface.cpp',
    '/root/csgo-src/CSGO-Source-Linux-20260928/src/vgui2/src/vgui.cpp',
    '/root/csgo-src/CSGO-Source-Linux-20260928/src/panorama/source2/panoramauiengine.cpp',
]
pat = re.compile(r'(fprintf\( stderr, "CSGO_TRACE: [^"]*)\n(",)')
pat2 = re.compile(r'(fprintf\( stderr, "CSGO_TRACE: [^"]*)\n("\);)')
for p in paths:
    s = io.open(p, encoding='utf-8', errors='replace').read()
    s2, n = pat.subn(r'\1\\n\2', s)
    s2, n2 = pat2.subn(r'\1\\n\2', s2)
    io.open(p, 'w', encoding='utf-8', newline='').write(s2)
    print(f'{p}: fixed {n + n2} broken literals')
