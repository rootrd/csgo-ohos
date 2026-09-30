#ifndef CSM_GEOMETRY_H
#define CSM_GEOMETRY_H

#if defined( ANDROID ) && defined( USE_DXVK_NATIVE )
struct CSMVertexData;
struct CSMIndexData;

// Called while original CPU upload data is still accessible, before Unlock.
void CSMGeometryUploadVertices( CSMVertexData *&cache, const void *data, int stride,
                               int total, int first, int locked, int written );
void CSMGeometryUploadIndices( CSMIndexData *&cache, const void *data,
                              int total, int first, int locked, int written );
void CSMGeometryDestroyVertices( CSMVertexData *&cache );
void CSMGeometryDestroyIndices( CSMIndexData *&cache );

struct CSMGeometryBinding
{
    IDirect3DVertexBuffer9 *vertices;
    IDirect3DIndexBuffer9 *indices;
    int stride, vertexOffset, vertexCount, indexOffset, mode;
    int referencedVertices, uniquePositions;
    bool inspected;
    CSMGeometryBinding();
    ~CSMGeometryBinding();
private:
    CSMGeometryBinding( const CSMGeometryBinding & );
    CSMGeometryBinding &operator=( const CSMGeometryBinding & );
};

int CSMGeometryMode();
void CSMGeometryPrepare( CSMVertexData *vertices, CSMIndexData *indices, int stride,
                         int vertexOffset, int firstIndex, int indexCount, bool inspect,
                         CSMGeometryBinding &binding );
void CSMGeometryMemory( int &ranges, size_t &cpuBytes, size_t &gpuBytes );
void CSMGeometryShutdown();
#endif
#endif
