// Android/DXVK experiment: keep the CSM caster list and instance transforms intact.
#ifndef CSM_INSTANCING_H
#define CSM_INSTANCING_H

#if defined( ANDROID ) && defined( USE_DXVK_NATIVE )

struct MeshInstanceData_t;
class IMaterialInternal;
struct CSMVertexData;
struct CSMIndexData;

void CSMInstancingBegin();
void CSMInstancingEnd();
void CSMInstancingFrameBegin();
void CSMInstancingFrameEnd();
void CSMInstancingShutdown();
bool CSMInstancingMaterial( IMaterialInternal *pMaterial );
bool CSMInstancingEnabled();
bool CSMProfileSuppressSampling( IMaterialInternal *pMaterial );
int CSMInstancingRunLength( const MeshInstanceData_t *pInstances, int nCount );
void CSMInstancingRecord( const MeshInstanceData_t *pInstances, int nCount, bool bEligible, bool bInstanced );
bool CSMInstancingDraw( IDirect3DVertexBuffer9 *pVertices, IDirect3DIndexBuffer9 *pIndices,
                       int nStride, int nVertexCount, const MeshInstanceData_t *pInstances, int nCount,
                       CSMVertexData *pVertexData, CSMIndexData *pIndexData );

#endif
#endif
