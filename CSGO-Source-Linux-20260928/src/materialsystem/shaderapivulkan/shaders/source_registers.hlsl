// Source SM3 register ABI. Arrays of bool in a cbuffer have a different stride;
// pack b0..b15 into uint4 explicitly so C++ stores sixteen 32-bit values (64 B).
[[vk::binding(0, 0)]] cbuffer SourceVertexRegisters
{
    float4 sourceVertexC[256];
    int4 sourceVertexI[16];
    uint4 sourceVertexB[4];
};
[[vk::binding(1, 0)]] cbuffer SourcePixelRegisters
{
    float4 sourcePixelC[224];
    int4 sourcePixelI[16];
    uint4 sourcePixelB[4];
};
bool SourceVertexBool(uint index) { return sourceVertexB[index / 4][index % 4] != 0; }
bool SourcePixelBool(uint index) { return sourcePixelB[index / 4][index % 4] != 0; }
