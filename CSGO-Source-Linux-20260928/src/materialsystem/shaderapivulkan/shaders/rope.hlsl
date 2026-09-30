#include "source_api.hlsl"
#if !NATIVE_ROPE_DEPTH
SOURCE_PIXEL_TEXTURE(baseTexture,baseSampler,0)
SOURCE_PIXEL_TEXTURE(normalTexture,normalSampler,1)
#endif

struct RopeVertex {
    [[vk::location(0)]] float3 parameters : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
    [[vk::location(2)]] float4 point0 : TEXCOORD0;
    [[vk::location(3)]] float4 point1 : TEXCOORD1;
    [[vk::location(4)]] float4 point2 : TEXCOORD2;
    [[vk::location(5)]] float4 point3 : TEXCOORD3;
};
struct RopeFragment {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR0;
};
float4 ProjectRope(float3 position) {
    float4 p=float4(position,1);
    return float4(dot(sourceVertexC[8],p),dot(sourceVertexC[9],p),
                  dot(sourceVertexC[10],p),dot(sourceVertexC[11],p));
}
RopeFragment VSMain(RopeVertex input) {
    float t=input.parameters.x;
    float4 a=input.point0,b=input.point1,c=input.point2,d=input.point3;
    float4 quadratic=2*a-5*b+4*c-d,cubic=-a+3*b-3*c+d;
    float4 position=b+0.5*t*(c-a+t*(quadratic+t*cubic));
    float3 tangent=0.5*(c.xyz-a.xyz+2*t*quadratic.xyz+3*t*t*cubic.xyz);
    float3 offset=cross(position.xyz-sourceVertexC[48].xyz,tangent);
    offset*=rsqrt(max(dot(offset,offset),1e-20));
    // Keep distant cables at the requested pixel width in both viewport axes.
    float4 projected=ProjectRope(position.xyz),edge=ProjectRope(position.xyz+offset*position.w);
    if(projected.w>0 && edge.w>0) {
        float diameter=length((edge.xy/edge.w-projected.xy/projected.w)*sourceVertexC[49].xy*0.5);
        position.w*=max(1,sourceVertexC[48].w/max(diameter,1e-5));
    }
    RopeFragment output;
    output.position=ProjectRope(position.xyz+offset*position.w*(input.parameters.z-0.5));
    output.uv=float2(1-input.parameters.z,input.parameters.y*sourceVertexC[49].z);
    output.color=input.color;
    return output;
}
float4 PSMain(RopeFragment input) : SV_Target0 {
#if NATIVE_ROPE_DEPTH
    return float4(0,0,0,1);
#else
    float4 value=baseTexture.Sample(baseSampler,input.uv)*input.color*sourcePixelC[0]*sourceDraw.modulation;
    // Source's cable normal map lights a cylindrical cross section using a
    // squared half-Lambert term; the CPU has supplied each control point's light.
    float light=normalTexture.Sample(normalSampler,input.uv).z;
    value.rgb*=light*light;
    SourceAlphaTest(value.a,sourceDraw.alpha);
    return value;
#endif
}
