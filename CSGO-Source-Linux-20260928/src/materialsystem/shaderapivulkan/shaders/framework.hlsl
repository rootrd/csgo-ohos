// Framework integration asset, not a migrated Source material. The bindings and
// constant layouts are explicit and shared by vertex/pixel variants.
#include "source_registers.hlsl"
#include "source_alpha.hlsl"
[[vk::binding(2, 0)]] Texture2D<float4> colorTexture : register(t0);
[[vk::binding(3, 0)]] SamplerState colorSampler : register(s0);

struct DrawConstants
{
    float4 transform; // xy scale, zw translation
    float4 parameters; // x depth, yzw reserved
    SourceAlphaState alpha;
};
[[vk::push_constant]] ConstantBuffer<DrawConstants> drawConstants;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
    [[vk::location(2)]] float2 offset : TEXCOORD1;
    [[vk::location(3)]] float4 color : COLOR0;
    [[vk::location(4)]] float4 vertexColor : COLOR1;
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
    float2 position = input.position.xy * sourceVertexC[255].xy * (SourceVertexBool(15) ? sourceVertexI[15].x : 0);
    output.position = float4(position * drawConstants.transform.xy + input.offset + drawConstants.transform.zw,
                             drawConstants.parameters.x, 1.0);
    output.uv = input.uv;
    output.color = input.color * input.vertexColor;
    return output;
}
float4 PSMain(FragmentInput input) : SV_Target0
{
    float4 frameTint = sourcePixelC[223] * (SourcePixelBool(15) ? sourcePixelI[15].x : 0);
    float4 color = colorTexture.Sample(colorSampler, input.uv) * input.color * frameTint;
#if FRAMEWORK_ALPHA_TEST
    SourceAlphaTest(color.a, drawConstants.alpha);
#endif
    return color;
}
