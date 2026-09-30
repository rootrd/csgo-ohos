#!/usr/bin/env python3
"""v3 bisect: per-system Init enter/done (stderr) + CSourceAppSystemGroup::Main entry"""
import io

# 1) appsystemgroup.cpp: stderr 打点带系统索引与返回值
p1 = '/root/csgo-src/CSGO-Source-Linux-20260928/src/appframework/appsystemgroup.cpp'
s = io.open(p1, encoding='utf-8', errors='replace').read()

old1 = '''		IAppSystem *pSystem = m_Systems[i];
		Warning( "CSGO_TRACE: Connect system[%d]\\n", i );'''
new1 = '''		IAppSystem *pSystem = m_Systems[i];
		fprintf( stderr, "CSGO_TRACE: ConnectSys[%d] enter\\n", i );'''
assert old1 in s, 'ConnectSystems block not found'
s = s.replace(old1, new1, 1)

old2 = '''		Warning( "CSGO_TRACE: Init system[%d]\\n", nSystemsInitialized );
		InitReturnVal_t nRetVal = m_Systems[nSystemsInitialized]->Init();
		if ( nRetVal != INIT_OK )'''
new2 = '''		fprintf( stderr, "CSGO_TRACE: InitSys[%d] enter\\n", nSystemsInitialized );
		InitReturnVal_t nRetVal = m_Systems[nSystemsInitialized]->Init();
		fprintf( stderr, "CSGO_TRACE: InitSys[%d] done rv=%d\\n", nSystemsInitialized, (int)nRetVal );
		if ( nRetVal != INIT_OK )'''
assert old2 in s, 'InitSystems block not found'
s = s.replace(old2, new2, 1)

if '#include <cstdio>' not in s:
    s = s.replace('#include "appframework/iappsystemgroup.h"', '#include "appframework/iappsystemgroup.h"\n#include <cstdio>', 1)
io.open(p1, 'w', encoding='utf-8', newline='').write(s)
print('appsystemgroup.cpp patched')

# 2) launcher.cpp: Main() 入口打点
p2 = '/root/csgo-src/CSGO-Source-Linux-20260928/src/launcher/launcher.cpp'
s2 = io.open(p2, encoding='utf-8', errors='replace').read()
old3 = '''int CSourceAppSystemGroup::Main()'''
new3 = '''int CSourceAppSystemGroup::Main()
{
	fprintf( stderr, "CSGO_TRACE: SourceAppGroup Main enter (all InitSystems done)\\n" );'''
assert old3 in s2, 'CSourceAppSystemGroup::Main not found'
s2 = s2.replace(old3, new3, 1)
io.open(p2, 'w', encoding='utf-8', newline='').write(s2)
print('launcher.cpp patched')
