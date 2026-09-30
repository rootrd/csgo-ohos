#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/studiorender/studiorender.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
start = s.index('InitReturnVal_t CStudioRender::Init()\n{')
end = s.index('\n}\n', start) + 3
body = s[start:end]
lines = body.split('\n')
newlines = []
for i, line in enumerate(lines):
    newlines.append(line)
    if i > 0 and i % 20 == 0 and 'CSGO_TRACE' not in line:
        newlines.append('\tfprintf( stderr, "CSGO_TRACE: studiorender Init +%d\\n" );' % i)
s2 = s[:start] + '\n'.join(newlines) + s[end:]
io.open(p, 'w', encoding='utf-8', newline='').write(s2)
print('progress traces added, total lines:', len(newlines))
