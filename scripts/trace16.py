#!/usr/bin/env python3
import io, re

W = '/root/csgo-src/CSGO-Source-Linux-20260928/src'

p = W + '/materialsystem/cmaterialsystem.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'FindProceduralMaterial enter' not in s:
    m = re.search(r'(IMaterial\s*\*\s*CMaterialSystem::FindProceduralMaterial\s*\([^)]*\)\s*\n\{)', s)
    assert m, 'FindProceduralMaterial def not found'
    s = s[:m.end()] + '\n\tfprintf( stderr, "CSGO_TRACE: FindProceduralMaterial enter\\n" );' + s[m.end():]
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('cmaterialsystem FindProceduralMaterial patched')
else:
    print('already')
