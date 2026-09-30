#include "shaderlib/cshader.h"

#include <algorithm>

namespace {
enum class ScreenEffect { Downsample, BloomDownsample, BlurX, BlurY, Post, FloatCopy };
enum {
    FrameBuffer=NUM_SHADER_MATERIAL_VARS, BloomEnable, BloomAmount, BloomType, BloomExponent,
    BloomSaturation, BloomTint, Kernel, EnableClearColor, ClearColor, AAEnable, AAInternal1,
    AAInternal2, AAInternal3, FXAAInternalC, FXAAInternalQ, ScreenBlur, DepthBlurEnable,
    DepthBlurStrength, DepthBlurFocalDistance, FadeToBlack, Fade, FadeColor, DesaturateEnable,
    Desaturation, AllowVignette, VignetteEnable, VignetteTexture, Panorama, ParameterCount
};
const ShaderParamInfo_t screenParameters[] = {
    {"$fbtexture","Full framebuffer",SHADER_PARAM_TYPE_TEXTURE,"_rt_FullFrameFB",0},
    {"$bloomenable","Enable bloom",SHADER_PARAM_TYPE_INTEGER,"1",0},
    {"$bloomamount","Bloom scale",SHADER_PARAM_TYPE_FLOAT,"1",0},
    {"$bloomtype","Bloom shaping",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$bloomexp","Bloom exponent",SHADER_PARAM_TYPE_FLOAT,"2.5",0},
    {"$bloomsaturation","Bloom saturation",SHADER_PARAM_TYPE_FLOAT,"1",0},
    {"$bloomtintenable","Enable bloom tint",SHADER_PARAM_TYPE_INTEGER,"1",0},
    {"$kernel","Blur kernel",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$enableclearcolor","Clear blur RGB",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$clearcolor","Blur clear RGB",SHADER_PARAM_TYPE_VEC3,"[0 0 0]",0},
    {"$aaenable","Screen edge smoothing",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$aainternal1","Engine AA parameters",SHADER_PARAM_TYPE_VEC4,"[0 0 0 0]",0},
    {"$aainternal2","Framebuffer to bloom UV transform",SHADER_PARAM_TYPE_VEC4,"[0 0 1 1]",0},
    {"$aainternal3","Engine AA parameters",SHADER_PARAM_TYPE_VEC4,"[0 0 0 0]",0},
    {"$fxaainternalc","Engine AA parameters",SHADER_PARAM_TYPE_VEC4,"[0 0 0 0]",0},
    {"$fxaainternalq","Engine AA parameters",SHADER_PARAM_TYPE_VEC4,"[0 0 0 0]",0},
    {"$screenblurstrength","Screen blur",SHADER_PARAM_TYPE_FLOAT,"0",0},
    {"$depthblurenable","Enable depth blur",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$depthblurstrength","Depth blur",SHADER_PARAM_TYPE_FLOAT,"0",0},
    {"$depthblurfocaldistance","Depth blur focus",SHADER_PARAM_TYPE_FLOAT,"0",0},
    {"$fadetoblackscale","Fade to black",SHADER_PARAM_TYPE_FLOAT,"0",0},
    {"$fade","View fade mode",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$fadecolor","View fade RGBA",SHADER_PARAM_TYPE_VEC4,"[0 0 0 0]",0},
    {"$desaturateenable","Enable desaturation",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$desaturation","Desaturation",SHADER_PARAM_TYPE_FLOAT,"0",0},
    {"$allowvignette","Allow vignette",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$vignetteenable","Enable vignette",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$internal_vignettetexture","Vignette image",SHADER_PARAM_TYPE_TEXTURE,"dev/vignette",0},
    {"$panorama","Preserve panel alpha",SHADER_PARAM_TYPE_INTEGER,"0",0}
};

class NativeScreen final : public CBaseShader {
    const char* name;
    ScreenEffect effect;
public:
    NativeScreen(const char* value,ScreenEffect kind):name(value),effect(kind) {}
    const char* GetName() const override { return name; }
    int GetFlags() const override { return SHADER_NOT_EDITABLE; }
    int GetParamCount() const override { return ParameterCount; }
    const ShaderParamInfo_t& GetParamInfo(int index) const override {
        return index<NUM_SHADER_MATERIAL_VARS ? CBaseShader::GetParamInfo(index) : screenParameters[index-NUM_SHADER_MATERIAL_VARS];
    }
    void OnInitShaderParams(IMaterialVar** params,const char*) override {
        params[FLAGS]->SetIntValue(params[FLAGS]->GetIntValue() | MATERIAL_VAR_NO_DEBUG_OVERRIDE | MATERIAL_VAR_NOFOG);
        auto integer=[&](int index,int value) { if(!params[index]->IsDefined())params[index]->SetIntValue(value); };
        auto number=[&](int index,float value) { if(!params[index]->IsDefined())params[index]->SetFloatValue(value); };
        for(int i:{BloomType,Kernel,EnableClearColor,AAEnable,DepthBlurEnable,Fade,DesaturateEnable,AllowVignette,VignetteEnable,Panorama})integer(i,0);
        integer(BloomEnable,1);integer(BloomTint,1);
        number(BloomAmount,1);number(BloomExponent,2.5f);number(BloomSaturation,1);
        for(int i:{ScreenBlur,DepthBlurStrength,DepthBlurFocalDistance,FadeToBlack,Desaturation})number(i,0);
        for(int i:{ClearColor,AAInternal1,AAInternal3,FXAAInternalC,FXAAInternalQ,FadeColor})
            if(!params[i]->IsDefined())params[i]->SetVecValue(0,0,0,0);
        if(!params[AAInternal2]->IsDefined())params[AAInternal2]->SetVecValue(0,0,1,1);
        if(!params[FrameBuffer]->IsDefined())params[FrameBuffer]->SetStringValue("_rt_FullFrameFB");
        if(!params[VignetteTexture]->IsDefined())params[VignetteTexture]->SetStringValue("dev/vignette");
    }
    void OnInitShaderInstance(IMaterialVar** params,IShaderInit*,const char*) override {
        if(params[BASETEXTURE]->IsDefined())LoadTexture(BASETEXTURE);
        if(effect==ScreenEffect::Post || effect==ScreenEffect::FloatCopy)LoadTexture(FrameBuffer);
        if(effect==ScreenEffect::Post)LoadTexture(VignetteTexture);
    }
    void OnDrawElements(IMaterialVar** params,IShaderShadow* shadow,IShaderDynamicAPI* api,
            VertexCompressionType_t,CBasePerMaterialContextData**) override {
        const bool post=effect==ScreenEffect::Post,blur=effect==ScreenEffect::BlurX || effect==ScreenEffect::BlurY;
        const int variant=post?3:blur?2:effect==ScreenEffect::BloomDownsample?1:effect==ScreenEffect::FloatCopy?4:0;
        if(shadow) {
            shadow->VertexShaderVertexFormat(VERTEX_POSITION,1,nullptr,0);
            shadow->EnableDepthTest(false);shadow->EnableDepthWrites(false);shadow->EnableCulling(false);
            shadow->EnableAlphaWrites(!post || params[Panorama]->GetIntValue()!=0);
            shadow->EnableSRGBWrite(effect==ScreenEffect::FloatCopy);
            for(int i=0;i<(post?3:1);++i)shadow->EnableTexture(Sampler_t(i),true);
            if(blur && (params[FLAGS]->GetIntValue() & MATERIAL_VAR_ADDITIVE)) {
                shadow->EnableBlending(true);shadow->BlendFunc(SHADER_BLEND_ONE,SHADER_BLEND_ONE);
            }
            shadow->SetVertexShader("native_screen_vs",0);
            shadow->SetPixelShader("native_screen_ps",variant);
        } else {
            auto bind=[&](Sampler_t sampler,int parameter,StandardTextureId_t fallback) {
                if(params[parameter]->IsTexture())BindTexture(sampler,TEXTURE_BINDFLAGS_NONE,parameter,-1);
                else api->BindStandardTexture(sampler,TEXTURE_BINDFLAGS_NONE,fallback);
            };
            bind(SHADER_SAMPLER0,effect==ScreenEffect::FloatCopy?FrameBuffer:BASETEXTURE,TEXTURE_BLACK);
            const int source=post || effect==ScreenEffect::FloatCopy?FrameBuffer:BASETEXTURE;
            ITexture* texture=params[source]->IsTexture()?params[source]->GetTextureValue():nullptr;
            const float dx=texture?1.f/std::max(1,texture->GetActualWidth()):1,dy=texture?1.f/std::max(1,texture->GetActualHeight()):1;
            float constants[24]={dx,dy,0,0};
            const float identity[4]={0,0,1,1};
            api->SetVertexShaderConstant(48,post?params[AAInternal2]->GetVecValue():identity,1);
            if(post) {
                bind(SHADER_SAMPLER1,FrameBuffer,TEXTURE_BLACK);
                bind(SHADER_SAMPLER2,VignetteTexture,TEXTURE_WHITE);
                constants[2]=params[AAEnable]->GetIntValue()!=0;
                constants[4]=params[BloomEnable]->GetIntValue()?params[BloomAmount]->GetFloatValue():0;
                constants[5]=params[ScreenBlur]->GetFloatValue();constants[6]=params[FadeToBlack]->GetFloatValue();
                constants[7]=float(params[Fade]->GetIntValue());params[FadeColor]->GetVecValue(constants+8,4);
                constants[12]=params[DesaturateEnable]->GetIntValue()?params[Desaturation]->GetFloatValue():0;
                constants[13]=params[AllowVignette]->GetIntValue() && params[VignetteEnable]->GetIntValue();
                constants[14]=params[DepthBlurEnable]->GetIntValue()?params[DepthBlurStrength]->GetFloatValue():0;
                constants[15]=params[DepthBlurFocalDistance]->GetFloatValue();
            } else if(blur) {
                constants[0]=effect==ScreenEffect::BlurX?dx:0;constants[1]=effect==ScreenEffect::BlurY?dy:0;
                constants[2]=effect==ScreenEffect::BlurY?params[BloomAmount]->GetFloatValue():1;
                constants[4]=float(std::clamp(params[Kernel]->GetIntValue(),0,4));
                constants[5]=effect==ScreenEffect::BlurY && params[EnableClearColor]->GetIntValue();
                params[ClearColor]->GetVecValue(constants+8,3);
            } else if(effect==ScreenEffect::BloomDownsample) {
                constants[4]=params[BloomExponent]->GetFloatValue();constants[5]=params[BloomSaturation]->GetFloatValue();
                constants[6]=float(params[BloomType]->GetIntValue());
                const bool tint=params[BloomTint]->GetIntValue()!=0;
                constants[8]=tint?.3f:1;constants[9]=tint?.59f:1;constants[10]=tint?.11f:1;constants[11]=tint?2.2f:1;
            }
            api->SetPixelShaderConstant(0,constants,6);
            api->SetVertexShaderIndex(0);api->SetPixelShaderIndex(0);
        }
        Draw();
    }
};
NativeScreen downsample("Downsample",ScreenEffect::Downsample);
NativeScreen bloomDownsample("Downsample_nohdr",ScreenEffect::BloomDownsample);
NativeScreen blurX("BlurFilterX",ScreenEffect::BlurX),blurY("BlurFilterY",ScreenEffect::BlurY);
NativeScreen post("Engine_Post",ScreenEffect::Post),postDX9("Engine_Post_dx9",ScreenEffect::Post);
NativeScreen floatCopy("floattoscreen",ScreenEffect::FloatCopy);
} // namespace
