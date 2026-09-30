#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/datacache/mdlcache.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
old = 'InitReturnVal_t CMDLCache::Init()\n{'
assert old in s
s = s.replace(old, old + '\n\tfprintf( stderr, "CSGO_TRACE: mdlcache Init enter\\n" );', 1)
# 找 return
import re
m = re.search(r'\n\treturn [A-Za-z_]*;\n\}', s[s.index(old):])
if m:
    endpos = s.index(old) + m.end() - len('\n}')
    s = s[:endpos] + '\n\tfprintf( stderr, "CSGO_TRACE: mdlcache Init done\\n" );' + s[endpos:]
if '#include <cstdio>' not in s:
    s = s.replace('#include "mdlcache.h"', '#include "mdlcache.h"\n#include <cstdio>', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('mdlcache traced')
