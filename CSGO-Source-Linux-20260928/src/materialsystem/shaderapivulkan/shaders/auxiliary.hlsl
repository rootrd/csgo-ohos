#include "source_api.hlsl"
SOURCE_PIXEL_TEXTURE(baseTexture,baseSampler,0)
struct ClearVertex {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
};
struct ClearFragment {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 color : COLOR0;
};
ClearFragment ClearVS(ClearVertex input) {
    ClearFragment output;output.position=float4(input.position,1);output.color=input.color;return output;
}
float4 ClearPS(ClearFragment input) : SV_Target0 { return input.color; }
struct GeneralVertex {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
#if GENERAL_COLOR
    [[vk::location(2)]] float4 color : COLOR0;
#endif
};
struct GeneralFragment {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR0;
};
GeneralFragment GeneralVS(GeneralVertex input) {
    GeneralFragment output;float4 position=float4(input.position,1);
#if GENERAL_TRANSFORM
    output.position=float4(dot(sourceVertexC[4],position),dot(sourceVertexC[5],position),dot(sourceVertexC[6],position),dot(sourceVertexC[7],position));
#else
    output.position=position;
#endif
    output.uv=input.uv;
#if GENERAL_COLOR
    output.color=input.color;
#else
    output.color=1;
#endif
    return output;
}
float4 GeneralPS(GeneralFragment input) : SV_Target0 {
#if GENERAL_EFFECT == 2
    float4 value=sourcePixelC[0];
#elif GENERAL_EFFECT == 7
    float4 value=float4(0,1,0,1); // The legacy constant_color_ps20 is fixed green.
#else
    float4 value=baseTexture.Sample(baseSampler,input.uv);
#if GENERAL_EFFECT == 1
    value.rgb*=sourcePixelC[0].x;value.a=1;
#elif GENERAL_EFFECT == 3
    value.rgb*=saturate(sourcePixelC[0].x);value.a=pow(max(value.r,max(value.g,value.b)),.8);
#elif GENERAL_EFFECT == 4
    value.rgb=lerp(value.rgb,dot(value.rgb,float3(.2125,.7154,.0721)),saturate(sourcePixelC[1].x))*sourcePixelC[2].rgb;
    value.a=sourcePixelC[0].x;
#elif GENERAL_EFFECT == 5
    value=.25*(baseTexture.Sample(baseSampler,input.uv+sourcePixelC[0].xy)+baseTexture.Sample(baseSampler,input.uv-sourcePixelC[0].xy)+
        baseTexture.Sample(baseSampler,input.uv+sourcePixelC[1].xy)+baseTexture.Sample(baseSampler,input.uv-sourcePixelC[1].xy));
#elif GENERAL_EFFECT == 6
    float luminance=dot(value.rgb,float3(.2125,.7154,.0721));
    value=float4(1,1,1,step(sourcePixelC[0].x,luminance)*step(luminance,sourcePixelC[0].y));
#endif
#endif
    value*=input.color;SourceAlphaTest(value.a,sourceDraw.alpha);return value;
}
