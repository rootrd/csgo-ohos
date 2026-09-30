#include "../csm_geometry_data.h"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace csm_geometry;

static void CheckTriangles( const Snapshot<Position> &vertices, const Snapshot<uint16_t> &indices,
                            int firstVertex, int firstIndex, int count, const Range &range )
{
    assert( range.indices.size() == size_t(count) );
    for ( int i = 0; i < count; ++i )
        assert( vertices.values[firstVertex + indices.values[firstIndex + i]] == range.positions[range.indices[i]] );
}

int main()
{
    struct FullVertex { Position p; uint32_t other[5]; };
    const FullVertex input[] = {
        {{0x3f800000u, 0, 0}, {1, 2, 3, 4, 5}},
        {{0x3f800000u, 0, 0}, {6, 7, 8, 9, 10}}, // same position, different normals/UV/etc.
        {{0x3f800000u, 0x80000000u, 0}, {}},      // signed zero must remain distinct
        {{0x3f800001u, 0, 0}, {}},               // one ULP apart must remain distinct
        {{0x7fc00001u, 0, 0}, {}},               // retain the exact NaN payload too
        {{0x7fc00002u, 0, 0}, {}}
    };
    Snapshot<Position> vertices;
    Snapshot<uint16_t> indices;
    assert( vertices.Update( 8, 2, 6, 6, input, sizeof(FullVertex) ) );
    const uint16_t sourceIndices[] = {0, 1, 2, 1, 3, 4, 5, 5, 0}; // keep the degenerate triangle
    assert( indices.Update( 10, 0, 9, 9, sourceIndices, sizeof(uint16_t) ) ); // aligned IB has one unwritten entry
    Range range;
    assert( BuildRange( vertices, indices, 2, 0, 9, range ) );
    assert( range.referencedVertices == 6 && range.positions.size() == 5 );
    CheckTriangles( vertices, indices, 2, 0, 9, range );
    assert( BuildRange( vertices, indices, 2, 3, 6, range ) );
    CheckTriangles( vertices, indices, 2, 3, 6, range );

    // Unknown upload bytes and invalid ranges must select the original renderer.
    assert( !BuildRange( vertices, indices, 0, 0, 9, range ) );
    assert( !BuildRange( vertices, indices, 2, 1, 9, range ) );
    assert( !BuildRange( vertices, indices, 2, 0, 8, range ) );
    assert( !BuildRange( vertices, indices, -1, 0, 9, range ) );
    assert( !BuildRange( vertices, indices, 2, std::numeric_limits<int>::max(), 9, range ) );
    assert( !BuildRange( vertices, indices, 2, 0, std::numeric_limits<int>::max(), range ) );

    // A later partial write invalidates the unwritten part of its lock.
    assert( vertices.Update( 8, 2, 2, 1, input, sizeof(FullVertex) ) );
    assert( !BuildRange( vertices, indices, 2, 0, 9, range ) );
    assert( vertices.Update( 8, 3, 1, 1, input + 3, sizeof(FullVertex) ) );
    assert( BuildRange( vertices, indices, 2, 0, 9, range ) );
    CheckTriangles( vertices, indices, 2, 0, 9, range );
    assert( !vertices.Update( 8, 7, 2, 2, input, sizeof(FullVertex) ) );
    assert( vertices.values.empty() && !BuildRange( vertices, indices, 2, 0, 9, range ) );

    // Exercise the complete 16-bit index domain, including index 65535 and a stream offset.
    std::vector<Position> manyVertices( 65538 );
    std::vector<uint16_t> manyIndices( 65538 );
    for ( unsigned i = 0; i < 65536; ++i )
    {
        manyVertices[i + 2].bits[0] = i;
        manyIndices[i] = uint16_t(i);
    }
    manyIndices[65536] = 65535;
    manyIndices[65537] = 0;
    assert( vertices.Update( 65538, 0, 65538, 65538, manyVertices.data(), sizeof(Position) ) );
    assert( indices.Update( 65538, 0, 65538, 65538, manyIndices.data(), sizeof(uint16_t) ) );
    assert( BuildRange( vertices, indices, 2, 0, 65538, range ) );
    assert( range.positions.size() == 65536 && range.referencedVertices == 65536 );
    CheckTriangles( vertices, indices, 2, 0, 65538, range );
    std::puts( "CSM geometry: exact triangles, bit patterns, partial updates and 16-bit boundaries passed" );
}
