//================ Copyright (c) 1996-2009 Valve Corporation. All Rights Reserved. =================
//
//
//
//==================================================================================================
#ifndef WINUTILS_H
#define WINUTILS_H

#include "togl/rendermechanism.h"

#if !defined(_WIN32)

	void Sleep( unsigned int ms );
	bool IsIconic( VD3DHWND hWnd );
	BOOL ClientToScreen( VD3DHWND hWnd, LPPOINT pPoint );
#if defined(USE_DXVK_NATIVE)
	BOOL GetClientRect( VD3DHWND hWnd, RECT *rect );
	int GetVulkanVideoMemorySize( unsigned vendor, unsigned device );
#endif
	void* GetCurrentThread();
	void SetThreadAffinityMask( void *hThread, int nMask );
	void GlobalMemoryStatus( MEMORYSTATUS *pOut );
#endif

#endif // WINUTILS_H
