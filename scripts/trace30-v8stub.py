#!/usr/bin/env python3
"""V8 桩守卫 + trace（uiengine.cpp，WSL 副本直接改——不在 wsl-sync 列表）"""
import io

p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/panorama/uiengine.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()

old1 = '''			const char* v8version = v8::V8::GetVersion();
			Msg("V8 Version: %s\\n", v8version);

			PushContextPanel( nullptr );'''
new1 = '''			const char* v8version = v8::V8::GetVersion();
			Msg("V8 Version: %s\\n", v8version);
			fprintf( stderr, "CSGO_TRACE: panorama V8 global init done (version=%s)\\n", v8version ? v8version : "STUB" );

			PushContextPanel( nullptr );
			fprintf( stderr, "CSGO_TRACE: panorama PushContextPanel done\\n" );'''
assert old1 in s, 'anchor1 miss'
s = s.replace(old1, new1, 1)

old2 = '''			bool bSuccess = ClientAPI_Init();
			AssertMsg( bSuccess, "ClientAPI_Init failed" );'''
new2 = '''			fprintf( stderr, "CSGO_TRACE: ClientAPI_Init calling\\n" );
			bool bSuccess = ClientAPI_Init();
			fprintf( stderr, "CSGO_TRACE: ClientAPI_Init done bSuccess=%d\\n", (int)bSuccess );
			AssertMsg( bSuccess, "ClientAPI_Init failed" );'''
assert old2 in s, 'anchor2 miss'
s = s.replace(old2, new2, 1)

old3 = '''	m_pV8Isolate = v8::Isolate::New( createParams );
	v8::Isolate::Scope isolate_scope( m_pV8Isolate );
	v8::V8::SetFatalErrorHandler( &V8FatalErrorHandler );
	m_bDoV8GarbageCollect = false;

	v8::HandleScope handle_scope( m_pV8Isolate );'''
new3 = '''	m_pV8Isolate = v8::Isolate::New( createParams );
	fprintf( stderr, "CSGO_TRACE: V8 Isolate::New -> %p\\n", (void*)m_pV8Isolate );
	v8::V8::SetFatalErrorHandler( &V8FatalErrorHandler );
	m_bDoV8GarbageCollect = false;

	// V8 桩库（零返回 thunk）下 Isolate 为 null：Isolate::Scope 构造 Enter()
	// 会解引用空指针，用桩判空跳过两个 Scope（真 v8 时行为不变）
	if ( m_pV8Isolate )
	{
		v8::Isolate::Scope isolate_scope( m_pV8Isolate );
		v8::HandleScope handle_scope( m_pV8Isolate );
	}
	else
	{
		fprintf( stderr, "CSGO_TRACE: V8 STUB detected - panorama JS disabled\\n" );
	}'''
assert old3 in s, 'anchor3 miss'
s = s.replace(old3, new3, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('uiengine.cpp v8-stub guards + traces applied')
