#include "shaderlib/cshader.h"
#include "convar.h"

#include <algorithm>

namespace {
ConVar ropeMinPixelDiameter("rope_min_pixel_diameter","2.0",FCVAR_CHEAT);

class NativeRope final : public CBaseShader {
    enum { BumpMap=NUM_SHADER_MATERIAL_VARS, Tiling, ShadowDepth, AlphaReference, ParameterCount };
public:
    const char* GetName() const override { return "SplineRope"; }
    int GetFlags() const override { return 0; }
    int GetParamCount() const override { return ParameterCount; }
    const ShaderParamInfo_t& GetParamInfo(int index) const override {
        static const ShaderParamInfo_t parameters[] = {
            {"$bumpmap","Cable normal map",SHADER_PARAM_TYPE_TEXTURE,"cable/cablenormalmap",0},
            {"$tiling","Cable texture tiling",SHADER_PARAM_TYPE_FLOAT,"1",0},
            {"$shadowdepth","Draw cable depth",SHADER_PARAM_TYPE_INTEGER,"0",0},
            {"$alphatestreference","Cable alpha cutoff; 1 disables",SHADER_PARAM_TYPE_FLOAT,"1",0}
        };
        return index<NUM_SHADER_MATERIAL_VARS ? CBaseShader::GetParamInfo(index) : parameters[index-NUM_SHADER_MATERIAL_VARS];
    }
    void OnInitShaderParams(IMaterialVar** params,const char*) override {
        params[FLAGS2]->SetIntValue(params[FLAGS2]->GetIntValue() |
            MATERIAL_VAR2_IS_SPRITECARD | MATERIAL_VAR2_LIGHTING_VERTEX_LIT);
        if(!params[BumpMap]->IsDefined())params[BumpMap]->SetStringValue("cable/cablenormalmap");
        if(!params[Tiling]->IsDefined())params[Tiling]->SetFloatValue(1);
        if(!params[ShadowDepth]->IsDefined())params[ShadowDepth]->SetIntValue(0);
        if(!params[AlphaReference]->IsDefined())params[AlphaReference]->SetFloatValue(1);
    }
    void OnInitShaderInstance(IMaterialVar** params,IShaderInit*,const char*) override {
        if(params[BASETEXTURE]->IsDefined())LoadTexture(BASETEXTURE,TEXTUREFLAGS_SRGB);
        if(params[BumpMap]->IsDefined())LoadTexture(BumpMap);
    }
    void OnDrawElements(IMaterialVar** params,IShaderShadow* shadow,IShaderDynamicAPI* api,
            VertexCompressionType_t,CBasePerMaterialContextData**) override {
        const bool depth=params[ShadowDepth]->GetIntValue()!=0;
        const float cutoff=params[AlphaReference]->GetFloatValue();
        if(shadow) {
            // CRopeManager writes (t, v, side) into POSITION and the four
            // Catmull-Rom control points with diameters into TEXCOORD0..3.
            int dimensions[4]={4,4,4,4};
            shadow->VertexShaderVertexFormat(VERTEX_POSITION | VERTEX_COLOR,4,dimensions,0);
            shadow->EnableCulling(false);
            shadow->EnableColorWrites(!depth);shadow->EnableAlphaWrites(!depth);
            if(depth)shadow->EnablePolyOffset(SHADER_POLYOFFSET_SHADOW_BIAS);
            else {
                shadow->EnableTexture(SHADER_SAMPLER0,true);
                shadow->EnableSRGBRead(SHADER_SAMPLER0,true);
                shadow->EnableTexture(SHADER_SAMPLER1,true);
                shadow->EnableSRGBWrite(true);
                shadow->EnableAlphaTest(cutoff<1);
                if(cutoff<1)shadow->AlphaFunc(SHADER_ALPHAFUNC_GEQUAL,std::clamp(cutoff,0.f,1.f));
                SetDefaultBlendingShadowState(BASETEXTURE,true);
            }
            shadow->SetVertexShader("native_rope_vs",0);
            shadow->SetPixelShader("native_rope_ps",depth?1:0);
        } else {
            api->SetVertexShaderViewProj();
            float camera[4]={0,0,0,std::max(0.f,ropeMinPixelDiameter.GetFloat())};
            api->GetWorldSpaceCameraPosition(camera);
            api->SetVertexShaderConstant(VERTEX_SHADER_SHADER_SPECIFIC_CONST_0,camera,1);
            int x,y,width,height;api->GetCurrentViewport(x,y,width,height);
            const float viewport[4]={float(width),float(height),params[Tiling]->GetFloatValue(),0};
            api->SetVertexShaderConstant(VERTEX_SHADER_SHADER_SPECIFIC_CONST_1,viewport,1);
            if(!depth) {
                if(params[BASETEXTURE]->IsTexture())BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,BASETEXTURE,FRAME);
                else api->BindStandardTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,TEXTURE_WHITE);
                if(params[BumpMap]->IsTexture())BindTexture(SHADER_SAMPLER1,TEXTURE_BINDFLAGS_NONE,BumpMap);
                else api->BindStandardTexture(SHADER_SAMPLER1,TEXTURE_BINDFLAGS_NONE,TEXTURE_NORMALMAP_FLAT);
                float color[4]={1,1,1,params[ALPHA]->GetFloatValue()};
                params[COLOR]->GetVecValue(color,3);ApplyColor2Factor(color);
                api->SetPixelShaderConstant(0,color,1);
            }
            api->SetVertexShaderIndex(0);api->SetPixelShaderIndex(0);
        }
        Draw();
    }
} rope;
} // namespace
