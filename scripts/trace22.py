#!/usr/bin/env python3
"""给真实参与构建的 AppSystemGroup.cpp（大写）打点；之前打在了 wsl-sync 残留的小写副本上"""
import io

p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/appframework/AppSystemGroup.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()

# InitSystems：enter + done 成对打点（替换既有 Warning 或直接插入）
old2 = '''		InitReturnVal_t nRetVal = m_Systems[nSystemsInitialized]->Init();
		if ( nRetVal != INIT_OK )'''
new2 = '''		fprintf( stderr, "CSGO_TRACE: InitSys[%d] enter\\n", nSystemsInitialized );
		InitReturnVal_t nRetVal = m_Systems[nSystemsInitialized]->Init();
		fprintf( stderr, "CSGO_TRACE: InitSys[%d] done rv=%d\\n", nSystemsInitialized, (int)nRetVal );
		if ( nRetVal != INIT_OK )'''
assert old2 in s, 'InitSystems block not found in AppSystemGroup.cpp'
s = s.replace(old2, new2, 1)

# ConnectSystems：enter 打点
old1 = '''	for (int i = 0; i < m_Systems.Count(); ++i )
	{
		IAppSystem *pSystem = m_Systems[i];'''
new1 = '''	for (int i = 0; i < m_Systems.Count(); ++i )
	{
		IAppSystem *pSystem = m_Systems[i];
		fprintf( stderr, "CSGO_TRACE: ConnectSys[%d] enter\\n", i );'''
assert old1 in s, 'ConnectSystems block not found'
s = s.replace(old1, new1, 1)

if '#include <cstdio>' not in s:
    s = s.replace('#include "appframework/iappsystemgroup.h"', '#include "appframework/iappsystemgroup.h"\n#include <cstdio>', 1)
    if '#include <cstdio>' not in s:
        # 头文件结构不同就放到文件头
        s = '#include <cstdio>\n' + s
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('AppSystemGroup.cpp patched (real build file)')
