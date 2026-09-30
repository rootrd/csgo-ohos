#include "shaderlib/cshader.h"
#include "mathlib/vmatrix.h"

#include <algorithm>
#include <cstring>

namespace {
enum class Kind { Unlit, Lightmapped, VertexLit, Depth, Screen, Modulate, Decal, Transition, Wireframe, Occlusion };
enum Parameters {
    AlphaReference=NUM_SHADER_MATERIAL_VARS, BaseTexture2, HDRColorScale,
    LinearWrite, GammaRead, ColorDepth, WriteZ, WriteAlpha, ParameterCount
};
const ShaderParamInfo_t parameters[] = {
    {"$alphatestreference","Alpha cutoff",SHADER_PARAM_TYPE_FLOAT,"0.7",0},
    {"$basetexture2","Second world texture",SHADER_PARAM_TYPE_TEXTURE,"",0},
    {"$hdrcolorscale","Color scale",SHADER_PARAM_TYPE_FLOAT,"1",0},
    {"$linearwrite","Write linear values",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$gammacolorread","Read encoded color values",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$color_depth","Write depth into color",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$writez","Clear depth",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$writealpha","Write destination alpha",SHADER_PARAM_TYPE_INTEGER,"0",0}
};

class NativeShader final : public CBaseShader {
    const char* name;
    Kind kind;
public:
    NativeShader(const char* shaderName,Kind type):name(shaderName),kind(type) {}
    const char* GetName() const override { return name; }
    int GetFlags() const override { return 0; }
    int GetParamCount() const override { return ParameterCount; }
    const ShaderParamInfo_t& GetParamInfo(int index) const override {
        return index<NUM_SHADER_MATERIAL_VARS ? CBaseShader::GetParamInfo(index) : parameters[index-NUM_SHADER_MATERIAL_VARS];
    }
    void OnInitShaderParams(IMaterialVar** params,const char*) override {
        if(kind==Kind::Wireframe)params[FLAGS]->SetIntValue(params[FLAGS]->GetIntValue() |
            MATERIAL_VAR_WIREFRAME | MATERIAL_VAR_NO_DEBUG_OVERRIDE | MATERIAL_VAR_NOFOG);
        int flags=params[FLAGS2]->GetIntValue();
        flags &= ~MATERIAL_VAR2_LIGHTING_MASK;
        if(kind==Kind::Lightmapped || kind==Kind::Transition)flags|=MATERIAL_VAR2_LIGHTING_LIGHTMAP;
        if(kind==Kind::VertexLit)flags|=MATERIAL_VAR2_LIGHTING_VERTEX_LIT | MATERIAL_VAR2_SUPPORTS_HW_SKINNING;
        if(params[FLAGS]->GetIntValue() & MATERIAL_VAR_MODEL)flags|=MATERIAL_VAR2_SUPPORTS_HW_SKINNING;
        params[FLAGS2]->SetIntValue(flags);
        if(!params[FRAME]->IsDefined())params[FRAME]->SetIntValue(0);
        if(!params[AlphaReference]->IsDefined())params[AlphaReference]->SetFloatValue(0.7f);
        if(!params[HDRColorScale]->IsDefined())params[HDRColorScale]->SetFloatValue(1);
    }
    void OnInitShaderInstance(IMaterialVar** params,IShaderInit*,const char*) override {
        if(params[BASETEXTURE]->IsDefined())LoadTexture(BASETEXTURE);
        if(kind==Kind::Transition && params[BaseTexture2]->IsDefined())LoadTexture(BaseTexture2);
    }
    void OnDrawElements(IMaterialVar** params,IShaderShadow* shadow,IShaderDynamicAPI* api,
            VertexCompressionType_t,CBasePerMaterialContextData**) override {
        const int flags=params[FLAGS]->GetIntValue();
        const bool model=kind==Kind::VertexLit || (flags & MATERIAL_VAR_MODEL);
        const bool lightmap=kind==Kind::Lightmapped || kind==Kind::Transition;
        const bool hdr=g_pHardwareConfig->GetHDRType()!=HDR_TYPE_NONE;
        const bool vertexColor=(flags & (MATERIAL_VAR_VERTEXCOLOR | MATERIAL_VAR_VERTEXALPHA)) || kind==Kind::Transition;
        const bool srgbRead=!params[GammaRead]->GetIntValue() && kind!=Kind::Screen && kind!=Kind::Modulate && kind!=Kind::Decal;
        const bool srgbWrite=!params[LinearWrite]->GetIntValue() && kind!=Kind::Screen && kind!=Kind::Modulate && kind!=Kind::Decal;
        const bool alphaTest=(flags & MATERIAL_VAR_ALPHATEST) || kind==Kind::Decal;
        const char* vs=model ? "native_model_vs" : lightmap ? "native_lightmap_vs" : "native_unlit_vs";
        const char* ps=kind==Kind::VertexLit ? "native_model_ps" : kind==Kind::Transition ? "native_transition_ps" :
            lightmap ? "native_lightmap_ps" : kind==Kind::Depth ? "native_depth_ps" : kind==Kind::Decal ? "native_decal_ps" : "native_unlit_ps";
        if(shadow) {
            unsigned format=VERTEX_POSITION | (model?VERTEX_NORMAL:0) | (vertexColor?VERTEX_COLOR:0);
            int uv[2]={2,2};
            shadow->VertexShaderVertexFormat(format,lightmap?2:1,uv,0);
            shadow->EnableTexture(SHADER_SAMPLER0,true);
            shadow->EnableSRGBRead(SHADER_SAMPLER0,srgbRead);
            if(lightmap) {shadow->EnableTexture(SHADER_SAMPLER1,true);shadow->EnableSRGBRead(SHADER_SAMPLER1,!hdr);}
            if(kind==Kind::Transition) {shadow->EnableTexture(SHADER_SAMPLER2,true);shadow->EnableSRGBRead(SHADER_SAMPLER2,true);}
            shadow->EnableSRGBWrite(srgbWrite);
            shadow->EnableCulling(!(flags & MATERIAL_VAR_NOCULL));
            if(flags & MATERIAL_VAR_WIREFRAME)shadow->PolyMode(SHADER_POLYMODEFACE_FRONT_AND_BACK,SHADER_POLYMODE_LINE);
            shadow->EnableDepthTest(!(flags & MATERIAL_VAR_IGNOREZ));
            shadow->EnableAlphaTest(alphaTest);
            if(alphaTest)shadow->AlphaFunc(kind==Kind::Decal?SHADER_ALPHAFUNC_GREATER:SHADER_ALPHAFUNC_GEQUAL,
                kind==Kind::Decal?0:params[AlphaReference]->GetFloatValue());
            if(kind==Kind::Depth) {
                shadow->EnableColorWrites(params[ColorDepth]->GetIntValue()!=0);
                shadow->EnableAlphaWrites(false);shadow->EnableDepthWrites(true);
            } else if(kind==Kind::Modulate || kind==Kind::Decal) {
                shadow->EnableBlending(true);shadow->BlendFunc(SHADER_BLEND_DST_COLOR,kind==Kind::Decal?SHADER_BLEND_SRC_COLOR:SHADER_BLEND_ZERO);
                shadow->EnableDepthWrites(false);
                if(kind==Kind::Decal)shadow->EnablePolyOffset(SHADER_POLYOFFSET_DECAL);
            } else SetDefaultBlendingShadowState(BASETEXTURE,true);
            if(flags & MATERIAL_VAR_IGNOREZ)shadow->EnableDepthWrites(false);
            if(kind==Kind::Screen) {shadow->EnableDepthTest(false);shadow->EnableDepthWrites(params[WriteZ]->GetIntValue()!=0);}
            shadow->EnableAlphaWrites(kind!=Kind::Decal && (kind!=Kind::Depth || params[WriteAlpha]->GetIntValue()!=0));
            if(kind==Kind::Occlusion) {
                shadow->EnableColorWrites(false);shadow->EnableAlphaWrites(false);shadow->EnableDepthWrites(false);
            }
            shadow->SetVertexShader(vs,vertexColor?1:0);
            shadow->SetPixelShader(ps,0);
        } else {
            if(kind!=Kind::Wireframe && params[BASETEXTURE]->IsTexture())BindTexture(SHADER_SAMPLER0,SRGBReadMask(srgbRead),BASETEXTURE,FRAME);
            else api->BindStandardTexture(SHADER_SAMPLER0,SRGBReadMask(srgbRead),TEXTURE_WHITE);
            if(lightmap)api->BindStandardTexture(SHADER_SAMPLER1,SRGBReadMask(!hdr),TEXTURE_LIGHTMAP);
            if(kind==Kind::Transition) {
                if(params[BaseTexture2]->IsTexture())BindTexture(SHADER_SAMPLER2,TEXTURE_BINDFLAGS_SRGBREAD,BaseTexture2);
                else api->BindStandardTexture(SHADER_SAMPLER2,TEXTURE_BINDFLAGS_SRGBREAD,TEXTURE_WHITE);
            }
            float color[4]={1,1,1,params[ALPHA]->GetFloatValue()};
            params[COLOR]->GetVecValue(color,3);ApplyColor2Factor(color);
            for(int i=0;i<3;++i)color[i]*=params[HDRColorScale]->GetFloatValue();
            // Source's integer HDR lightmaps represent linear light / 16.
            // Exposure is applied before writing the sRGB scene attachment.
            const float lightScale=(lightmap?g_pHardwareConfig->GetLightMapScaleFactor():1.f)*
                (hdr && (lightmap || kind==Kind::VertexLit)?api->GetToneMappingScaleLinear().x:1.f);
            for(int i=0;i<3;++i)color[i]*=lightScale;
            api->SetPixelShaderConstant(0,color,1);
            const auto& transform=params[BASETEXTURETRANSFORM]->GetMatrixValue();
            api->SetVertexShaderConstant(VERTEX_SHADER_SHADER_SPECIFIC_CONST_0,&transform.m[0][0],2);
            api->SetVertexShaderViewProj();api->SetVertexShaderModelViewProjAndModelView();
            LightState_t lightState {};
            if(model)api->GetDX9LightState(&lightState);
            api->SetVertexShaderIndex((model && api->GetCurrentNumBones()>0 ? 1 : 0) | (model && lightState.m_bStaticLight ? 2 : 0));
            api->SetPixelShaderIndex(0);
        }
        Draw();
    }
};
NativeShader unlit("UnlitGeneric",Kind::Unlit);
NativeShader lightmapped("LightmappedGeneric",Kind::Lightmapped);
NativeShader vertexlit("VertexLitGeneric",Kind::VertexLit);
NativeShader depth("DepthWrite",Kind::Depth);
NativeShader transition("WorldVertexTransition",Kind::Transition);
NativeShader wireframe("Wireframe",Kind::Wireframe);
// CMaterial's error and debug paths request this exact legacy name even when
// the material library and shader API are native Vulkan.
NativeShader wireframeDX9("Wireframe_DX9",Kind::Wireframe);
NativeShader sky("Sky",Kind::Unlit);
NativeShader skyHDR("Sky_HDR_DX9",Kind::Unlit);
NativeShader modulate("Modulate",Kind::Modulate);
NativeShader decal("DecalModulate",Kind::Decal);
NativeShader screenspace("ScreenspaceGeneral",Kind::Screen);
NativeShader clear("ClearBuffer",Kind::Screen);
NativeShader debugTexture("DebugTextureView",Kind::Unlit);
NativeShader occlusion("Occlusion",Kind::Occlusion);
} // namespace

// The stage-2 model tier provides Character's diffuse texture, ambient/local
// lighting and skinning. Its layered paint, cloth and specular paths are later
// material coverage; make the reduced shader choice explicit to the engine.
DEFINE_FALLBACK_SHADER(Character,VertexLitGeneric)
