#!/usr/bin/env python3
import io, re

W = '/root/csgo-src/CSGO-Source-Linux-20260928/src'

# 1) 修 mdlcache done 位置（原插在 return 后的死代码区）
p = W + '/datacache/mdlcache.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
dead = '\treturn INIT_OK;\n\tfprintf( stderr, "CSGO_TRACE: mdlcache Init done\\n" );\n}'
if dead in s:
    s = s.replace(dead, '\tfprintf( stderr, "CSGO_TRACE: mdlcache Init done\\n" );\n\treturn INIT_OK;\n}', 1)
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('mdlcache done 位置已修正')

# 2) 给后续系统的 Init 统一加 enter 打点（幂等）
targets = [
    ('studiorender/studiorender.cpp',   r'CreateInterfaceFn\s*\*?|bool\s+.*Init\(\)', None),
]
def add_enter(path, pattern, name, fmt_line):
    fp = W + '/' + path
    s = io.open(fp, encoding='utf-8', errors='replace').read()
    if name in s:
        print(path, 'already'); return
    m = re.search(pattern, s)
    if not m:
        print(path, 'PATTERN NOT FOUND'); return
    s = s[:m.end()] + '\n\t' + fmt_line + s[m.end():]
    io.open(fp, 'w', encoding='utf-8', newline='').write(s)
    print(path, 'patched')

# studiorender
p = W + '/studiorender/studiorender.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'CSGO_TRACE' not in s:
    m = re.search(r'(InitReturnVal_t CStudioRender::Init\(\)\s*\{)', s)
    if m:
        s = s.replace(m.group(1), m.group(1) + '\n\tfprintf( stderr, "CSGO_TRACE: studiorender Init enter\\n" );', 1)
        m2 = re.search(r'InitReturnVal_t CStudioRender::Init\(\)\s*\{[^}]*', s)
        io.open(p, 'w', encoding='utf-8', newline='').write(s)
        print('studiorender enter added')

# soundemittersystem
p = W + '/soundemittersystem/soundemittersystem.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'CSGO_TRACE' not in s:
    m = re.search(r'(InitReturnVal_t CSoundEmitterSystem::Init\(\s*\)\s*\{)', s)
    if m:
        s = s.replace(m.group(1), m.group(1) + '\n\tfprintf( stderr, "CSGO_TRACE: soundemittersystem Init enter\\n" );', 1)
        io.open(p, 'w', encoding='utf-8', newline='').write(s)
        print('soundemittersystem enter added')

# vscript
p = W + '/vscript/vscript.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'CSGO_TRACE' not in s:
    m = re.search(r'(InitReturnVal_t CVScriptManager::Init\(\)\s*\{)', s)
    if m:
        s = s.replace(m.group(1), m.group(1) + '\n\tfprintf( stderr, "CSGO_TRACE: vscript Init enter\\n" );', 1)
        io.open(p, 'w', encoding='utf-8', newline='').write(s)
        print('vscript enter added')

# vguimatsurface
p = W + '/vguimatsurface/CMatSystemSurface.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'CSGO_TRACE' not in s:
    m = re.search(r'(InitReturnVal_t CMatSystemSurface::Init\(void\)\s*\{)', s)
    if m:
        s = s.replace(m.group(1), m.group(1) + '\n\tfprintf( stderr, "CSGO_TRACE: vguimatsurface Init enter\\n" );', 1)
        io.open(p, 'w', encoding='utf-8', newline='').write(s)
        print('vguimatsurface enter added')

# vgui2
p = W + '/vgui2/src/vgui2.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'CSGO_TRACE' not in s:
    m = re.search(r'(InitReturnVal_t VGUI2::Init\(.*?\)\s*\{)', s)
    if m:
        s = s.replace(m.group(1), m.group(1) + '\n\tfprintf( stderr, "CSGO_TRACE: vgui2 Init enter\\n" );', 1)
        io.open(p, 'w', encoding='utf-8', newline='').write(s)
        print('vgui2 enter added')
