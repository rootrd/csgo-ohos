#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/cmatrendercontext.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
old = '''	m_pBoundMorph = NULL;

	// Create some lovely textures
	m_pLocalCubemapTexture = TextureManager()->ErrorTexture();
	m_pMorphRenderContext = g_pMorphMgr->AllocateRenderContext();

	return INIT_OK;'''
new = '''	m_pBoundMorph = NULL;

	Warning( "CSGO_TRACE: renderctx base done, ErrorTexture calling\\n" );
	// Create some lovely textures
	m_pLocalCubemapTexture = TextureManager()->ErrorTexture();
	Warning( "CSGO_TRACE: ErrorTexture done, morph AllocateRenderContext calling\\n" );
	m_pMorphRenderContext = g_pMorphMgr->AllocateRenderContext();
	Warning( "CSGO_TRACE: renderctx Init done\\n" );

	return INIT_OK;'''
assert old in s, 'pattern not found'
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('patched, traces:', s.count('CSGO_TRACE'))
