#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/vguimatsurface/MatSystemSurface.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'surface Init stage' not in s:
    s = s.replace('''	g_pLocalize->SetTextQuery( this );

	// Allocate a white material''',
'''	g_pLocalize->SetTextQuery( this );
	fprintf( stderr, "CSGO_TRACE: surface Init stage localize done\\n" );

	// Allocate a white material''', 1)
    s = s.replace('''	m_pWhite.Init( "VGUI_White", TEXTURE_GROUP_OTHER, pVMTKeyValues );

	InitFullScreenBuffer( MODEL_PANEL_RT_NAME );''',
'''	m_pWhite.Init( "VGUI_White", TEXTURE_GROUP_OTHER, pVMTKeyValues );
	fprintf( stderr, "CSGO_TRACE: surface Init stage white-mat done\\n" );
	fprintf( stderr, "CSGO_TRACE: surface Init stage fullscreen-buffer calling\\n" );
	InitFullScreenBuffer( MODEL_PANEL_RT_NAME );
	fprintf( stderr, "CSGO_TRACE: surface Init stage fullscreen-buffer done\\n" );''', 1)
    s = s.replace('''	// Initialize cursors
	InitCursors();''',
'''	// Initialize cursors
	fprintf( stderr, "CSGO_TRACE: surface Init stage embedded-panel done, cursors calling\\n" );
	InitCursors();
	fprintf( stderr, "CSGO_TRACE: surface Init stage cursors done, fonts calling\\n" );''', 1)
    io.open(p, 'w', encoding='utf-8', newline='').write(s)
    print('surface init staged')
else:
    print('already')
