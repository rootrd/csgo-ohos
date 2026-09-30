#!/usr/bin/env python3
import io
# 1) datacache Init 打点
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/datacache/datacache.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
old = 'InitReturnVal_t CDataCache::Init( void )\n{'
assert old in s
s = s.replace(old, old + '\n\tfprintf( stderr, "CSGO_TRACE: datacache Init enter\\n" );', 1)
# 尾部（return BaseClass::Init()）
old2 = '\treturn BaseClass::Init();'
assert old2 in s
s = s.replace(old2, '\tfprintf( stderr, "CSGO_TRACE: datacache Init done\\n" );\n' + old2, 1)
if '#include <cstdio>' not in s:
    s = s.replace('#include "datacache.h"', '#include "datacache.h"\n#include <cstdio>', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('datacache traced')

# 2) launcher 的 Connect/InitSystems 打点补 fprintf（确认存在）
p2 = '/root/csgo-src/CSGO-Source-Linux-20260928/src/appframework/appsystemgroup.cpp'
s2 = io.open(p2, encoding='utf-8', errors='replace').read()
print('appsystemgroup traces:', s2.count('CSGO_TRACE'))
