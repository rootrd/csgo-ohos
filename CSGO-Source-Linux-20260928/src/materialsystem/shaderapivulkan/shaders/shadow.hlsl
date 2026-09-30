// Ported from shadow_vs20, shadow_ps2x and shadowbuildtexture_ps2x.
// Preserve PC coverage/filter/falloff and linear-space multiplicative blending.
#include "source_api.hlsl"
SOURCE_PIXEL_TEXTURE(shadowTexture, shadowSampler, 0)

float3 ShadowWorldPosition(float3 position, uint bone)
{
    uint first=58+3*min(bone,52);
    float4 value=float4(position,1);
    return float3(dot(sourceVertexC[first],value),dot(sourceVertexC[first+1],value),dot(sourceVertexC[first+2],value));
}
float4 ShadowProjection(float3 position)
{
    float4 value=float4(position,1);
    return float4(dot(sourceVertexC[8],value),dot(sourceVertexC[9],value),
        dot(sourceVertexC[10],value),dot(sourceVertexC[11],value));
}

#if SHADOW_BUILD
struct BuildInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
#if SHADOW_SKIN
    [[vk::location(2)]] float2 weights : BLENDWEIGHT;
    [[vk::location(3)]] uint4 bones : BLENDINDICES;
#endif
};
struct BuildFragment
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};
BuildFragment BuildVS(BuildInput input)
{
    BuildFragment output;
#if SHADOW_SKIN
    float3 weights=float3(input.weights,1-input.weights.x-input.weights.y);
    float3 world=0;
    [unroll] for(uint i=0;i<3;++i)world+=ShadowWorldPosition(input.position,input.bones[i])*weights[i];
#else
    float3 world=ShadowWorldPosition(input.position,0);
#endif
    output.position=ShadowProjection(world);
    float4 uv=float4(input.uv,0,1);
    output.uv=float2(dot(sourceVertexC[48],uv),dot(sourceVertexC[49],uv));
    return output;
}
float4 BuildPS(BuildFragment input) : SV_Target0
{
    float alpha=shadowTexture.Sample(shadowSampler,input.uv).a*sourcePixelC[0].a*sourceDraw.modulation.a;
    return float4(1,1,1,alpha);
}
#else
struct ShadowInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 uv : TEXCOORD0;
    [[vk::location(2)]] float3 falloff : TEXCOORD1;
};
struct ShadowFragment
{
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world : TEXCOORD0;
    [[vk::location(1)]] float4 falloff : TEXCOORD1;
    [[vk::location(2)]] float2 uv : TEXCOORD2;
    [[vk::location(3)]] float2 jitter : TEXCOORD3;
};
ShadowFragment ShadowVS(ShadowInput input)
{
    ShadowFragment output;
    output.world=ShadowWorldPosition(input.position,0);
    output.position=ShadowProjection(output.world);
    output.falloff=float4(input.falloff,input.uv.z);
    float4 uv=float4(input.uv,1);
    output.uv=float2(dot(sourceVertexC[48],uv),dot(sourceVertexC[49],uv));
    output.jitter=sourceVertexC[50].xy;
    return output;
}
float4 ShadowPS(ShadowFragment input) : SV_Target0
{
    float coverage=shadowTexture.Sample(shadowSampler,input.uv).a;
#if !SHADOW_BLOBBY
    float2 diagonal=input.jitter;
    float2 otherDiagonal=float2(diagonal.x,-diagonal.y);
    coverage+=shadowTexture.Sample(shadowSampler,input.uv+diagonal).a;
    coverage+=shadowTexture.Sample(shadowSampler,input.uv-diagonal).a;
    coverage+=shadowTexture.Sample(shadowSampler,input.uv+otherDiagonal).a;
    coverage+=shadowTexture.Sample(shadowSampler,input.uv-otherDiagonal).a;
    coverage*=0.2;
#endif
    float fade=saturate(input.falloff.w*input.falloff.y+input.falloff.x);
    fade=saturate(input.falloff.z+fade*sourcePixelC[4].x);
    coverage=saturate(coverage-fade);
    float3 result=1+coverage*(sourcePixelC[1].rgb-1);
#if SHADOW_WATER_FOG
    float fog=saturate((sourcePixelC[3].y-input.world.z-2)*sourcePixelC[3].w);
#else
    float fog=min(sourcePixelC[3].z,saturate(sourcePixelC[3].x+distance(input.world,sourcePixelC[2].xyz)*sourcePixelC[3].w));
#endif
    // Compensate for multiplying an already fogged scene, as the PC shader does.
    result=1-(1-result)*pow(1-fog,4);
    return float4(result,1);
}
#endif
