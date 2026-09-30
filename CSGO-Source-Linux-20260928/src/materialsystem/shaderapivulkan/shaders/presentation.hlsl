[[vk::binding(0, 0)]] Texture2D<float4> sourceColor;
[[vk::binding(1, 0)]] SamplerState sourceSampler;
struct PresentationConstants { float4 outputGamma; };
[[vk::push_constant]] ConstantBuffer<PresentationConstants> presentation;
struct ScreenVertex { float4 position : SV_Position; [[vk::location(0)]] float2 uv : TEXCOORD0; };
ScreenVertex VSMain(uint id : SV_VertexID)
{
    ScreenVertex output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv.x * 2 - 1, 1 - output.uv.y * 2, 0, 1);
    return output;
}
float4 PSMain(ScreenVertex input) : SV_Target0
{
    float4 value = sourceColor.SampleLevel(sourceSampler, input.uv, 0);
    value.rgb = saturate(pow(saturate(value.rgb), presentation.outputGamma.x) * presentation.outputGamma.y + presentation.outputGamma.z);
#if PRESENT_SRGB
    value.rgb = select(value.rgb <= 0.04045, value.rgb / 12.92, pow((value.rgb + 0.055) / 1.055, 2.4));
#endif
    return value;
}
