// ShaderApi029's initial explicit resource contract. Names/combos still come
// from the offline manifest; semantic locations are registered with SourceAPI.
#include "source_registers.hlsl"
#include "source_alpha.hlsl"

struct SourceDrawConstants
{
    SourceAlphaState alpha;
    float4 modulation;
};
[[vk::push_constant]] ConstantBuffer<SourceDrawConstants> sourceDraw;

// VS/PS register banks occupy bindings 0/1. PS textures/samplers use 2..33;
// VS textures/samplers use 34..41. Only declared/enabled samplers are allocated.
#define SOURCE_PIXEL_TEXTURE(textureName, samplerName, index) \
    [[vk::binding(2 + 2 * index, 0)]] Texture2D<float4> textureName; \
    [[vk::binding(3 + 2 * index, 0)]] SamplerState samplerName;
#define SOURCE_VERTEX_TEXTURE(textureName, samplerName, index) \
    [[vk::binding(34 + 2 * index, 0)]] Texture2D<float4> textureName; \
    [[vk::binding(35 + 2 * index, 0)]] SamplerState samplerName;
