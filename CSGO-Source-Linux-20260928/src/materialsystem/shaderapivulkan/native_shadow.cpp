#include "shaderlib/cshader.h"
#include "mathlib/vmatrix.h"

#include <algorithm>

namespace {
// PC projected shadows use the original alpha atlas, five-tap filter and
// multiplicative blend. CSM and flashlight depth comparisons are separate paths.
class NativeShadow final : public CBaseShader {
    enum { MaxFalloff=NUM_SHADER_MATERIAL_VARS, Deferred, ZFail, Blobby, DepthTexture, ParameterCount };
public:
    const char* GetName() const override { return "Shadow"; }
    int GetFlags() const override { return SHADER_NOT_EDITABLE; }
    int GetParamCount() const override { return ParameterCount; }
    const ShaderParamInfo_t& GetParamInfo(int index) const override {
        static const ShaderParamInfo_t parameters[] = {
            {"$maxfalloffamount","Maximum distance fade",SHADER_PARAM_TYPE_FLOAT,"240",0},
            {"$deferredshadows","Console deferred shadow path",SHADER_PARAM_TYPE_INTEGER,"0",0},
            {"$zfailenable","Deferred shadow depth test",SHADER_PARAM_TYPE_INTEGER,"0",0},
            {"$blobbyshadows","Simple shadow texture",SHADER_PARAM_TYPE_INTEGER,"0",0},
            {"$depthtexture","Deferred shadow depth",SHADER_PARAM_TYPE_TEXTURE,"",0}
        };
        return index<NUM_SHADER_MATERIAL_VARS?CBaseShader::GetParamInfo(index):parameters[index-NUM_SHADER_MATERIAL_VARS];
    }
    const char* GetFallbackShader(IMaterialVar** params) const override {
        return params[Deferred]->GetIntValue()?"Wireframe_DX9":nullptr;
    }
    void OnInitShaderParams(IMaterialVar** params,const char* material) override {
        if(!params[MaxFalloff]->IsDefined())params[MaxFalloff]->SetFloatValue(240);
        for(int i=Deferred;i<=Blobby;++i)if(!params[i]->IsDefined())params[i]->SetIntValue(0);
        if(params[Deferred]->GetIntValue())Warning("Vulkan material %s requests unported console deferred shadows\n",material);
    }
    void OnInitShaderInstance(IMaterialVar** params,IShaderInit*,const char*) override {
        if(params[BASETEXTURE]->IsDefined())LoadTexture(BASETEXTURE,TEXTUREFLAGS_SRGB);
    }
    void OnDrawElements(IMaterialVar** params,IShaderShadow* shadow,IShaderDynamicAPI* api,
            VertexCompressionType_t,CBasePerMaterialContextData**) override {
        if(shadow) {
            int dimensions[2]={3,3};
            shadow->VertexShaderVertexFormat(VERTEX_POSITION|VERTEX_COLOR,2,dimensions,0);
            shadow->EnableTexture(SHADER_SAMPLER0,true);shadow->EnableSRGBRead(SHADER_SAMPLER0,true);
            shadow->EnableSRGBWrite(true);EnableAlphaBlending(SHADER_BLEND_ZERO,SHADER_BLEND_SRC_COLOR);
            shadow->EnableAlphaWrites(false);FogToWhite();
            shadow->SetVertexShader("native_shadow_vs",0);
            shadow->SetPixelShader("native_shadow_ps",params[Blobby]->GetIntValue()?1:0);
        } else {
            BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,BASETEXTURE,FRAME);
            const auto& transform=params[BASETEXTURETRANSFORM]->GetMatrixValue();
            api->SetVertexShaderConstant(VERTEX_SHADER_SHADER_SPECIFIC_CONST_0,&transform.m[0][0],2);
            auto* texture=params[BASETEXTURE]->GetTextureValue();
            const float jitter[4]={1.f/(texture?std::max(1,texture->GetActualWidth()):16),
                1.f/(texture?std::max(1,texture->GetActualHeight()):16),0,0};
            api->SetVertexShaderConstant(VERTEX_SHADER_SHADER_SPECIFIC_CONST_2,jitter);
            float color[4]={1,1,1,1};params[COLOR]->GetVecValue(color,3);
            for(int i=0;i<3;++i)if(color[i]<=1)color[i]=GammaToLinear(color[i]);
            api->SetPixelShaderConstant(1,color);
            float eye[4]={0,0,0,1};api->GetWorldSpaceCameraPosition(eye);api->SetPixelShaderConstant(2,eye);
            api->SetPixelShaderFogParams(3);
            const float falloff[4]={params[MaxFalloff]->GetFloatValue()/255.f,0,0,0};api->SetPixelShaderConstant(4,falloff);
            api->SetVertexShaderViewProj();api->SetVertexShaderModelViewProjAndModelView();
            api->SetVertexShaderIndex(0);api->SetPixelShaderIndex(api->GetPixelFogCombo());
        }
        Draw();
    }
} projectedShadow;

class NativeShadowBuild final : public CBaseShader {
    enum { TranslucentMaterial=NUM_SHADER_MATERIAL_VARS, ParameterCount };
    const char* name;
public:
    explicit NativeShadowBuild(const char* value):name(value) {}
    const char* GetName() const override { return name; }
    int GetFlags() const override { return SHADER_NOT_EDITABLE; }
    int GetParamCount() const override { return ParameterCount; }
    const ShaderParamInfo_t& GetParamInfo(int index) const override {
        static const ShaderParamInfo_t parameter={"$translucent_material","Material supplying shadow alpha",SHADER_PARAM_TYPE_MATERIAL,"",0};
        return index<NUM_SHADER_MATERIAL_VARS?CBaseShader::GetParamInfo(index):parameter;
    }
    void OnInitShaderParams(IMaterialVar** params,const char*) override {
        params[FLAGS2]->SetIntValue(params[FLAGS2]->GetIntValue()|MATERIAL_VAR2_SUPPORTS_HW_SKINNING);
        params[FLAGS]->SetIntValue(params[FLAGS]->GetIntValue()|MATERIAL_VAR_NO_DEBUG_OVERRIDE);
    }
    void OnInitShaderInstance(IMaterialVar** params,IShaderInit*,const char*) override {
        if(params[BASETEXTURE]->IsDefined())LoadTexture(BASETEXTURE,TEXTUREFLAGS_SRGB);
    }
    void OnDrawElements(IMaterialVar** params,IShaderShadow* shadow,IShaderDynamicAPI* api,
            VertexCompressionType_t,CBasePerMaterialContextData**) override {
        IMaterialVar** original=nullptr;
        ITexture* texture=nullptr;
        if(params[TranslucentMaterial]->IsDefined()) {
            if(auto* material=params[TranslucentMaterial]->GetMaterialValue()) {
                original=material->GetShaderParams();
                if(original[BASETEXTURE]->IsTexture())texture=original[BASETEXTURE]->GetTextureValue();
            }
        }
        const bool srgb=texture || g_pHardwareConfig->GetHDRType()==HDR_TYPE_NONE;
        if(shadow) {
            shadow->VertexShaderVertexFormat(VERTEX_POSITION,1,nullptr,0);
            shadow->EnableTexture(SHADER_SAMPLER0,true);shadow->EnableSRGBRead(SHADER_SAMPLER0,srgb);
            shadow->EnableSRGBWrite(true);EnableAlphaBlending(SHADER_BLEND_ONE,SHADER_BLEND_ONE);
            shadow->EnableAlphaWrites(true);shadow->EnableDepthTest(false);shadow->EnableDepthWrites(false);
            shadow->SetVertexShader("native_shadowbuild_vs",0);shadow->SetPixelShader("native_shadowbuild_ps",0);
        } else {
            float transform[8]={1,0,0,0,0,1,0,0};
            if(texture) {
                BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,texture,original[FRAME]->GetIntValue());
                const auto& matrix=original[BASETEXTURETRANSFORM]->GetMatrixValue();
                for(int row=0;row<2;++row)for(int column=0;column<4;++column)transform[row*4+column]=matrix[row][column];
            } else api->BindStandardTexture(SHADER_SAMPLER0,SRGBReadMask(srgb),TEXTURE_LIGHTMAP_FULLBRIGHT);
            api->SetVertexShaderConstant(VERTEX_SHADER_SHADER_SPECIFIC_CONST_0,transform,2);
            const float modulation[4]={1,1,1,params[ALPHA]->GetFloatValue()};api->SetPixelShaderConstant(0,modulation);
            api->SetVertexShaderViewProj();api->SetVertexShaderModelViewProjAndModelView();
            api->SetVertexShaderIndex(api->GetCurrentNumBones()>0?1:0);api->SetPixelShaderIndex(0);
        }
        Draw();
    }
} shadowBuild("ShadowBuild"),shadowBuildDX9("ShadowBuild_DX9");
} // namespace
