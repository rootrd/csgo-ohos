#ifndef SOURCE_D3D9_COMPAT_H
#define SOURCE_D3D9_COMPAT_H

#include "tier0/platform.h"
#include "tier0/basetypes.h"

// Use DXVK's native COM ABI and format values. Restore the Source macros
// afterwards, especially ZeroMemory, which is a tier0 function on POSIX.
#pragma push_macro("TRUE")
#pragma push_macro("WAIT_FAILED")
#pragma push_macro("ZeroMemory")
#undef TRUE
#undef WAIT_FAILED
#undef ZeroMemory
// Source's legacy platform header may define INT64_MAX with a C++ cast. DXVK
// uses this standard limit in #if expressions to select pointer-sized types.
#pragma push_macro("INT64_MAX")
#undef INT64_MAX
#define INT64_MAX 0x7fffffffffffffffLL
#include <d3d9.h>
#pragma pop_macro("INT64_MAX")
#pragma pop_macro("ZeroMemory")
#pragma pop_macro("WAIT_FAILED")
#pragma pop_macro("TRUE")

static_assert(sizeof(GUID) == 16 && sizeof(DWORD) == 4 && sizeof(HRESULT) == 4,
              "D3D9 native ABI must retain Windows fixed-width types");

#endif
