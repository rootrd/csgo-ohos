// CPU-only data preparation shared by the Android CSM cache and its tests.
#ifndef CSM_GEOMETRY_DATA_H
#define CSM_GEOMETRY_DATA_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace csm_geometry
{
// Compare the original float bits, including signed zero. Never quantize or
// evaluate positions on the CPU: the existing vertex shader still transforms them.
struct Position
{
    uint32_t bits[3];
    bool operator==( const Position &p ) const
    {
        return bits[0] == p.bits[0] && bits[1] == p.bits[1] && bits[2] == p.bits[2];
    }
};
static_assert( sizeof(Position) == 12, "CSM positions must remain float3" );

struct PositionHash
{
    size_t operator()( const Position &p ) const
    {
        size_t h = 2166136261u;
        for ( int i = 0; i < 3; ++i ) h = ( h ^ p.bits[i] ) * 16777619u;
        return h;
    }
};

template <typename T> struct Snapshot
{
    std::vector<T> values;
    std::vector<unsigned char> valid;

    // A lock may be only partly written. Invalidate its entire range before
    // retaining the known bytes; this also handles incremental uploads and IB padding.
    bool Update( int total, int first, int locked, int written, const void *data, int stride )
    {
        if ( total <= 0 || first < 0 || first > total || locked < 0 || locked > total - first ||
             written < 0 || written > locked || stride < int(sizeof(T)) || ( written && !data ) )
        {
            values.clear(); valid.clear();
            return false;
        }
        if ( values.size() != size_t(total) )
        {
            values.resize( total );
            valid.assign( total, 0 );
        }
        std::fill( valid.begin() + first, valid.begin() + first + locked, 0 );
        const unsigned char *bytes = static_cast<const unsigned char *>( data );
        for ( int i = 0; i < written; ++i )
        {
            std::memcpy( &values[first + i], bytes + size_t(i) * stride, sizeof(T) );
            valid[first + i] = 1;
        }
        return true;
    }

    size_t Bytes() const { return values.capacity() * sizeof(T) + valid.capacity(); }
};

struct Range
{
    std::vector<Position> positions;
    std::vector<uint16_t> indices;
    int referencedVertices = 0;
};

inline bool BuildRange( const Snapshot<Position> &vertices, const Snapshot<uint16_t> &indices,
                        int firstVertex, int firstIndex, int indexCount, Range &out )
{
    out = Range();
    if ( firstVertex < 0 || size_t(firstVertex) >= vertices.values.size() ||
         firstIndex < 0 || indexCount <= 0 || indexCount % 3 ||
         size_t(firstIndex) > indices.values.size() ||
         size_t(indexCount) > indices.values.size() - firstIndex ) return false;

    std::vector<unsigned char> seen( 65536, 0 );
    std::unordered_map<Position, uint16_t, PositionHash> positions;
    out.indices.reserve( indexCount );
    for ( int i = 0; i < indexCount; ++i )
    {
        if ( !indices.valid[firstIndex + i] ) return false;
        const uint16_t originalIndex = indices.values[firstIndex + i];
        const size_t vertex = size_t(firstVertex) + originalIndex;
        if ( vertex >= vertices.values.size() || !vertices.valid[vertex] ) return false;
        if ( !seen[originalIndex] ) { seen[originalIndex] = 1; ++out.referencedVertices; }
        const Position &p = vertices.values[vertex];
        auto found = positions.find( p );
        if ( found == positions.end() )
        {
            if ( out.positions.size() >= 65536 ) return false;
            const uint16_t compactIndex = uint16_t(out.positions.size());
            positions.emplace( p, compactIndex );
            out.positions.push_back( p );
            out.indices.push_back( compactIndex );
        }
        else
            out.indices.push_back( found->second );
    }
    // Every original index produces exactly one output index, in original order.
    return true;
}
}
#endif
