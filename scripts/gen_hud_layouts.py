#!/usr/bin/env python3
"""批量生成 HUD 元件子布局骨架：
扫描 csgo_hud*.cpp 的 FindChild/RequireChild id，生成含全部必需子面板的 xml。
根结构模仿 CS:GO 原版（root > styles? + Panel 包装 > 子面板）。"""
import io
import os
import re

SRC = '/mnt/e/csgo/CSGO-Source-Linux-20260928/src/game/client/cstrike15/panorama/hud'
OUT = '/mnt/e/csgo/CSGO-Source-Linux-20260928/ohos/overlay/csgo/panorama/layout/hud'
# 种子/overlay 只覆盖部分；生成的放 overlay 即可

generated = 0
for cpp in sorted(os.listdir(SRC)):
    if not cpp.startswith('csgo_hud') or not cpp.endswith('.cpp'):
        continue
    src_path = os.path.join(SRC, cpp)
    src = io.open(src_path, encoding='utf-8', errors='replace').read()

    # 该元件的子布局文件名（RequireLoadLayout/LoadLayout 引用）
    m = re.search(r'layout/hud/([a-z_]+\.xml)', src)
    if not m:
        continue
    xml_name = m.group(1)
    out_path = os.path.join(OUT, xml_name)
    if os.path.exists(out_path):
        continue  # 已有（我们手写或之前生成的不覆盖）

    # 收集该 cpp 里的子面板 id（FindChild/RequireChild 的字符串参数）
    ids = []
    for mm in re.finditer(r'(?:RequireChild|FindChild)(?:InLayoutFile)?\( "([A-Za-z0-9_]+)" \)', src):
        if mm.group(1) not in ids:
            ids.append(mm.group(1))

    with io.open(out_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('<root>\n')
        f.write('\t<Panel hittest="false">\n')
        for pid in ids:
            f.write(f'\t\t<Panel id="{pid}"></Panel>\n')
        f.write('\t</Panel>\n')
        f.write('</root>\n')
    generated += 1
    print(f'{xml_name}: {len(ids)} children')

print('generated', generated, 'skeleton layouts')
