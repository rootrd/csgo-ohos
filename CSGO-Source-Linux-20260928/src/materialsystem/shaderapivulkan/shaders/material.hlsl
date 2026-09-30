#include "source_api.hlsl"
SOURCE_PIXEL_TEXTURE(baseTexture, baseSampler, 0)
#if NATIVE_LIGHTMAP
SOURCE_PIXEL_TEXTURE(lightTexture, lightSampler, 1)
#endif
#if NATIVE_TRANSITION
SOURCE_PIXEL_TEXTURE(secondTexture, secondSampler, 2)
#endif

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
#if NATIVE_COLOR
    [[vk::location(2)]] float4 color : COLOR0;
#endif
#if NATIVE_LIGHTMAP
    [[vk::location(3)]] float2 lightUV : TEXCOORD1;
#endif
#if NATIVE_MODEL
    [[vk::location(4)]] float3 normal : NORMAL;
#if NATIVE_STATIC_LIGHT
    [[vk::location(7)]] float4 staticLight0 : COLOR1;
    [[vk::location(8)]] float4 staticLight1 : COLOR2;
    [[vk::location(9)]] float4 staticLight2 : COLOR3;
#endif
#if NATIVE_SKIN
    [[vk::location(5)]] float2 weights : BLENDWEIGHT;
    [[vk::location(6)]] uint4 bones : BLENDINDICES;
#endif
#endif
};
struct FragmentInput
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float2 lightUV : TEXCOORD1;
    [[vk::location(3)]] float3 lighting : TEXCOORD2;
};
float4 TransformPosition(float3 position, int first)
{
    float4 v=float4(position,1);
    return float4(dot(sourceVertexC[first],v),dot(sourceVertexC[first+1],v),dot(sourceVertexC[first+2],v),dot(sourceVertexC[first+3],v));
}
float3 BonePosition(float3 position,uint bone)
{
    uint first=58+3*min(bone,52);
    float4 v=float4(position,1);
    return float3(dot(sourceVertexC[first],v),dot(sourceVertexC[first+1],v),dot(sourceVertexC[first+2],v));
}
float3 BoneNormal(float3 normal,uint bone)
{
    uint first=58+3*min(bone,52);
    return float3(dot(sourceVertexC[first].xyz,normal),dot(sourceVertexC[first+1].xyz,normal),dot(sourceVertexC[first+2].xyz,normal));
}
FragmentInput VSMain(VertexInput input)
{
    FragmentInput output;
    float3 position=input.position;
    output.position=TransformPosition(position,4);
    output.lighting=1;
#if NATIVE_MODEL
    float3 normal;
#if NATIVE_SKIN
    float3 weights=float3(input.weights,max(0,1-input.weights.x-input.weights.y));
    position=0;normal=0;
    [unroll] for(uint i=0;i<3;++i) {
        position+=BonePosition(input.position,input.bones[i])*weights[i];
        normal+=BoneNormal(input.normal,input.bones[i])*weights[i];
    }
#else
    position=BonePosition(input.position,0);normal=BoneNormal(input.normal,0);
#endif
    normal=normalize(normal);
    output.position=TransformPosition(position,8);
    float3 n2=normal*normal;
    output.lighting=sourceVertexC[21+(normal.x<0)].rgb*n2.x+
        sourceVertexC[23+(normal.y<0)].rgb*n2.y+sourceVertexC[25+(normal.z<0)].rgb*n2.z;
#if NATIVE_STATIC_LIGHT
    // Source packs baked lighting in gamma space with an overbright factor of 2.
    // One-component streams bind the same color to all three inputs.
    output.lighting=(pow(input.staticLight0.rgb*2,2.2)+pow(input.staticLight1.rgb*2,2.2)+pow(input.staticLight2.rgb*2,2.2))/3;
#endif
    [unroll] for(uint light=0;light<4;++light) {
        uint reg=27+light*5;
        float4 lightColor=sourceVertexC[reg];
        float3 delta=sourceVertexC[reg+1].xyz-position;
        float distance=max(length(delta),0.0001);
        float3 direction=lightColor.w==2 ? -sourceVertexC[reg+2].xyz : delta/distance;
        float attenuation=lightColor.w==2 ? 1 : rcp(max(dot(sourceVertexC[reg+3].xyz,float3(1,distance,distance*distance)),0.0001));
        if(lightColor.w==3) {
            float cone=saturate((dot(-direction,sourceVertexC[reg+2].xyz)-sourceVertexC[reg+4].x)*sourceVertexC[reg+4].y);
            attenuation*=pow(cone,max(sourceVertexC[reg+2].w,0.0001));
        }
        output.lighting+=lightColor.rgb*max(0,dot(normal,direction))*attenuation;
    }
#endif
    float4 uv=float4(input.uv,0,1);
    output.uv=float2(dot(sourceVertexC[48],uv),dot(sourceVertexC[49],uv));
#if NATIVE_COLOR
    output.color=input.color;
#else
    output.color=1;
#endif
#if NATIVE_LIGHTMAP
    output.lightUV=input.lightUV;
#else
    output.lightUV=0;
#endif
    return output;
}
float4 PSMain(FragmentInput input) : SV_Target0
{
    float4 value=baseTexture.Sample(baseSampler,input.uv);
#if NATIVE_TRANSITION
    value=lerp(value,secondTexture.Sample(secondSampler,input.uv),input.color.a);
    input.color=1;
#endif
#if NATIVE_DECAL
    value.rgb=lerp(float3(.5,.5,.5),value.rgb,input.color.a*sourcePixelC[0].a*sourceDraw.modulation.a);
    value.rgb*=sourcePixelC[0].rgb*sourceDraw.modulation.rgb;
#else
    value*=input.color*sourcePixelC[0]*sourceDraw.modulation;
#endif
    SourceAlphaTest(value.a,sourceDraw.alpha);
#if NATIVE_LIGHTMAP
    value.rgb*=lightTexture.Sample(lightSampler,input.lightUV).rgb;
#endif
#if NATIVE_LIT
    value.rgb*=input.lighting;
#endif
#if NATIVE_DEPTH
    value=float4(input.position.zzz,1);
#endif
    return value;
}
