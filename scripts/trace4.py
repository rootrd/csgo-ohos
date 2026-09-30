#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/cmaterialsystem.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'stage composite-init calling' not in s:
    s = s.replace('''	BeginRenderTargetAllocation();
	m_CompositeTextureGenerator.Init();
	EndRenderTargetAllocation();
	m_CustomMaterialManager.Init();

	return m_HardwareRenderContext.Init( this );''',
'''	BeginRenderTargetAllocation();
	Warning( "CSGO_TRACE: Init stage composite-init calling\\n" );
	m_CompositeTextureGenerator.Init();
	Warning( "CSGO_TRACE: Init stage composite-init done\\n" );
	EndRenderTargetAllocation();
	Warning( "CSGO_TRACE: Init stage custom-materials calling\\n" );
	m_CustomMaterialManager.Init();
	Warning( "CSGO_TRACE: Init stage custom-materials done\\n" );
	Warning( "CSGO_TRACE: Init stage hw-render-context calling\\n" );
	return m_HardwareRenderContext.Init( this );''', 1)
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched, traces:', s.count('CSGO_TRACE'))
else:
    print('already patched')
