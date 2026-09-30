#include "source_api.hlsl"
SOURCE_PIXEL_TEXTURE(baseTexture,baseSampler,0)
#if NATIVE_SCREEN_EFFECT == 3
SOURCE_PIXEL_TEXTURE(frameTexture,frameSampler,1)
SOURCE_PIXEL_TEXTURE(vignetteTexture,vignetteSampler,2)
#endif
struct ScreenVertex {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float2 uv : TEXCOORD0;
};
struct ScreenFragment {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float2 bloomUV : TEXCOORD1;
};
ScreenFragment VSMain(ScreenVertex input) {
    ScreenFragment output;
    output.position=float4(input.position,1);output.uv=input.uv;
    output.bloomUV=input.uv*sourceVertexC[48].zw+sourceVertexC[48].xy;
    return output;
}
#if NATIVE_SCREEN_EFFECT == 3
float3 SmoothEdges(float2 uv,float3 center) {
    float2 texel=sourcePixelC[0].xy;
    const float3 luma=float3(.299,.587,.114);
    float3 nw=frameTexture.Sample(frameSampler,uv+float2(-1,-1)*texel).rgb;
    float3 ne=frameTexture.Sample(frameSampler,uv+float2(1,-1)*texel).rgb;
    float3 sw=frameTexture.Sample(frameSampler,uv+float2(-1,1)*texel).rgb;
    float3 se=frameTexture.Sample(frameSampler,uv+float2(1,1)*texel).rgb;
    float4 corners=float4(dot(nw,luma),dot(ne,luma),dot(sw,luma),dot(se,luma));
    float luminance=dot(center,luma),lo=min(luminance,min(min(corners.x,corners.y),min(corners.z,corners.w)));
    float hi=max(luminance,max(max(corners.x,corners.y),max(corners.z,corners.w)));
    if(hi-lo<max(.0312,hi*.125))return center;
    float2 direction=float2(-(corners.x+corners.y-corners.z-corners.w),corners.x+corners.z-corners.y-corners.w);
    float reduce=max(dot(corners,.03125),.0078125);
    direction=clamp(direction/(min(abs(direction.x),abs(direction.y))+reduce),-8,8)*texel;
    float3 a=.5*(frameTexture.Sample(frameSampler,uv-direction/6).rgb+frameTexture.Sample(frameSampler,uv+direction/6).rgb);
    float3 b=a*.5+.25*(frameTexture.Sample(frameSampler,uv-direction*.5).rgb+frameTexture.Sample(frameSampler,uv+direction*.5).rgb);
    float lb=dot(b,luma);return lb<lo || lb>hi?a:b;
}
#endif
float4 PSMain(ScreenFragment input) : SV_Target0 {
    float2 uv=input.uv,texel=sourcePixelC[0].xy;
#if NATIVE_SCREEN_EFFECT == 0 || NATIVE_SCREEN_EFFECT == 1
#if NATIVE_SCREEN_EFFECT == 0
    float2 low=-texel,high=texel;
#else
    float2 low=.5*texel,high=2.5*texel;
#endif
    float4 value=.25*(baseTexture.Sample(baseSampler,uv+low)+baseTexture.Sample(baseSampler,uv+high)+
        baseTexture.Sample(baseSampler,uv+float2(low.x,high.y))+baseTexture.Sample(baseSampler,uv+float2(high.x,low.y)));
#if NATIVE_SCREEN_EFFECT == 1
    float luminance=dot(value.rgb,float3(.299,.587,.114));
    if(sourcePixelC[1].z<.5)value.rgb=pow(max(value.rgb,0),sourcePixelC[2].w)*dot(value.rgb,sourcePixelC[2].rgb);
    else {
        value.rgb=pow(saturate(min(.59,value.rgb*1.55-.09)),sourcePixelC[1].x);
        value.rgb=lerp(dot(value.rgb,sourcePixelC[2].rgb),value.rgb,sourcePixelC[1].y);
    }
    value.a=luminance;
#endif
    return value;
#elif NATIVE_SCREEN_EFFECT == 2
    int kernel=int(sourcePixelC[1].x);
    float4 value=0;
    // Preserve Source's original bloom filter and all four Gaussian kernels.
    if(kernel==0) {
        const float offsets[6]={1.3366,3.4295,5.4264,7.4359,9.4436,11.4401};
        const float weights[6]={.2185,.0821,.0461,.0262,.0162,.0102};
        value=saturate(baseTexture.Sample(baseSampler,uv))*.2013;
        [unroll] for(int i=0;i<6;++i)value+=weights[i]*(saturate(baseTexture.Sample(baseSampler,uv+offsets[i]*texel))+
            saturate(baseTexture.Sample(baseSampler,uv-offsets[i]*texel)));
    } else {
        const float offsets[4][6]={{1.182425,3,0,0,0,0},{1.276878,3.096215,0,0,0,0},
            {1.379942,3.241796,5.142349,0,0,0},{1.464557,3.417910,5.372686,7.329586,9.289172,11.251852}};
        const float weights[4][7]={{.399050,.296042,.004433,0,0,0,0},{.319224,.320561,.019827,0,0,0,0},
            {.228005,.312325,.069185,.004487,0,0,0},{.122765,.218677,.137740,.059928,.018004,.003733,.000534}};
        int row=kernel-1,count=kernel==4?6:kernel==3?3:2;
        value=baseTexture.Sample(baseSampler,uv)*weights[row][0];
        for(int i=0;i<count;++i)value+=weights[row][i+1]*(baseTexture.Sample(baseSampler,uv+offsets[row][i]*texel)+
            baseTexture.Sample(baseSampler,uv-offsets[row][i]*texel));
    }
    value.rgb=sourcePixelC[1].y>0?sourcePixelC[2].rgb:value.rgb*sourcePixelC[0].z;
    return value;
#elif NATIVE_SCREEN_EFFECT == 3
    float4 frame=frameTexture.Sample(frameSampler,uv);
    float3 value=sourcePixelC[0].z>0?SmoothEdges(uv,frame.rgb):frame.rgb;
    float3 bloom=baseTexture.Sample(baseSampler,input.bloomUV).rgb*.5;
    float blur=saturate(sourcePixelC[1].y+abs(frame.a-sourcePixelC[3].w)*sourcePixelC[3].z);
    value=lerp(value,bloom,blur)+bloom*sourcePixelC[1].x;
    if(sourcePixelC[1].w==1)value=lerp(value,sourcePixelC[2].rgb,sourcePixelC[2].a);
    if(sourcePixelC[1].w==2)value=lerp(value,value*sourcePixelC[2].rgb,sourcePixelC[2].a);
    value=lerp(value,dot(value,float3(.3,.59,.11)),saturate(sourcePixelC[3].x));
    if(sourcePixelC[3].y>0)value*=vignetteTexture.Sample(vignetteSampler,uv).r*.33+.67;
    return float4(value*(1-saturate(sourcePixelC[1].z)),frame.a);
#else
    return baseTexture.Sample(baseSampler,uv);
#endif
}
