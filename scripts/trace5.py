#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/shaderapidx9/shaderdevicedx8.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'CreateD3DDevice enter' not in s:
    s = s.replace('''	// Determine the adapter format
	ShaderDisplayMode_t mode;
	g_pShaderDeviceMgrDx8->GetCurrentModeInfo( &mode, nAdapter );''',
'''	Warning( "CSGO_TRACE: CreateD3DDevice enter\\n" );
	// Determine the adapter format
	ShaderDisplayMode_t mode;
	g_pShaderDeviceMgrDx8->GetCurrentModeInfo( &mode, nAdapter );
	Warning( "CSGO_TRACE: CreateD3DDevice caps ok\\n" );''', 1)
    s = s.replace('''	SetPresentParameters( hWnd, nAdapter, info );''',
'''	Warning( "CSGO_TRACE: SetPresentParameters calling\\n" );
	SetPresentParameters( hWnd, nAdapter, info );
	Warning( "CSGO_TRACE: SetPresentParameters done\\n" );''', 1)
    s = s.replace('''	// Creates the device
	IDirect3DDevice9 *pD3DDevice = InvokeCreateDevice( pHWnd, nAdapter, deviceCreationFlags );''',
'''	// Creates the device
	Warning( "CSGO_TRACE: InvokeCreateDevice calling\\n" );
	IDirect3DDevice9 *pD3DDevice = InvokeCreateDevice( pHWnd, nAdapter, deviceCreationFlags );
	Warning( "CSGO_TRACE: InvokeCreateDevice -> %p\\n", pD3DDevice );''', 1)
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('patched, traces:', s.count('CSGO_TRACE'))
else:
    print('already')
