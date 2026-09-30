// CSM-only hardware instancing. All decisions about which casters, materials,
// LODs and strips to render are made by the existing model render system.
#if defined( ANDROID ) && defined( USE_DXVK_NATIVE )

#include "locald3dtypes.h"
#include "csm_instancing.h"
#include "csm_geometry.h"
#include "shaderapidx8_global.h"
#include "materialsystem/imesh.h"
#include "imaterialinternal.h"
#include "tier1/convar.h"
#include "tier1/strtools.h"
#include "tier1/utlvector.h"
#include <algorithm>

#include "csm_instancing_vs.inc"

// Keep this opt-in until same-frame depth and image comparisons have passed.
static ConVar r_csm_instancing( "r_csm_instancing", "0", FCVAR_DEVELOPMENTONLY,
    "Instance identical rigid opaque meshes in the existing CSM caster list (Android/DXVK)" );
static ConVar r_csm_profile( "r_csm_profile", "0", FCVAR_DEVELOPMENTONLY,
    "Measure this many complete GPU render frames, CSM passes and model counts (0 = idle)" );
static ConVar r_csm_profile_no_sampling( "r_csm_profile_no_sampling", "0", FCVAR_CHEAT | FCVAR_DEVELOPMENTONLY,
    "Diagnostic only: disable the CSM receiving branch; bit 1=world, bit 2=models. Still renders the complete shadow atlas.",
    true, 0, true, 3 );

namespace
{
IDirect3DDevice9 *CSMDevice() { return Dx9Device()->GetSynchronousDevice(); }
COMPILE_TIME_ASSERT( sizeof(matrix3x4_t) == 48 );

template <class T> void ReleaseCSMObject( T *&p )
{
    if ( p ) p->Release();
    p = NULL;
}

const int MAX_BATCH_INSTANCES = 1024;
const int MATRIX_BUFFER_INSTANCES = 8192;
const int PROFILE_SLOTS = 16;
IDirect3DVertexShader9 *s_pShader = NULL;
IDirect3DVertexDeclaration9 *s_pDeclaration = NULL;
IDirect3DVertexBuffer9 *s_pMatrices = NULL;
int s_nNextMatrix = 0;
bool s_bGenerating = false;
bool s_bResourceFailure = false;

struct Counts
{
    int64 draws, triangles, eligible, candidateBatches, mergedBatches, savedDraws;
    int64 compactDraws, compactTriangles, inspectedInstances, inspectedTriangles;
    int64 sourceVertexRefs, positionRefs, sourceVertexBytes;
    int64 worldSamplingSets, modelSamplingSets;
    int strideMin, strideMax;
    Counts() : draws(0), triangles(0), eligible(0), candidateBatches(0), mergedBatches(0), savedDraws(0),
        compactDraws(0), compactTriangles(0), inspectedInstances(0), inspectedTriangles(0),
        sourceVertexRefs(0), positionRefs(0), sourceVertexBytes(0), worldSamplingSets(0), modelSamplingSets(0), strideMin(0), strideMax(0) {}
    void Add( const Counts &c )
    {
        draws += c.draws; triangles += c.triangles; eligible += c.eligible;
        candidateBatches += c.candidateBatches; mergedBatches += c.mergedBatches; savedDraws += c.savedDraws;
        compactDraws += c.compactDraws; compactTriangles += c.compactTriangles;
        inspectedInstances += c.inspectedInstances; inspectedTriangles += c.inspectedTriangles;
        sourceVertexRefs += c.sourceVertexRefs; positionRefs += c.positionRefs; sourceVertexBytes += c.sourceVertexBytes;
        worldSamplingSets += c.worldSamplingSets; modelSamplingSets += c.modelSamplingSets;
        if ( c.strideMin && ( !strideMin || c.strideMin < strideMin ) ) strideMin = c.strideMin;
        strideMax = MAX( strideMax, c.strideMax );
    }
};
struct QuerySlot
{
    IDirect3DQuery9 *begin, *end, *frameBegin, *frameEnd, *frequency, *disjoint;
    bool pending, csmSeen;
    Counts counts, otherCounts;
    QuerySlot() : begin(NULL), end(NULL), frameBegin(NULL), frameEnd(NULL), frequency(NULL), disjoint(NULL), pending(false), csmSeen(false) {}
};
QuerySlot s_Queries[PROFILE_SLOTS];
int s_nCurrentSlot = -1;
int s_nRequested = 0, s_nIssued = 0, s_nResolved = 0, s_nInvalid = 0;
int s_nProfileMode = 0;
int s_nProfileGeometryMode = 0;
int s_nProfileSamplingMask = 0;
Counts s_ProfileCounts, s_OtherCounts;
CUtlVector<double> s_Times, s_FrameTimes;

bool InitQueries( QuerySlot &q )
{
    if ( q.begin ) return true;
    IDirect3DDevice9 *device = CSMDevice();
    if ( FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &q.begin ) ) ||
         FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &q.end ) ) ||
         FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &q.frameBegin ) ) ||
         FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMP, &q.frameEnd ) ) ||
         FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMPFREQ, &q.frequency ) ) ||
         FAILED( device->CreateQuery( D3DQUERYTYPE_TIMESTAMPDISJOINT, &q.disjoint ) ) )
    {
        ReleaseCSMObject( q.begin ); ReleaseCSMObject( q.end );
        ReleaseCSMObject( q.frameBegin ); ReleaseCSMObject( q.frameEnd );
        ReleaseCSMObject( q.frequency ); ReleaseCSMObject( q.disjoint );
        return false;
    }
    return true;
}

void PollProfile()
{
    if ( !s_nRequested ) return;
    for ( int i = 0; i < PROFILE_SLOTS; ++i )
    {
        QuerySlot &q = s_Queries[i];
        if ( !q.pending ) continue;
        UINT64 begin = 0, end = 0, frameBegin = 0, frameEnd = 0, frequency = 0;
        BOOL disjoint = FALSE;
        HRESULT results[6] = {
            q.csmSeen ? q.begin->GetData( &begin, sizeof(begin), 0 ) : S_OK,
            q.csmSeen ? q.end->GetData( &end, sizeof(end), 0 ) : S_OK,
            q.frameBegin->GetData( &frameBegin, sizeof(frameBegin), 0 ),
            q.frameEnd->GetData( &frameEnd, sizeof(frameEnd), 0 ),
            q.frequency->GetData( &frequency, sizeof(frequency), 0 ), q.disjoint->GetData( &disjoint, sizeof(disjoint), 0 )
        };
        bool ready = true, failed = false;
        for ( int j = 0; j < 6; ++j ) { ready &= results[j] != S_FALSE; failed |= FAILED( results[j] ); }
        if ( !ready && !failed ) continue;
        q.pending = false;
        ++s_nResolved;
        if ( failed || disjoint || !frequency || frameEnd <= frameBegin ||
             ( q.csmSeen && ( end <= begin || begin < frameBegin || end > frameEnd ) ) )
        {
            ++s_nInvalid;
            continue;
        }
        s_Times.AddToTail( double( end - begin ) * 1000.0 / double( frequency ) );
        s_FrameTimes.AddToTail( double( frameEnd - frameBegin ) * 1000.0 / double( frequency ) );
        s_ProfileCounts.Add( q.counts );
        s_OtherCounts.Add( q.otherCounts );
    }
    if ( s_nResolved != s_nRequested ) return;
    if ( s_Times.Count() )
    {
        std::sort( s_Times.Base(), s_Times.Base() + s_Times.Count() );
        double total = 0, frameTotal = 0;
        for ( int i = 0; i < s_Times.Count(); ++i ) { total += s_Times[i]; frameTotal += s_FrameTimes[i]; }
        const double n = s_Times.Count();
        int geometryRanges = 0;
        size_t cpuBytes = 0, gpuBytes = 0;
        CSMGeometryMemory( geometryRanges, cpuBytes, gpuBytes );
        Msg( "[CSM profile] mode=%d samples=%d invalid=%d gpu_ms=%.4f p50=%.4f p90=%.4f render_ms=%.4f noncsm_ms=%.4f model_draws=%.1f triangles=%.1f eligible=%.1f candidate_batches=%.1f merged_batches=%.1f saved_draws=%.1f noncsm_model_draws=%.1f noncsm_triangles=%.1f "
             "compact_mode=%d compact_draws=%.1f compact_triangles=%.1f inspected_instances=%.1f inspected_triangles=%.1f source_vertex_refs=%.1f unique_position_refs=%.1f source_stride=%.1f stride_min=%d stride_max=%d geometry_ranges=%d geometry_cpu_kb=%.1f geometry_gpu_kb=%.1f sampling_mask=%d world_sampling_sets=%.1f model_sampling_sets=%.1f\n",
             s_nProfileMode, s_Times.Count(), s_nInvalid, total / n,
             s_Times[s_Times.Count()/2], s_Times[(s_Times.Count()*9)/10],
             frameTotal/n, (frameTotal-total)/n,
             s_ProfileCounts.draws/n, s_ProfileCounts.triangles/n, s_ProfileCounts.eligible/n,
             s_ProfileCounts.candidateBatches/n, s_ProfileCounts.mergedBatches/n, s_ProfileCounts.savedDraws/n,
             s_OtherCounts.draws/n, s_OtherCounts.triangles/n,
             s_nProfileGeometryMode, s_ProfileCounts.compactDraws/n, s_ProfileCounts.compactTriangles/n,
             s_ProfileCounts.inspectedInstances/n, s_ProfileCounts.inspectedTriangles/n,
             s_ProfileCounts.sourceVertexRefs/n, s_ProfileCounts.positionRefs/n,
             s_ProfileCounts.sourceVertexRefs ? double(s_ProfileCounts.sourceVertexBytes)/s_ProfileCounts.sourceVertexRefs : 0.0,
             s_ProfileCounts.strideMin, s_ProfileCounts.strideMax, geometryRanges, cpuBytes/1024.0, gpuBytes/1024.0,
             s_nProfileSamplingMask, s_OtherCounts.worldSamplingSets/n, s_OtherCounts.modelSamplingSets/n );
    }
    else
        Warning( "[CSM profile] No valid GPU timestamps (%d invalid samples)\n", s_nInvalid );
    s_nRequested = 0;
}

bool InitInstanceResources()
{
    if ( s_bResourceFailure ) return false;
    if ( s_pMatrices ) return true;
    IDirect3DDevice9 *device = CSMDevice();
    const D3DVERTEXELEMENT9 elements[] = {
        { 0, 0,  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 1, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4 },
        { 1, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 5 },
        { 1, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6 },
        D3DDECL_END()
    };
    if ( FAILED( device->CreateVertexShader( s_CSMInstancingVertexShader, &s_pShader ) ) ||
         FAILED( device->CreateVertexDeclaration( elements, &s_pDeclaration ) ) ||
         FAILED( device->CreateVertexBuffer( MATRIX_BUFFER_INSTANCES * sizeof(matrix3x4_t),
             D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &s_pMatrices, NULL ) ) )
    {
        ReleaseCSMObject( s_pShader ); ReleaseCSMObject( s_pDeclaration ); ReleaseCSMObject( s_pMatrices );
        s_bResourceFailure = true;
        Warning( "CSM instancing unavailable; retaining individual model draws\n" );
        return false;
    }
    return true;
}

// Restore actual D3D state so Source's vertex/shader/stream caches stay valid.
struct SavedBindings
{
    IDirect3DDevice9 *device;
    IDirect3DVertexShader9 *shader;
    IDirect3DVertexDeclaration9 *declaration;
    IDirect3DIndexBuffer9 *indices;
    IDirect3DVertexBuffer9 *vertices[2];
    UINT offsets[2], strides[2], frequencies[2];
    bool valid, changed;
    SavedBindings() : device(CSMDevice()), shader(NULL), declaration(NULL), indices(NULL), valid(true), changed(false)
    {
        vertices[0] = vertices[1] = NULL;
        valid &= SUCCEEDED( device->GetVertexShader( &shader ) );
        valid &= SUCCEEDED( device->GetVertexDeclaration( &declaration ) );
        valid &= SUCCEEDED( device->GetIndices( &indices ) );
        for ( int i = 0; i < 2; ++i )
        {
            valid &= SUCCEEDED( device->GetStreamSource( i, &vertices[i], &offsets[i], &strides[i] ) );
            valid &= SUCCEEDED( device->GetStreamSourceFreq( i, &frequencies[i] ) );
        }
    }
    ~SavedBindings()
    {
        if ( changed )
        {
            device->SetVertexShader( shader );
            device->SetVertexDeclaration( declaration );
            device->SetIndices( indices );
            for ( int i = 0; i < 2; ++i )
            {
                device->SetStreamSourceFreq( i, frequencies[i] );
                device->SetStreamSource( i, vertices[i], offsets[i], strides[i] );
            }
        }
        ReleaseCSMObject( shader ); ReleaseCSMObject( declaration ); ReleaseCSMObject( indices );
        ReleaseCSMObject( vertices[0] ); ReleaseCSMObject( vertices[1] );
    }
};

bool RigidInstance( const MeshInstanceData_t &instance )
{
    return instance.m_nBoneCount == 0 && !instance.m_pBoneRemap && instance.m_pPoseToWorld &&
        instance.m_nPrimType == MATERIAL_TRIANGLES && instance.m_nIndexCount > 0 &&
        instance.m_nIndexCount % 3 == 0 && instance.m_nIndexOffset >= 0 &&
        instance.m_pVertexBuffer && instance.m_pIndexBuffer && !instance.m_pColorBuffer &&
        !instance.m_pStencilState && !instance.m_pLightingState && !instance.m_pEnvCubemap &&
        ( instance.m_pVertexBuffer->GetVertexFormat() & VERTEX_POSITION ) &&
        !( instance.m_pVertexBuffer->GetVertexFormat() & VERTEX_WRINKLE ) &&
        NumBoneWeights( instance.m_pVertexBuffer->GetVertexFormat() ) == 0;
}
}

void CSMInstancingFrameBegin()
{
    s_bGenerating = false;
    if ( !CSMDevice() ) return;
    s_nNextMatrix = 0;
    PollProfile();
    const int requested = r_csm_profile.GetInt();
    if ( requested > 0 && !s_nRequested )
    {
        r_csm_profile.SetValue( 0 );
        s_nRequested = MIN( requested, 1200 );
        s_nIssued = s_nResolved = s_nInvalid = 0;
        s_nProfileMode = r_csm_instancing.GetInt();
        s_nProfileGeometryMode = CSMGeometryMode();
        s_nProfileSamplingMask = r_csm_profile_no_sampling.GetInt();
        s_ProfileCounts = Counts();
        s_OtherCounts = Counts();
        s_Times.RemoveAll();
        s_FrameTimes.RemoveAll();
        Msg( "[CSM profile] begin mode=%d frames=%d (render-pass boundaries; no forced submissions)\n", s_nProfileMode, s_nRequested );
    }
    s_nCurrentSlot = -1;
    if ( !s_nRequested || s_nIssued >= s_nRequested ) return;
    for ( int i = 0; i < PROFILE_SLOTS; ++i )
    {
        QuerySlot &q = s_Queries[i];
        if ( q.pending ) continue;
        if ( !InitQueries( q ) )
        {
            Warning( "[CSM profile] Timestamp queries unavailable\n" );
            s_nRequested = s_nIssued;
            return;
        }
        // The Android DXVK timestamp path ends an active tile pass without
        // submitting it. Keep timestamps with the work, avoiding CPU feed gaps.
        q.counts = Counts();
        q.otherCounts = Counts();
        q.csmSeen = false;
        q.disjoint->Issue( D3DISSUE_BEGIN );
        q.frameBegin->Issue( D3DISSUE_END );
        s_nCurrentSlot = i;
        ++s_nIssued;
        break;
    }
}

void CSMInstancingFrameEnd()
{
    if ( s_nCurrentSlot >= 0 )
    {
        QuerySlot &q = s_Queries[s_nCurrentSlot];
        q.frameEnd->Issue( D3DISSUE_END );
        q.disjoint->Issue( D3DISSUE_END );
        q.frequency->Issue( D3DISSUE_END );
        q.pending = true;
        s_nCurrentSlot = -1;
    }
    s_bGenerating = false;
}

void CSMInstancingBegin()
{
    s_bGenerating = CSMDevice() != NULL;
    s_nNextMatrix = 0;
    if ( s_nCurrentSlot >= 0 )
    {
        QuerySlot &q = s_Queries[s_nCurrentSlot];
        if ( !q.csmSeen )
        {
            q.begin->Issue( D3DISSUE_END );
            q.csmSeen = true;
        }
    }
}

void CSMInstancingEnd()
{
    if ( s_nCurrentSlot >= 0 && s_Queries[s_nCurrentSlot].csmSeen )
    {
        s_Queries[s_nCurrentSlot].end->Issue( D3DISSUE_END );
    }
    s_bGenerating = false;
}

bool CSMInstancingMaterial( IMaterialInternal *pMaterial )
{
    if ( !s_bGenerating || !pMaterial || ( !r_csm_instancing.GetBool() && s_nCurrentSlot < 0 ) ) return false;
    // These are the two existing non-alpha, non-sway, non-colour-depth studio
    // materials. Culling/depth bias/pixel shader remain untouched.
    const char *name = pMaterial->GetName();
    return !Q_stricmp( name, "__DepthWrite000" ) || !Q_stricmp( name, "__DepthWrite010" );
}

bool CSMInstancingEnabled() { return r_csm_instancing.GetBool(); }

bool CSMProfileSuppressSampling( IMaterialInternal *pMaterial )
{
    const int mask = r_csm_profile_no_sampling.GetInt();
    if ( s_bGenerating || !pMaterial || ( !mask && s_nCurrentSlot < 0 ) ) return false;
    // These SM3 families use b0 for g_bCSMEnabled, including their fast paths.
    // Do not change arbitrary shaders' boolean registers or flashlight shaders.
    const char *shader = pMaterial->GetShaderName();
    int group = 0;
    if ( !Q_stricmp( shader, "LightmappedGeneric" ) || !Q_stricmp( shader, "WorldVertexTransition_DX9" ) ) group = 1;
    else if ( !Q_stricmp( shader, "VertexLitGeneric" ) || !Q_stricmp( shader, "UnlitGeneric" ) ||
              !Q_stricmp( shader, "CustomWeapon_dx9" ) ) group = 2;
    if ( s_nCurrentSlot >= 0 )
    {
        Counts &c = s_Queries[s_nCurrentSlot].otherCounts;
        if ( group == 1 ) ++c.worldSamplingSets;
        if ( group == 2 ) ++c.modelSamplingSets;
    }
    return ( mask & group ) != 0;
}

int CSMInstancingRunLength( const MeshInstanceData_t *pInstances, int nCount )
{
    const MeshInstanceData_t &first = pInstances[0];
    if ( !RigidInstance( first ) ) return 0;
    nCount = MIN( nCount, MAX_BATCH_INSTANCES );
    int n = 1;
    for ( ; n < nCount; ++n )
    {
        const MeshInstanceData_t &instance = pInstances[n];
        if ( !RigidInstance( instance ) || instance.m_pVertexBuffer != first.m_pVertexBuffer ||
             instance.m_pIndexBuffer != first.m_pIndexBuffer || instance.m_nIndexOffset != first.m_nIndexOffset ||
             instance.m_nIndexCount != first.m_nIndexCount || instance.m_nVertexOffsetInBytes != first.m_nVertexOffsetInBytes ) break;
    }
    return n;
}

void CSMInstancingRecord( const MeshInstanceData_t *pInstances, int nCount, bool bEligible, bool bInstanced )
{
    if ( s_nCurrentSlot < 0 ) return;
    Counts &c = s_bGenerating ? s_Queries[s_nCurrentSlot].counts : s_Queries[s_nCurrentSlot].otherCounts;
    c.draws += nCount;
    for ( int i = 0; i < nCount; ++i ) c.triangles += pInstances[i].m_nIndexCount / 3;
    if ( bEligible ) { c.eligible += nCount; ++c.candidateBatches; }
    if ( bInstanced ) { ++c.mergedBatches; c.savedDraws += nCount - 1; }
}

bool CSMInstancingDraw( IDirect3DVertexBuffer9 *pVertices, IDirect3DIndexBuffer9 *pIndices,
                       int nStride, int nVertexCount, const MeshInstanceData_t *pInstances, int nCount,
                       CSMVertexData *pVertexData, CSMIndexData *pIndexData )
{
    if ( nCount < 2 || nCount > MAX_BATCH_INSTANCES || !InitInstanceResources() ) return false;
    SavedBindings saved;
    if ( !saved.valid ) return false;
    CSMGeometryBinding geometry;
    CSMGeometryPrepare( pVertexData, pIndexData, nStride, pInstances[0].m_nVertexOffsetInBytes,
                         pInstances[0].m_nIndexOffset, pInstances[0].m_nIndexCount, s_nCurrentSlot >= 0, geometry );
    const int sourceStride = nStride;
    int vertexOffset = pInstances[0].m_nVertexOffsetInBytes;
    int firstIndex = pInstances[0].m_nIndexOffset;
    if ( geometry.mode )
    {
        pVertices = geometry.vertices;
        if ( geometry.indices ) pIndices = geometry.indices;
        nStride = geometry.stride;
        vertexOffset = geometry.vertexOffset;
        nVertexCount = geometry.vertexCount;
        firstIndex = geometry.indexOffset;
    }
    if ( s_nNextMatrix + nCount > MATRIX_BUFFER_INSTANCES ) s_nNextMatrix = 0;
    const UINT offset = s_nNextMatrix * sizeof(matrix3x4_t);
    void *data = NULL;
    if ( FAILED( s_pMatrices->Lock( offset, nCount * sizeof(matrix3x4_t), &data,
                    s_nNextMatrix ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD ) ) ) return false;
    for ( int i = 0; i < nCount; ++i )
        memcpy( (byte *)data + i * sizeof(matrix3x4_t), pInstances[i].m_pPoseToWorld[0].Base(), sizeof(matrix3x4_t) );
    if ( FAILED( s_pMatrices->Unlock() ) ) return false;
    s_nNextMatrix += nCount;

    IDirect3DDevice9 *device = CSMDevice();
    saved.changed = true;
    if ( FAILED( device->SetVertexShader( s_pShader ) ) ||
         FAILED( device->SetVertexDeclaration( s_pDeclaration ) ) ||
         FAILED( device->SetStreamSource( 0, pVertices, vertexOffset, nStride ) ) ||
         FAILED( device->SetStreamSource( 1, s_pMatrices, offset, sizeof(matrix3x4_t) ) ) ||
         FAILED( device->SetStreamSourceFreq( 0, D3DSTREAMSOURCE_INDEXEDDATA | nCount ) ) ||
         FAILED( device->SetStreamSourceFreq( 1, D3DSTREAMSOURCE_INSTANCEDATA | 1 ) ) ||
         FAILED( device->SetIndices( pIndices ) ) ) return false;
    const bool drawn = SUCCEEDED( device->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, nVertexCount,
                         firstIndex, pInstances[0].m_nIndexCount / 3 ) );
    if ( drawn && s_nCurrentSlot >= 0 && geometry.inspected )
    {
        Counts &c = s_Queries[s_nCurrentSlot].counts;
        c.inspectedInstances += nCount;
        c.inspectedTriangles += int64(pInstances[0].m_nIndexCount / 3) * nCount;
        c.sourceVertexRefs += int64(geometry.referencedVertices) * nCount;
        c.positionRefs += int64(geometry.uniquePositions) * nCount;
        c.sourceVertexBytes += int64(geometry.referencedVertices) * nCount * sourceStride;
        if ( !c.strideMin || sourceStride < c.strideMin ) c.strideMin = sourceStride;
        c.strideMax = MAX( c.strideMax, sourceStride );
        if ( geometry.mode )
        {
            ++c.compactDraws;
            c.compactTriangles += int64(pInstances[0].m_nIndexCount / 3) * nCount;
        }
    }
    return drawn;
}

void CSMInstancingShutdown()
{
    CSMGeometryShutdown();
    ReleaseCSMObject( s_pShader ); ReleaseCSMObject( s_pDeclaration ); ReleaseCSMObject( s_pMatrices );
    for ( int i = 0; i < PROFILE_SLOTS; ++i )
    {
        QuerySlot &q = s_Queries[i];
        ReleaseCSMObject( q.begin ); ReleaseCSMObject( q.end );
        ReleaseCSMObject( q.frameBegin ); ReleaseCSMObject( q.frameEnd );
        ReleaseCSMObject( q.frequency ); ReleaseCSMObject( q.disjoint );
        q.pending = false;
    }
    s_nCurrentSlot = -1;
    s_nRequested = 0;
    s_bGenerating = s_bResourceFailure = false;
    s_Times.Purge();
    s_FrameTimes.Purge();
}

#endif
