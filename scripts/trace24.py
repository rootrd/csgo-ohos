#!/usr/bin/env python3
"""bisect v5: videomode->Init 正确打点 + CVideoMode_MaterialSystem::Init 内部逐段"""
import io

# 1) sys_dll2.cpp: 修正版 videomode 打点（goto onStartupShutdownGame）
p1 = '/root/csgo-src/CSGO-Source-Linux-20260928/src/engine/sys_dll2.cpp'
s = io.open(p1, encoding='utf-8', errors='replace').read()
old = '''	COM_TimestampedLog( "videomode->Init" );

	// This needs to be after Shader_Init and registry->Init
	// This way mods can have different default video settings
	if ( !videomode->Init( ) )
	{
		goto onStartupShutdownGame;'''
new = '''	COM_TimestampedLog( "videomode->Init" );
	fprintf( stderr, "CSGO_TRACE: OnStartup videomode->Init calling\\n" );

	// This needs to be after Shader_Init and registry->Init
	// This way mods can have different default video settings
	if ( !videomode->Init( ) )
	{
		goto onStartupShutdownGame;'''
assert old in s, 'videomode block v2 not found'
s = s.replace(old, new, 1)
old2 = '''		goto onStartupShutdownGame;
	}

	COM_TimestampedLog( "InitRegistry" );'''
new2 = '''		goto onStartupShutdownGame;
	}
	fprintf( stderr, "CSGO_TRACE: OnStartup videomode->Init done\\n" );

	COM_TimestampedLog( "InitRegistry" );'''
assert old2 in s, 'videomode tail not found'
s = s.replace(old2, new2, 1)
io.open(p1, 'w', encoding='utf-8', newline='').write(s)
print('sys_dll2.cpp patched')

# 2) sys_getmodes.cpp: CVideoMode_MaterialSystem::Init 内部
p2 = '/root/csgo-src/CSGO-Source-Linux-20260928/src/engine/sys_getmodes.cpp'
s2 = io.open(p2, encoding='utf-8', errors='replace').read()
pairs = [
    ('''    m_bSetModeOnce = false;
    m_bPlayedStartupVideo = false;''',
     '''    m_bSetModeOnce = false;
    m_bPlayedStartupVideo = false;
    fprintf( stderr, "CSGO_TRACE: videomode MS::Init enter\\n" );'''),
    ('''    int nAdapter = materials->GetCurrentAdapter();
    int nModeCount = materials->GetModeCount( nAdapter );''',
     '''    int nAdapter = materials->GetCurrentAdapter();
    fprintf( stderr, "CSGO_TRACE: videomode GetModeCount calling\\n" );
    int nModeCount = materials->GetModeCount( nAdapter );
    fprintf( stderr, "CSGO_TRACE: videomode GetModeCount done n=%d\\n", nModeCount );'''),
    ('''    game->GetDesktopInfo( nDesktopWidth, nDesktopHeight, nDesktopRefresh );''',
     '''    fprintf( stderr, "CSGO_TRACE: videomode GetDesktopInfo calling\\n" );
    game->GetDesktopInfo( nDesktopWidth, nDesktopHeight, nDesktopRefresh );
    fprintf( stderr, "CSGO_TRACE: videomode GetDesktopInfo done\\n" );'''),
    ('''    // Sort modes for easy searching later
    if ( m_nNumModes > 1 )''',
     '''    fprintf( stderr, "CSGO_TRACE: videomode enum loop done n=%d\\n", m_nNumModes );
    // Sort modes for easy searching later
    if ( m_nNumModes > 1 )'''),
]
applied = 0
for old, new in pairs:
    if old in s2:
        s2 = s2.replace(old, new, 1)
        applied += 1
    else:
        print(f'MISS: {old[:50]!r}')
io.open(p2, 'w', encoding='utf-8', newline='').write(s2)
print(f'sys_getmodes.cpp patched: {applied}/{len(pairs)}')
