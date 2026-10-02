#!/usr/bin/env python3
"""批量生成缺失 HUD 子布局：扫 hud 元件 cpp 的 RequireChildInLayoutFile，
生成含全部必需子面板的骨架 xml（跳过已存在的）"""
import io
import os
import re

SRC = '/mnt/e/csgo/CSGO-Source-Linux-20260928/src/game/client/cstrike15/panorama/hud'
OUT = '/mnt/e/csgo/CSGO-Source-Linux-20260928/ohos/overlay/csgo/panorama/layout/hud'
gen = 0
for cpp in sorted(os.listdir(SRC)):
    if not (cpp.startswith('csgo_hud') and cpp.endswith('.cpp')):
        continue
    src = io.open(os.path.join(SRC, cpp), errors='replace').read()
    m = re.search(r'layout/hud/([a-z_0-9]+\.xml)', src)
    if not m:
        continue
    xml = m.group(1)
    out = os.path.join(OUT, xml)
    if os.path.exists(out):
        continue
    req = re.findall(r'RequireChildInLayoutFile\( "([A-Za-z0-9_]+)" \)', src)
    with io.open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('<root>\n')
        for rid in req:
            f.write('\t<Panel id="%s"></Panel>\n' % rid)
        f.write('</root>\n')
    gen += 1
    print(xml, '->', len(req), 'required')
print('generated', gen)
