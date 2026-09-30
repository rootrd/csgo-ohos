// Optional full-precision, position-only streams for the existing CSM instanced draws.
#if defined( ANDROID ) && defined( USE_DXVK_NATIVE )
#include "locald3dtypes.h"
#include "csm_geometry.h"
#include "csm_geometry_data.h"
#include "shaderapidx8_global.h"
#include "tier0/threadtools.h"
#include "tier1/convar.h"
#include <map>
#include <tuple>

static ConVar r_csm_compact_vertices( "r_csm_compact_vertices", "0", FCVAR_DEVELOPMENTONLY,
    "CSM instance geometry: 0=original, 1=float3 stream, 2=bit-identical position welding. Enable before loading the map.",
    true, 0, true, 2 );

struct CSMVertexData
{
    uint64 id;
    int stride;
    csm_geometry::Snapshot<csm_geometry::Position> data;
    IDirect3DVertexBuffer9 *packed;
    CSMVertexData() : id(0), stride(0), packed(NULL) {}
};
struct CSMIndexData
{
    uint64 id;
    csm_geometry::Snapshot<uint16_t> data;
    CSMIndexData() : id(0) {}
};

namespace
{
CThreadFastMutex s_Mutex;
uint64 s_NextId = 0;
std::map<uint64, CSMVertexData *> s_Vertices;
std::map<uint64, CSMIndexData *> s_Indices;

template <typename T> void Release( T *&p ) { if ( p ) p->Release(); p = NULL; }

struct CachedRange
{
    IDirect3DVertexBuffer9 *vertices;
    IDirect3DIndexBuffer9 *indices;
    int referencedVertices, uniquePositions, indexCount;
    bool valid, failed;
    CachedRange() : vertices(NULL), indices(NULL), referencedVertices(0), uniquePositions(0),
                    indexCount(0), valid(false), failed(false) {}
    ~CachedRange() { Release( vertices ); Release( indices ); }
};
// Source IDs cannot be reused after a buffer is released. The exact original
// submesh/index range and stream offset remain part of the key.
typedef std::tuple<uint64, uint64, int, int, int> RangeKey;
std::map<RangeKey, CachedRange *> s_Ranges;

void Invalidate( uint64 vertexId, uint64 indexId )
{
    for ( auto it = s_Ranges.begin(); it != s_Ranges.end(); )
    {
        if ( ( vertexId && std::get<0>(it->first) == vertexId ) ||
             ( indexId && std::get<1>(it->first) == indexId ) )
        {
            delete it->second;
            it = s_Ranges.erase( it );
        }
        else ++it;
    }
}

bool UploadVertexBuffer( const std::vector<csm_geometry::Position> &data, IDirect3DVertexBuffer9 *&buffer )
{
    IDirect3DDevice9 *device = Dx9Device()->GetSynchronousDevice();
    const UINT bytes = UINT(data.size() * sizeof(csm_geometry::Position));
    if ( !device || !bytes || FAILED( device->CreateVertexBuffer( bytes, D3DUSAGE_WRITEONLY, 0,
                                         D3DPOOL_DEFAULT, &buffer, NULL ) ) ) return false;
    void *destination = NULL;
    if ( FAILED( buffer->Lock( 0, bytes, &destination, 0 ) ) ) { Release( buffer ); return false; }
    memcpy( destination, data.data(), bytes );
    if ( FAILED( buffer->Unlock() ) ) { Release( buffer ); return false; }
    return true;
}

bool UploadIndexBuffer( const std::vector<uint16_t> &data, IDirect3DIndexBuffer9 *&buffer )
{
    IDirect3DDevice9 *device = Dx9Device()->GetSynchronousDevice();
    const UINT bytes = UINT(data.size() * sizeof(uint16_t));
    if ( !device || !bytes || FAILED( device->CreateIndexBuffer( bytes, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                         D3DPOOL_DEFAULT, &buffer, NULL ) ) ) return false;
    void *destination = NULL;
    if ( FAILED( buffer->Lock( 0, bytes, &destination, 0 ) ) ) { Release( buffer ); return false; }
    memcpy( destination, data.data(), bytes );
    if ( FAILED( buffer->Unlock() ) ) { Release( buffer ); return false; }
    return true;
}
}

int CSMGeometryMode() { return r_csm_compact_vertices.GetInt(); }

void CSMGeometryUploadVertices( CSMVertexData *&cache, const void *data, int stride,
                               int total, int first, int locked, int written )
{
    if ( !cache && !CSMGeometryMode() ) return;
    AUTO_LOCK( s_Mutex );
    if ( !cache )
    {
        cache = new CSMVertexData;
        cache->id = ++s_NextId;
        s_Vertices[cache->id] = cache;
    }
    Invalidate( cache->id, 0 );
    Release( cache->packed );
    cache->stride = stride;
    cache->data.Update( total, first, locked, written, data, stride );
}

void CSMGeometryUploadIndices( CSMIndexData *&cache, const void *data,
                              int total, int first, int locked, int written )
{
    if ( !cache && !CSMGeometryMode() ) return;
    AUTO_LOCK( s_Mutex );
    if ( !cache )
    {
        cache = new CSMIndexData;
        cache->id = ++s_NextId;
        s_Indices[cache->id] = cache;
    }
    Invalidate( 0, cache->id );
    cache->data.Update( total, first, locked, written, data, sizeof(uint16_t) );
}

void CSMGeometryDestroyVertices( CSMVertexData *&cache )
{
    if ( !cache ) return;
    AUTO_LOCK( s_Mutex );
    Invalidate( cache->id, 0 );
    Release( cache->packed );
    s_Vertices.erase( cache->id );
    delete cache;
    cache = NULL;
}

void CSMGeometryDestroyIndices( CSMIndexData *&cache )
{
    if ( !cache ) return;
    AUTO_LOCK( s_Mutex );
    Invalidate( 0, cache->id );
    s_Indices.erase( cache->id );
    delete cache;
    cache = NULL;
}

CSMGeometryBinding::CSMGeometryBinding() : vertices(NULL), indices(NULL), stride(0), vertexOffset(0),
    vertexCount(0), indexOffset(0), mode(0), referencedVertices(0), uniquePositions(0), inspected(false) {}
CSMGeometryBinding::~CSMGeometryBinding() { Release( vertices ); Release( indices ); }

void CSMGeometryPrepare( CSMVertexData *vertices, CSMIndexData *indices, int stride,
                         int vertexOffset, int firstIndex, int indexCount, bool inspect,
                         CSMGeometryBinding &binding )
{
    const int mode = CSMGeometryMode();
    if ( ( !mode && !inspect ) || !vertices || !indices || stride <= 0 ||
         vertexOffset < 0 || vertexOffset % stride ) return;
    AUTO_LOCK( s_Mutex );
    if ( vertices->stride != stride ) return;
    const int firstVertex = vertexOffset / stride;
    const RangeKey key( vertices->id, indices->id, firstVertex, firstIndex, indexCount );
    auto found = s_Ranges.find( key );
    CachedRange *range;
    csm_geometry::Range data;
    if ( found == s_Ranges.end() )
    {
        range = new CachedRange;
        range->valid = csm_geometry::BuildRange( vertices->data, indices->data, firstVertex, firstIndex, indexCount, data );
        if ( range->valid )
        {
            range->referencedVertices = data.referencedVertices;
            range->uniquePositions = int(data.positions.size());
            range->indexCount = indexCount;
        }
        s_Ranges[key] = range;
    }
    else range = found->second;
    if ( !range->valid ) return;
    binding.inspected = true;
    binding.referencedVertices = range->referencedVertices;
    binding.uniquePositions = range->uniquePositions;

    if ( mode == 1 )
    {
        if ( !vertices->packed && !UploadVertexBuffer( vertices->data.values, vertices->packed ) ) return;
        binding.vertices = vertices->packed;
        binding.vertexOffset = firstVertex * sizeof(csm_geometry::Position);
        binding.vertexCount = int(vertices->data.values.size()) - firstVertex;
        binding.indexOffset = firstIndex;
        // Retain the caller's original IB, including its original index range.
    }
    else if ( mode == 2 && !range->failed )
    {
        if ( !range->vertices )
        {
            if ( data.indices.empty() &&
                 !csm_geometry::BuildRange( vertices->data, indices->data, firstVertex, firstIndex, indexCount, data ) ) return;
            if ( !UploadVertexBuffer( data.positions, range->vertices ) || !UploadIndexBuffer( data.indices, range->indices ) )
            {
                Release( range->vertices ); Release( range->indices );
                range->failed = true;
                return;
            }
        }
        binding.vertices = range->vertices;
        binding.indices = range->indices;
        binding.vertexCount = range->uniquePositions;
    }
    else return;
    binding.mode = mode;
    binding.stride = sizeof(csm_geometry::Position);
    binding.vertices->AddRef();
    if ( binding.indices ) binding.indices->AddRef();
}

void CSMGeometryMemory( int &ranges, size_t &cpuBytes, size_t &gpuBytes )
{
    AUTO_LOCK( s_Mutex );
    ranges = int(s_Ranges.size());
    cpuBytes = gpuBytes = 0;
    for ( const auto &entry : s_Vertices )
    {
        cpuBytes += entry.second->data.Bytes();
        if ( entry.second->packed ) gpuBytes += entry.second->data.values.size() * sizeof(csm_geometry::Position);
    }
    for ( const auto &entry : s_Indices ) cpuBytes += entry.second->data.Bytes();
    for ( const auto &entry : s_Ranges )
    {
        if ( entry.second->vertices ) gpuBytes += entry.second->uniquePositions * sizeof(csm_geometry::Position);
        if ( entry.second->indices ) gpuBytes += entry.second->indexCount * sizeof(uint16_t);
    }
}

void CSMGeometryShutdown()
{
    AUTO_LOCK( s_Mutex );
    for ( const auto &entry : s_Ranges ) delete entry.second;
    s_Ranges.clear();
    for ( const auto &entry : s_Vertices ) Release( entry.second->packed );
    // CPU snapshots belong to source buffers, which may survive a device reset.
}
#endif
