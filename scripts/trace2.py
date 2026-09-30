#!/usr/bin/env python3
import io

# 1) shaderdevicedx8.cpp: InitAdapterInfo 尾部打点
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/shaderapidx9/shaderdevicedx8.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'InitAdapterInfo done' not in s:
    s = s.replace('''		const char *pShaderParam = CommandLine()->ParmValue( "-shader" );''',
'''		Warning( "CSGO_TRACE: adapter dxsupport read done\\n" );
		const char *pShaderParam = CommandLine()->ParmValue( "-shader" );''', 1)
    s = s.replace('''	}
}

//--------------------------------------------------------------------------------
// Code to detect support for texture border color''',
'''	}
	Warning( "CSGO_TRACE: InitAdapterInfo done (adapters=%d)\\n", m_Adapters.Count() );
}

//--------------------------------------------------------------------------------
// Code to detect support for texture border color''', 1)
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('devicedx8 patched, traces:', s.count('CSGO_TRACE'))

# 2) appsystemgroup.cpp: OnStartup 阶段打点
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/appframework/appsystemgroup.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'OnStartup create' not in s:
    s = s.replace('''	m_nCurrentStage = CREATION;
	if ( !Create() )''',
'''	Warning( "CSGO_TRACE: OnStartup create\\n" );
	m_nCurrentStage = CREATION;
	if ( !Create() )''', 1)
    s = s.replace('''	m_nCurrentStage = DEPENDENCIES;
	if ( !LoadDependentSystems() )''',
'''	Warning( "CSGO_TRACE: OnStartup load-deps calling\\n" );
	m_nCurrentStage = DEPENDENCIES;
	if ( !LoadDependentSystems() )''', 1)
    s = s.replace('''	m_nCurrentStage = CONNECTION;
	if ( !ConnectSystems() )''',
'''	Warning( "CSGO_TRACE: OnStartup connect calling\\n" );
	m_nCurrentStage = CONNECTION;
	if ( !ConnectSystems() )''', 1)
    s = s.replace('''	m_nCurrentStage = PREINITIALIZATION;
	if ( !PreInit() )''',
'''	Warning( "CSGO_TRACE: OnStartup preinit calling\\n" );
	m_nCurrentStage = PREINITIALIZATION;
	if ( !PreInit() )''', 1)
    s = s.replace('''	m_nCurrentStage = INITIALIZATION;
	int nRetVal = InitSystems();''',
'''	Warning( "CSGO_TRACE: OnStartup init-systems calling\\n" );
	m_nCurrentStage = INITIALIZATION;
	int nRetVal = InitSystems();''', 1)
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('appsystemgroup patched, traces:', s.count('CSGO_TRACE'))

# 3) cmaterialsystem.cpp: Init() 入口
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/cmaterialsystem.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'materialsystem Init enter' not in s:
    marker = 'InitReturnVal_t CMaterialSystem::Init()\n{'
    if marker in s:
        s = s.replace(marker, marker + '\n\tWarning( "CSGO_TRACE: materialsystem Init enter\\n" );', 1)
        io.open(p, 'w', encoding='utf-8', newline='').write(s)
        print('materialsystem Init patched, traces:', s.count('CSGO_TRACE'))
    else:
        print('Init() marker not found')
