#!/usr/bin/env python3
import io

# 1) cmaterialsystem.cpp Init 阶段打点
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/cmaterialsystem.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'Init stage adapter-set done' not in s:
    s = s.replace('''	g_pShaderDeviceMgr->SetAdapter( m_nAdapter, m_nAdapterFlags );
	if ( g_pShaderDeviceMgr->Init( ) != INIT_OK )''',
'''	Warning( "CSGO_TRACE: Init stage adapter-set calling\\n" );
	g_pShaderDeviceMgr->SetAdapter( m_nAdapter, m_nAdapterFlags );
	Warning( "CSGO_TRACE: Init stage adapter-set done\\n" );
	Warning( "CSGO_TRACE: Init stage devicemgr-init calling\\n" );
	if ( g_pShaderDeviceMgr->Init( ) != INIT_OK )''', 1)
    s = s.replace('''	// Texture manager...
	TextureManager()->Init( m_nAdapterFlags );

	// Shader system!
	ShaderSystem()->Init();''',
'''	Warning( "CSGO_TRACE: Init stage texture-manager calling\\n" );
	// Texture manager...
	TextureManager()->Init( m_nAdapterFlags );
	Warning( "CSGO_TRACE: Init stage texture-manager done\\n" );
	// Shader system!
	Warning( "CSGO_TRACE: Init stage shadersystem calling\\n" );
	ShaderSystem()->Init();
	Warning( "CSGO_TRACE: Init stage shadersystem done\\n" );''', 1)
    s = s.replace('''	InitColorCorrection();

	// Set up debug materials...
	CreateDebugMaterials();''',
'''	Warning( "CSGO_TRACE: Init stage color-correction calling\\n" );
	InitColorCorrection();
	Warning( "CSGO_TRACE: Init stage debug-materials calling\\n" );
	// Set up debug materials...
	CreateDebugMaterials();
	Warning( "CSGO_TRACE: Init stage debug-materials done\\n" );''', 1)
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('cmaterialsystem staged, traces:', s.count('CSGO_TRACE'))

# 2) SetAdapter 头尾
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/shaderapidx9/shaderdevicedx8.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'SetAdapter enter' not in s:
    s = s.replace('''bool CShaderDeviceMgrDx8::SetAdapter( int nAdapter, int nAdapterFlags )
{
	LOCK_SHADERAPI();''',
'''bool CShaderDeviceMgrDx8::SetAdapter( int nAdapter, int nAdapterFlags )
{
	Warning( "CSGO_TRACE: SetAdapter enter\\n" );
	LOCK_SHADERAPI();''', 1)
    s = s.replace('''	// backward compat
	if ( !g_pShaderDeviceDx8->OnAdapterSet() )
		return false;''',
'''	// backward compat
	Warning( "CSGO_TRACE: SetAdapter OnAdapterSet calling\\n" );
	if ( !g_pShaderDeviceDx8->OnAdapterSet() )
		return false;
	Warning( "CSGO_TRACE: SetAdapter done\\n" );''', 1)
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('shaderdevicedx8 staged, traces:', s.count('CSGO_TRACE'))
