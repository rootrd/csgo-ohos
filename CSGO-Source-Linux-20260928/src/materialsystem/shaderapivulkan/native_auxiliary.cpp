#include "shaderlib/cshader.h"

#include <cstring>

namespace {
class NativeClear final : public CBaseShader {
    enum { ClearRGB=NUM_SHADER_MATERIAL_VARS,ClearAlpha,ClearDepth,ParameterCount };
    const char* name;
public:
    explicit NativeClear(const char* value):name(value) {}
    const char* GetName() const override { return name; }
    int GetFlags() const override { return SHADER_NOT_EDITABLE; }
    int GetParamCount() const override { return ParameterCount; }
    const ShaderParamInfo_t& GetParamInfo(int index) const override {
        static const ShaderParamInfo_t parameters[] = {
            {"$clearcolor","Clear RGB",SHADER_PARAM_TYPE_INTEGER,"1",0},
            {"$clearalpha","Clear alpha",SHADER_PARAM_TYPE_INTEGER,"-1",0},
            {"$cleardepth","Clear depth",SHADER_PARAM_TYPE_INTEGER,"1",0}
        };
        return index<NUM_SHADER_MATERIAL_VARS?CBaseShader::GetParamInfo(index):parameters[index-NUM_SHADER_MATERIAL_VARS];
    }
    void OnInitShaderParams(IMaterialVar** params,const char*) override {
        if(!params[ClearRGB]->IsDefined())params[ClearRGB]->SetIntValue(1);
        if(!params[ClearAlpha]->IsDefined())params[ClearAlpha]->SetIntValue(-1);
        if(!params[ClearDepth]->IsDefined())params[ClearDepth]->SetIntValue(1);
    }
    void OnInitShaderInstance(IMaterialVar**,IShaderInit*,const char*) override {}
    void OnDrawElements(IMaterialVar** params,IShaderShadow* shadow,IShaderDynamicAPI* api,
            VertexCompressionType_t,CBasePerMaterialContextData**) override {
        if(shadow) {
            shadow->VertexShaderVertexFormat(VERTEX_POSITION|VERTEX_COLOR,1,nullptr,0);
            shadow->EnableCulling(false);shadow->DepthFunc(SHADER_DEPTHFUNC_ALWAYS);
            shadow->EnableDepthWrites(params[ClearDepth]->GetIntValue()!=0);
            shadow->EnableColorWrites(params[ClearRGB]->GetIntValue()!=0);
            shadow->EnableAlphaWrites((params[ClearAlpha]->GetIntValue()<0?params[ClearRGB]:params[ClearAlpha])->GetIntValue()!=0);
            shadow->SetVertexShader("native_clear_vs",0);shadow->SetPixelShader("native_clear_ps",0);
        } else { api->SetVertexShaderIndex(0);api->SetPixelShaderIndex(0); }
        Draw();
    }
} clear("BufferClearObeyStencil"),clearDX9("BufferClearObeyStencil_DX9");

enum { PixelShader=NUM_SHADER_MATERIAL_VARS,LinearRead,LinearWrite,WriteAlpha,WriteDepth,DepthTest,
    Cull,DisableColor,AlphaBlend,AlphaBlend2,Premultiplied,MultiplyColor,CopyAlpha,VertexTransform,
    FirstConstant,ParameterCount=FirstConstant+24 };
#define CONSTANT_INFO(i,c) {"$c" #i "_" #c,"Pixel constant",SHADER_PARAM_TYPE_FLOAT,"0",0}
#define REGISTER_INFO(i) CONSTANT_INFO(i,x),CONSTANT_INFO(i,y),CONSTANT_INFO(i,z),CONSTANT_INFO(i,w)
const ShaderParamInfo_t generalParameters[] = {
    {"$pixshader","Screen pixel shader",SHADER_PARAM_TYPE_STRING,"",0},
    {"$linearread_basetexture","Read encoded values",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$linearwrite","Write encoded values",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$writealpha","Write alpha",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$writedepth","Write depth",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$depthtest","Test depth",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$cull","Cull backfaces",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$disable_color_writes","Disable RGB",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$alphablend","Alpha blend",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$alpha_blend","Alpha blend",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$alpha_blend_color_overlay","Premultiplied blend",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$multiplycolor","Multiply destination",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$copyalpha","Copy without alpha test",SHADER_PARAM_TYPE_INTEGER,"0",0},
    {"$vertextransform","Transform world vertices",SHADER_PARAM_TYPE_INTEGER,"0",0},
    REGISTER_INFO(0),REGISTER_INFO(1),REGISTER_INFO(2),REGISTER_INFO(3),REGISTER_INFO(4),REGISTER_INFO(5)
};
#undef REGISTER_INFO
#undef CONSTANT_INFO
int generalProgram(const char* name) {
    const char* programs[]={"unlitgeneric","bloomadd","clearalpha","haloadd","fade_blur","blurgaussian_3x3","luminance_compare","constant_color"};
    for(int i=0;i<8;++i) {
        const auto length=std::strlen(programs[i]);
        if(!Q_strnicmp(name,programs[i],length) && !Q_strnicmp(name+length,"_ps",3))return i;
    }
    return -1;
}
class NativeGeneral final : public CBaseShader {
    const char* name;
    bool resolve;
public:
    explicit NativeGeneral(const char* value,bool ssaa=false):name(value),resolve(ssaa) {}
    const char* GetName() const override { return name; }
    int GetFlags() const override { return SHADER_NOT_EDITABLE; }
    int GetParamCount() const override { return ParameterCount; }
    const ShaderParamInfo_t& GetParamInfo(int index) const override {
        return index<NUM_SHADER_MATERIAL_VARS?CBaseShader::GetParamInfo(index):generalParameters[index-NUM_SHADER_MATERIAL_VARS];
    }
    const char* GetFallbackShader(IMaterialVar** params) const override {
        return !resolve && generalProgram(params[PixelShader]->GetStringValue())<0?"Wireframe_DX9":nullptr;
    }
    void OnInitShaderParams(IMaterialVar** params,const char* material) override {
        for(int i=LinearRead;i<FirstConstant;++i)if(!params[i]->IsDefined())params[i]->SetIntValue(0);
        for(int i=FirstConstant;i<ParameterCount;++i)if(!params[i]->IsDefined())params[i]->SetFloatValue(0);
        if(resolve) {
            params[LinearRead]->SetIntValue(1);params[LinearWrite]->SetIntValue(1);
            params[WriteAlpha]->SetIntValue(1);params[DepthTest]->SetIntValue(1);
        } else if(generalProgram(params[PixelShader]->GetStringValue())<0)
            Warning("Vulkan material %s uses unported screen shader %s; using Wireframe_DX9\n",material,params[PixelShader]->GetStringValue());
    }
    void OnInitShaderInstance(IMaterialVar** params,IShaderInit*,const char*) override {
        if(params[BASETEXTURE]->IsDefined())LoadTexture(BASETEXTURE);
    }
    void OnDrawElements(IMaterialVar** params,IShaderShadow* shadow,IShaderDynamicAPI* api,
            VertexCompressionType_t,CBasePerMaterialContextData**) override {
        const int program=resolve?0:generalProgram(params[PixelShader]->GetStringValue());
        const bool texture=program!=2 && program!=7,srgbRead=!params[LinearRead]->GetIntValue();
        const bool color=(params[FLAGS]->GetIntValue() & MATERIAL_VAR_VERTEXCOLOR)!=0;
        const int vertexIndex=(color?1:0) | (params[VertexTransform]->GetIntValue()?2:0);
        if(shadow) {
            shadow->VertexShaderVertexFormat(VERTEX_POSITION|(color?VERTEX_COLOR:0),1,nullptr,0);
            shadow->EnableTexture(SHADER_SAMPLER0,texture);shadow->EnableSRGBRead(SHADER_SAMPLER0,srgbRead);
            shadow->EnableSRGBWrite(!params[LinearWrite]->GetIntValue());
            shadow->EnableCulling(params[Cull]->GetIntValue()!=0);
            shadow->EnableDepthTest(params[DepthTest]->GetIntValue()!=0 || params[WriteDepth]->GetIntValue()!=0);
            shadow->EnableDepthWrites(params[WriteDepth]->GetIntValue()!=0);
            if(params[WriteDepth]->GetIntValue())shadow->DepthFunc(SHADER_DEPTHFUNC_ALWAYS);
            shadow->EnableColorWrites(!params[DisableColor]->GetIntValue());
            shadow->EnableAlphaWrites(params[WriteAlpha]->GetIntValue()!=0);
            shadow->EnableAlphaTest(!params[CopyAlpha]->GetIntValue());
            shadow->AlphaFunc(SHADER_ALPHAFUNC_GREATER,resolve?1.f/255:0);
            const int flags=params[FLAGS]->GetIntValue();
            if(flags & MATERIAL_VAR_ADDITIVE) {shadow->EnableBlending(true);shadow->BlendFunc(SHADER_BLEND_ONE,SHADER_BLEND_ONE);}
            else if((flags & MATERIAL_VAR_MULTIPLY) || params[MultiplyColor]->GetIntValue()) {
                shadow->EnableBlending(true);shadow->BlendFunc(SHADER_BLEND_ZERO,SHADER_BLEND_SRC_COLOR);
            } else if(params[Premultiplied]->GetIntValue() || params[AlphaBlend]->GetIntValue() || params[AlphaBlend2]->GetIntValue()) {
                shadow->EnableBlending(true);shadow->BlendFunc(params[Premultiplied]->GetIntValue()?SHADER_BLEND_ONE:SHADER_BLEND_SRC_ALPHA,SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
            }
            if(params[CopyAlpha]->GetIntValue())shadow->EnableBlending(false);
            shadow->SetVertexShader("native_general_vs",vertexIndex);shadow->SetPixelShader("native_general_ps",program);
        } else {
            if(texture) {
                if(params[BASETEXTURE]->IsTexture())BindTexture(SHADER_SAMPLER0,SRGBReadMask(srgbRead),BASETEXTURE);
                else api->BindStandardTexture(SHADER_SAMPLER0,SRGBReadMask(srgbRead),TEXTURE_WHITE);
            }
            float constants[24];for(int i=0;i<24;++i)constants[i]=params[FirstConstant+i]->GetFloatValue();
            api->SetPixelShaderConstant(0,constants,6);api->SetVertexShaderModelViewProjAndModelView();
            api->SetVertexShaderIndex(0);api->SetPixelShaderIndex(0);
        }
        Draw();
    }
} general("screenspace_general"),generalDX9("screenspace_general_dx9"),resolve("PanoramaSSAAResolve",true);
} // namespace
