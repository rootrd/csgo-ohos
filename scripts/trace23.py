#!/usr/bin/env python3
"""bisect v4: CEngineAPI::Init + OnStartup 逐子步骤打点（sys_dll2.cpp，WSL 直接改）"""
import io

p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/engine/sys_dll2.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()

pairs = [
    # Init 入口与 BaseClass 之后
    ('''InitReturnVal_t CEngineAPI::Init() 
{
	if ( CommandLine()->FindParm( "-sv_benchmark" ) != 0 )
	{
		Plat_SetBenchmarkMode( true );
	}

	InitReturnVal_t nRetVal = BaseClass::Init();
	if ( nRetVal != INIT_OK )
		return nRetVal;''',
     '''InitReturnVal_t CEngineAPI::Init() 
{
	fprintf( stderr, "CSGO_TRACE: engine Init[15] enter\\n" );
	if ( CommandLine()->FindParm( "-sv_benchmark" ) != 0 )
	{
		Plat_SetBenchmarkMode( true );
	}

	InitReturnVal_t nRetVal = BaseClass::Init();
	fprintf( stderr, "CSGO_TRACE: engine BaseClass::Init done\\n" );
	if ( nRetVal != INIT_OK )
		return nRetVal;'''),
    # VideoMode_Create 之后
    ('''	// This creates the videomode singleton object, it doesn't depend on the registry
	VideoMode_Create();''',
     '''	// This creates the videomode singleton object, it doesn't depend on the registry
	VideoMode_Create();
	fprintf( stderr, "CSGO_TRACE: engine VideoMode_Create done\\n" );'''),
    # OnStartup 调用前后
    ('''	if ( !OnStartup( m_StartupInfo.m_pInstance, m_StartupInfo.m_pInitialMod ) )
	{
		return HandleSetModeError();
	}''',
     '''	fprintf( stderr, "CSGO_TRACE: engine OnStartup calling\\n" );
	if ( !OnStartup( m_StartupInfo.m_pInstance, m_StartupInfo.m_pInitialMod ) )
	{
		return HandleSetModeError();
	}
	fprintf( stderr, "CSGO_TRACE: engine OnStartup done\\n" );'''),
    # OnStartup 内部：splitscreen / game->Init / videomode->Init / registry / ModInit
    ('''	COM_TimestampedLog( "game->Init" );

	splitscreen->Init();''',
     '''	COM_TimestampedLog( "game->Init" );
	fprintf( stderr, "CSGO_TRACE: OnStartup splitscreen calling\\n" );
	splitscreen->Init();
	fprintf( stderr, "CSGO_TRACE: OnStartup splitscreen done\\n" );'''),
    ('''	if ( !game->Init( pInstance ) )
	{
		goto onStartupError;
	}''',
     '''	fprintf( stderr, "CSGO_TRACE: OnStartup game->Init calling\\n" );
	if ( !game->Init( pInstance ) )
	{
		goto onStartupError;
	}
	fprintf( stderr, "CSGO_TRACE: OnStartup game->Init done\\n" );'''),
    ('''	if ( !videomode->Init( ) )
	{
		goto onStartupShutdownVideoMode;
	}''',
     '''	fprintf( stderr, "CSGO_TRACE: OnStartup videomode->Init calling\\n" );
	if ( !videomode->Init( ) )
	{
		goto onStartupShutdownVideoMode;
	}
	fprintf( stderr, "CSGO_TRACE: OnStartup videomode->Init done\\n" );'''),
    ('''	if ( !InitRegistry( pStartupModName ) )
	{
		goto onStartupShutdownVideoMode;
	}''',
     '''	fprintf( stderr, "CSGO_TRACE: OnStartup InitRegistry calling\\n" );
	if ( !InitRegistry( pStartupModName ) )
	{
		goto onStartupShutdownVideoMode;
	}
	fprintf( stderr, "CSGO_TRACE: OnStartup InitRegistry done\\n" );'''),
    ('''	materials->ModInit();

	COM_TimestampedLog( "InitMaterialSystemConfig" );''',
     '''	fprintf( stderr, "CSGO_TRACE: OnStartup materials->ModInit calling (shader precache)\\n" );
	materials->ModInit();
	fprintf( stderr, "CSGO_TRACE: OnStartup materials->ModInit done\\n" );

	COM_TimestampedLog( "InitMaterialSystemConfig" );'''),
]

applied = 0
for old, new in pairs:
    if old in s:
        s = s.replace(old, new, 1)
        applied += 1
    else:
        print(f'MISS: {old[:60]!r}')
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print(f'sys_dll2.cpp patched: {applied}/{len(pairs)}')
