#include "source_api.hlsl"
SOURCE_PIXEL_TEXTURE(firstTexture, firstSampler, 0)
SOURCE_PIXEL_TEXTURE(secondTexture, secondSampler, 1)
struct VertexInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
#if DUAL_COLOR
    [[vk::location(2)]] float4 color : COLOR0;
#endif
#if DUAL_SKIN
    [[vk::location(3)]] float2 weights : BLENDWEIGHT;
    [[vk::location(4)]] uint4 bones : BLENDINDICES;
#endif
};
struct FragmentInput {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR0;
};
float4 Project(float4 position,uint first) {
    return float4(dot(sourceVertexC[first],position),dot(sourceVertexC[first+1],position),
        dot(sourceVertexC[first+2],position),dot(sourceVertexC[first+3],position));
}
FragmentInput VSMain(VertexInput input) {
    FragmentInput output;
    float4 position=float4(input.position,1);
#if DUAL_SKIN
    float3 weights=float3(input.weights,max(0,1-input.weights.x-input.weights.y)),world=0;
    [unroll] for(uint i=0;i<3;++i) {
        uint bone=58+3*min(input.bones[i],52);
        world+=float3(dot(sourceVertexC[bone],position),dot(sourceVertexC[bone+1],position),dot(sourceVertexC[bone+2],position))*weights[i];
    }
    output.position=Project(float4(world,1),8);
#else
    output.position=Project(position,4);
#endif
    float4 uv=float4(input.uv,0,1);
    output.uv=float4(dot(sourceVertexC[48],uv),dot(sourceVertexC[49],uv),dot(sourceVertexC[50],uv),dot(sourceVertexC[51],uv));
#if DUAL_COLOR
    output.color=input.color;
#else
    output.color=1;
#endif
    return output;
}
float FoamWave(float2 uv,float time,float yOffset) {
    float phase=frac(time/16);
    float curve=pow(2.95,2.95)/(pow(.95,.95)*4)*pow(phase,.95)*pow(1-phase,2);
    uv*=1-.05*sin(time);uv.x+=curve-1;uv.y-=.1*sin(time*.5)+yOffset;
    float foam=firstTexture.Sample(firstSampler,uv).r;
    return saturate(foam-(1-smoothstep(0,.2,phase)*smoothstep(.9,.5,phase)));
}
float4 PSMain(FragmentInput input) : SV_Target0 {
#if DUAL_EFFECT == 1
    // The original crosshair repeats the same center sample five times.
    float luminance=dot(secondTexture.Sample(secondSampler,float2(.5,.5)).rgb,float3(.299,.587,.114));
    float4 value=firstTexture.Sample(firstSampler,input.uv.xy);
    value.rgb=lerp(value.rgb+sourcePixelC[1].rgb,value.rgb*sourcePixelC[1].rgb,luminance*sourcePixelC[1].a);
#elif DUAL_EFFECT == 2
    float2 uv=input.uv.xy,distortion=firstTexture.Sample(firstSampler,uv).gb;
    uv+=distortion.x*.06;uv.x*=1+(distortion.y-.5)*.6;
    float foam=FoamWave(uv,sourcePixelC[1].x,0)+FoamWave(uv,sourcePixelC[1].x+8,.5);
    float4 value=foam*(1-input.uv.x)*(.8-pow(uv.y-.9,2));
#else
    float4 value=firstTexture.Sample(firstSampler,input.uv.xy)*secondTexture.Sample(secondSampler,input.uv.zw);
#endif
    value*=sourcePixelC[0]*input.color*sourceDraw.modulation;
#if !DUAL_TRANSLUCENT && DUAL_EFFECT == 0
    value.a=1;
#endif
    SourceAlphaTest(value.a,sourceDraw.alpha);
    return value;
}
