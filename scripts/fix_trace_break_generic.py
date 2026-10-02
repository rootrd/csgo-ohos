#!/usr/bin/env python3
"""通用修复：CSGO_TRACE 字符串里被 shell 层转成真实换行的 \n —— 合并回单行"""
import io
import re
import sys

p = sys.argv[1]
lines = io.open(p, encoding='utf-8', errors='replace').read().split('\n')
out = []
i = 0
fixed = 0
while i < len(lines):
    L = lines[i]
    # 特征：fprintf(...CSGO_TRACE: ... 行不以 " ); 结尾，且下一行以 " ); 开头
    if ('CSGO_TRACE:' in L and 'fprintf' in L
            and not L.rstrip().endswith(');')
            and i + 1 < len(lines)
            and lines[i + 1].lstrip().startswith('"')):
        merged = L.rstrip() + '\\n' + lines[i + 1].strip()
        out.append(merged)
        i += 2
        fixed += 1
        continue
    out.append(L)
    i += 1
io.open(p, 'w', encoding='utf-8', newline='').write('\n'.join(out))
print(f'{p}: fixed {fixed}')
