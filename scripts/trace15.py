#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/cmaterialsystem.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
if 'TRACE FindMaterial' in s:
    print('already patched')
    raise SystemExit
old = '''IMaterial* CMaterialSystem::FindMaterial( char const *pMaterialName, const char *pTextureGroupName, bool bComplain, const char *pComplainPrefix )
{
	if ( g_pResourceAccessControl )'''
new = '''IMaterial* CMaterialSystem::FindMaterial( char const *pMaterialName, const char *pTextureGroupName, bool bComplain, const char *pComplainPrefix )
{
	fprintf( stderr, "CSGO_TRACE: FindMaterial enter %s\\n", pMaterialName ? pMaterialName : "(null)" );
	if ( g_pResourceAccessControl )'''
assert old in s
s = s.replace(old, new, 1)
old2 = '''		KeyValues *pKeyValues = new KeyValues("vmt");
		KeyValues *pPatchKeyValues = new KeyValues( "vmt_patches" );
		if ( !LoadVMTFile( *pKeyValues, *pPatchKeyValues, vmtName, true, &includes ) )'''
new2 = '''		KeyValues *pKeyValues = new KeyValues("vmt");
		KeyValues *pPatchKeyValues = new KeyValues( "vmt_patches" );
		fprintf( stderr, "CSGO_TRACE: LoadVMTFile %s\\n", vmtName );
		if ( !LoadVMTFile( *pKeyValues, *pPatchKeyValues, vmtName, true, &includes ) )'''
assert old2 in s
s = s.replace(old2, new2, 1)
old3 = '''					pMat->PrecacheVars( pKeyValues, pPatchKeyValues, &includes );
					m_pForcedTextureLoadPathID = NULL;'''
new3 = '''					fprintf( stderr, "CSGO_TRACE: PrecacheVars %s\\n", matNameWithExtension );
					pMat->PrecacheVars( pKeyValues, pPatchKeyValues, &includes );
					fprintf( stderr, "CSGO_TRACE: PrecacheVars done %s\\n", matNameWithExtension );
					m_pForcedTextureLoadPathID = NULL;'''
assert old3 in s
s = s.replace(old3, new3, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('FindMaterial traced: 3 points')
