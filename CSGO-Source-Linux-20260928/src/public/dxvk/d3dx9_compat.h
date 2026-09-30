#ifndef SOURCE_D3DX9_COMPAT_H
#define SOURCE_D3DX9_COMPAT_H

#include "dxvk/d3d9_compat.h"

// Native DXVK's Windows type header omits GDI/OLE declarations referenced by
// the SDK's D3DX headers. The renderer only uses their pointer declarations.
#define LF_FACESIZE 32
struct tagTEXTMETRICA;
struct tagTEXTMETRICW;
typedef tagTEXTMETRICA TEXTMETRICA;
typedef tagTEXTMETRICW TEXTMETRICW;
struct GLYPHMETRICSFLOAT;
struct IStream;
typedef GUID *LPGUID;
typedef double DOUBLE;
#define STDAPI extern "C" HRESULT WINAPI
#include <d3dx9.h>

#endif
