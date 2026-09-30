#include "shaderlib/cshader.h"
#include "mathlib/vmatrix.h"

namespace {
class NativeTwoTexture final : public CBaseShader {
    enum { Texture2=NUM_SHADER_MATERIAL_VARS, Frame2, Transform2, HDRScale, AlphaReference,
        Crosshair, CrosshairTint, CrosshairAdapt, BeachFoam, CloakPass, ParameterCount };
    const char* name;
public:
    explicit NativeTwoTexture(const char* value):name(value) {}
    const char* GetName() const override { return name; }
    int GetFlags() const override { return 0; }
    int GetParamCount() const override { return ParameterCount; }
    const ShaderParamInfo_t& GetParamInfo(int index) const override {
        static const ShaderParamInfo_t parameters[] = {
            {"$texture2","Second texture",SHADER_PARAM_TYPE_TEXTURE,"",0},
            {"$frame2","Second texture frame",SHADER_PARAM_TYPE_INTEGER,"0",0},
            {"$texture2transform","Second UV transform",SHADER_PARAM_TYPE_MATRIX,"center .5 .5 scale 1 1 rotate 0 translate 0 0",0},
            {"$hdrcolorscale","HDR color scale",SHADER_PARAM_TYPE_FLOAT,"1",0},
            {"$alphatestreference","Alpha cutoff",SHADER_PARAM_TYPE_FLOAT,"0.7",0},
            {"$crosshairmode","Adaptive crosshair",SHADER_PARAM_TYPE_INTEGER,"0",0},
            {"$crosshaircolortint","Crosshair tint",SHADER_PARAM_TYPE_COLOR,"[1 1 1]",0},
            {"$crosshaircoloradapt","Crosshair adaptation",SHADER_PARAM_TYPE_FLOAT,"0.4",0},
            {"$beachfoam","Animated beach foam",SHADER_PARAM_TYPE_INTEGER,"0",0},
            {"$cloakpassenabled","Refraction cloak pass",SHADER_PARAM_TYPE_INTEGER,"0",0}
        };
        return index<NUM_SHADER_MATERIAL_VARS?CBaseShader::GetParamInfo(index):parameters[index-NUM_SHADER_MATERIAL_VARS];
    }
    const char* GetFallbackShader(IMaterialVar** params) const override {
        if(params[CloakPass]->GetIntValue()) {
            Warning("Vulkan UnlitTwoTexture cloak refraction is not implemented; using Wireframe_DX9\n");
            return "Wireframe_DX9";
        }
        return nullptr;
    }
    void OnInitShaderParams(IMaterialVar** params,const char*) override {
        int flags=params[FLAGS2]->GetIntValue();
        flags=(flags & ~MATERIAL_VAR2_LIGHTING_MASK) | MATERIAL_VAR2_SUPPORTS_HW_SKINNING;
        if(params[Crosshair]->GetIntValue()>0)flags|=MATERIAL_VAR2_NEEDS_POWER_OF_TWO_FRAME_BUFFER_TEXTURE;
        params[FLAGS2]->SetIntValue(flags);
        if(!params[HDRScale]->IsDefined())params[HDRScale]->SetFloatValue(1);
        if(!params[AlphaReference]->IsDefined())params[AlphaReference]->SetFloatValue(.7f);
        if(!params[CrosshairAdapt]->IsDefined())params[CrosshairAdapt]->SetFloatValue(.4f);
    }
    void OnInitShaderInstance(IMaterialVar** params,IShaderInit*,const char*) override {
        if(params[BASETEXTURE]->IsDefined())LoadTexture(BASETEXTURE,TEXTUREFLAGS_SRGB);
        if(params[Texture2]->IsDefined())LoadTexture(Texture2,TEXTUREFLAGS_SRGB);
    }
    void OnDrawElements(IMaterialVar** params,IShaderShadow* shadow,IShaderDynamicAPI* api,
            VertexCompressionType_t,CBasePerMaterialContextData**) override {
        const int flags=params[FLAGS]->GetIntValue();
        const bool alphaTest=flags & MATERIAL_VAR_ALPHATEST;
        const bool translucent=IsAlphaModulating() || (flags & MATERIAL_VAR_TRANSLUCENT) ||
            TextureIsTranslucent(BASETEXTURE,true) || TextureIsTranslucent(Texture2,true);
        const bool additive=flags & MATERIAL_VAR_ADDITIVE;
        const bool color=flags & (MATERIAL_VAR_VERTEXCOLOR | MATERIAL_VAR_VERTEXALPHA);
        const int effect=params[BeachFoam]->GetIntValue()?2:params[Crosshair]->GetIntValue()>0?1:0;
        if(shadow) {
            shadow->VertexShaderVertexFormat(VERTEX_POSITION | (color?VERTEX_COLOR:0),1,nullptr,0);
            shadow->EnableTexture(SHADER_SAMPLER0,true);shadow->EnableSRGBRead(SHADER_SAMPLER0,true);
            shadow->EnableTexture(SHADER_SAMPLER1,true);shadow->EnableSRGBRead(SHADER_SAMPLER1,true);
            shadow->EnableSRGBWrite(true);shadow->EnableCulling(!(flags & MATERIAL_VAR_NOCULL));
            shadow->EnableDepthTest(!(flags & MATERIAL_VAR_IGNOREZ));
            shadow->EnableDepthWrites(!translucent && !additive && !(flags & MATERIAL_VAR_IGNOREZ));
            shadow->EnableBlending(translucent || additive);
            shadow->BlendFunc(translucent?SHADER_BLEND_SRC_ALPHA:SHADER_BLEND_ONE,additive?SHADER_BLEND_ONE:SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
            shadow->EnableAlphaTest(alphaTest);
            if(alphaTest)shadow->AlphaFunc(SHADER_ALPHAFUNC_GEQUAL,params[AlphaReference]->GetFloatValue());
            shadow->EnableAlphaWrites(!translucent && !additive && !alphaTest);
            shadow->SetVertexShader("native_twotexture_vs",color?1:0);
            shadow->SetPixelShader("native_twotexture_ps",effect*2+int(translucent || alphaTest));
        } else {
            if(params[BASETEXTURE]->IsTexture())BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,BASETEXTURE,FRAME);
            else api->BindStandardTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,TEXTURE_WHITE);
            if(params[Texture2]->IsTexture())BindTexture(SHADER_SAMPLER1,TEXTURE_BINDFLAGS_SRGBREAD,Texture2,Frame2);
            else api->BindStandardTexture(SHADER_SAMPLER1,TEXTURE_BINDFLAGS_SRGBREAD,TEXTURE_WHITE);
            api->SetVertexShaderConstant(48,params[BASETEXTURETRANSFORM]->GetMatrixValue().Base(),2);
            api->SetVertexShaderConstant(50,params[Transform2]->GetMatrixValue().Base(),2);
            api->SetVertexShaderViewProj();api->SetVertexShaderModelViewProjAndModelView();
            float tint[4]={1,1,1,params[ALPHA]->GetFloatValue()};params[COLOR]->GetVecValue(tint,3);ApplyColor2Factor(tint);
            const float exposure=effect!=1 && g_pHardwareConfig->GetHDRType()!=HDR_TYPE_NONE?api->GetToneMappingScaleLinear().x:1;
            for(int i=0;i<3;++i)tint[i]*=params[HDRScale]->GetFloatValue()*exposure;
            api->SetPixelShaderConstant(0,tint,1);
            float special[4]={float(api->CurrentTime()),0,0,0};
            if(effect==1) {params[CrosshairTint]->GetVecValue(special,3);special[3]=params[CrosshairAdapt]->GetFloatValue();}
            api->SetPixelShaderConstant(1,special,1);
            api->SetVertexShaderIndex(api->GetCurrentNumBones()>0?1:0);api->SetPixelShaderIndex(0);
        }
        Draw();
    }
} twoTexture("UnlitTwoTexture"),twoTextureDX9("UnlitTwoTexture_DX9");
} // namespace
