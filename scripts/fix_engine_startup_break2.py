#!/usr/bin/env python3
"""engine_startup.cpp 注入段残留行清理（WSL 内执行）"""
import io

p = '/mnt/e/csgo/CSGO-Source-Linux-20260928/android/native/engine_startup.cpp'
lines = io.open(p, encoding='utf-8', errors='replace').read().split('\n')

out = []
skip = 0
fixed = 0
for i, L in enumerate(lines):
    if skip:
        skip -= 1
        continue
    # 断裂特征：fprintf 行以 content=%s 结尾（无闭合），下一行以 ", n, cmdBuf 开头
    if 'read n=%zu content=%s' in L and L.rstrip().endswith('content=%s'):
        nxt = lines[i + 1] if i + 1 < len(lines) else ''
        merged = L.rstrip() + '\\n' + nxt.strip()
        out.append(merged)
        fixed += 1
        skip = 1
        continue
    # 残留行：n ? cmdBuf : "(empty)"); 单独一行
    if L.strip().startswith('n ? cmdBuf : "(empty)");'):
        skip = 1
        fixed += 1
        continue
    out.append(L)

io.open(p, 'w', encoding='utf-8', newline='').write('\n'.join(out))
print('fixed:', fixed)
