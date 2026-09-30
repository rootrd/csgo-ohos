#!/usr/bin/env python3
import io, re
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/studiorender/studiorender.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
start = s.index('void CStudioRender::InitDebugMaterials( void )\n{')
end = s.index('\n}\n', start) + 3
body = s[start:end]
lines = body.split('\n')
out = []
n = 0
for line in lines:
    if 'FindMaterial(' in line:
        # 提取材质名
        m = re.search(r'FindMaterial\(\s*"([^"]*)"', line)
        name = m.group(1) if m else 'mat%d' % n
        out.append('\tfprintf( stderr, "CSGO_TRACE: FindMaterial %s...\\n" );' % name)
        n += 1
    out.append(line)
s2 = s[:start] + '\n'.join(out) + s[end:]
io.open(p, 'w', encoding='utf-8', newline='').write(s2)
print('material find traces added:', n)
