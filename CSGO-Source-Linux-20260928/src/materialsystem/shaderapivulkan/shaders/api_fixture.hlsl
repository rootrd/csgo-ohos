// Interface acceptance asset; not a migrated UnlitGeneric/Source material.
#include "source_api.hlsl"
SOURCE_PIXEL_TEXTURE(colorTexture, colorSampler, 0)

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float4 color : COLOR0;
};
struct FragmentInput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR0;
};
FragmentInput VSMain(VertexInput input)
{
    FragmentInput output;
    float4x4 transform = float4x4(sourceVertexC[0], sourceVertexC[1], sourceVertexC[2], sourceVertexC[3]);
    output.position = mul(transform, float4(input.position, 1));
    output.uv = input.uv;
    output.color = input.color * sourceVertexC[255] * (SourceVertexBool(15) ? sourceVertexI[15].x : 0);
    return output;
}
float4 PSMain(FragmentInput input) : SV_Target0
{
    float4 color = colorTexture.SampleLevel(colorSampler, input.uv, sourcePixelC[1].x) * input.color;
    color *= sourcePixelC[0] * sourcePixelC[223] * (SourcePixelBool(15) ? sourcePixelI[15].x : 0) * sourceDraw.modulation;
    SourceAlphaTest(color.a, sourceDraw.alpha);
    return color;
}
