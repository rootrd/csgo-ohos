#!/usr/bin/env python3
"""给全部 HUD 骨架布局加 WindowRoot 包装层（根子面板不能有 id）"""
import io
import os
import re

OUT = '/mnt/e/csgo/CSGO-Source-Linux-20260928/ohos/overlay/csgo/panorama/layout/hud'
fixed = 0
for fn in os.listdir(OUT):
    if not fn.endswith('.xml'):
        continue
    p = os.path.join(OUT, fn)
    lines = io.open(p, encoding='utf-8', errors='replace').read().split('\n')
    # 已有包装层则跳过
    if any('WindowRoot' in L for L in lines):
        continue
    body = [L for L in lines if L.strip() and L.strip() not in ('<root>', '</root>')]
    if not body:
        continue
    out = ['<root>', '\t<Panel class="WindowRoot" hittest="false">']
    out += body
    out.append('\t</Panel>')
    out.append('</root>')
    io.open(p, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    fixed += 1
print('wrapped', fixed, 'layouts')
