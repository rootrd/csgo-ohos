#!/usr/bin/env python3
import io

W = '/root/csgo-src/CSGO-Source-Linux-20260928/src'

def add_enter(path, pattern, tag):
    fp = W + '/' + path
    s = io.open(fp, encoding='utf-8', errors='replace').read()
    if 'CSGO_TRACE' in s:
        print(path, 'already has trace')
        return
    m = re.search(pattern, s)
    if not m:
        print(path, 'PATTERN NOT FOUND:', pattern[:60])
        return
    s = s[:m.end()] + '\n\tfprintf( stderr, "CSGO_TRACE: ' + tag + ' enter\\n" );' + s[m.end():]
    if '#include <cstdio>' not in s:
        # 在首个 #include 后补 cstdio
        m2 = re.search(r'#include\s+[<"][^>"]+[>"]', s)
        s = s[:m2.end()] + '\n#include <cstdio>' + s[m2.end():]
    io.open(fp, 'w', encoding='utf-8', newline='').write(s)
    print(path, 'patched')

import re
add_enter('soundemittersystem/soundemittersystembase.cpp',
          r'(InitReturnVal_t CSoundEmitterSystem::Init\(\s*\)\s*\{)',
          'soundemittersystem')
add_enter('vscript/vscript.cpp',
          r'(InitReturnVal_t CVScriptManager::Init\(\s*\)\s*\{)',
          'vscript')
add_enter('vguimatsurface/CMatSystemSurface.cpp',
          r'(InitReturnVal_t CMatSystemSurface::Init\(\s*void\s*\)\s*\{)',
          'vguimatsurface')
