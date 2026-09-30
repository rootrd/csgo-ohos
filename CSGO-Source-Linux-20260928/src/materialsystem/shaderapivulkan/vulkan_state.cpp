#include "vulkan_state.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include "mathlib/vmatrix.h"
#include "materialsystem/IShader.h"
#include "materialsystem/imesh.h"
#include "shaderapi/commandbuffer.h"

namespace sourcevk::detail {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
template<class T> T consume(const unsigned char*& cursor) {
    T value; std::memcpy(&value, cursor, sizeof(value)); cursor += sizeof(value); return value;
}
void parameter(int index) { require(index >= 0 && index < 128, "Source render parameter is out of range"); }
}
SourceStateAPI::Matrix SourceStateAPI::identity() {
    Matrix value {}; value[0] = value[5] = value[10] = value[15] = 1; return value;
}
SourceStateAPI::Matrix SourceStateAPI::multiply(const Matrix& a, const Matrix& b) {
    Matrix value {};
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        for (int k = 0; k < 4; ++k) value[row*4+col] += a[row*4+k] * b[k*4+col];
    return value;
}
SourceStateAPI::Matrix SourceStateAPI::transpose(const Matrix& a) {
    Matrix value;
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) value[row*4+col] = a[col*4+row];
    return value;
}
SourceStateAPI::Matrix SourceStateAPI::inverse(const Matrix& a) {
    double augmented[4][8] {};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) augmented[row][col] = a[row*4+col];
        augmented[row][row+4] = 1;
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        for (int row = col+1; row < 4; ++row) if (std::abs(augmented[row][col]) > std::abs(augmented[pivot][col])) pivot = row;
        require(std::abs(augmented[pivot][col]) > 1e-20, "Cannot invert a singular Source matrix");
        for (int k = 0; k < 8; ++k) std::swap(augmented[pivot][k], augmented[col][k]);
        const double scale = augmented[col][col];
        for (int k = 0; k < 8; ++k) augmented[col][k] /= scale;
        for (int row = 0; row < 4; ++row) if (row != col) {
            const double factor = augmented[row][col];
            for (int k = 0; k < 8; ++k) augmented[row][k] -= factor * augmented[col][k];
        }
    }
    Matrix result;
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col) result[row*4+col] = float(augmented[row][col+4]);
    return result;
}
SourceStateAPI::SourceStateAPI(Context& context, FrameArena& arena) : constants(context, arena) {
    for (auto& stack : matrices) stack.push_back(identity());
    for (auto& bone : bones) { bone.fill(0); bone[0] = bone[5] = bone[10] = 1; }
    for (auto& vector : vectorParameters) vector.Init();
    for (auto& vector : ambient) vector.Init(0,0,0,1);
}
SourceStateAPI::Matrix& SourceStateAPI::currentMatrix() { return matrices[size_t(matrixMode)].back(); }
const SourceStateAPI::Matrix& SourceStateAPI::matrix(MaterialMatrixMode_t mode) const {
    require(mode >= 0 && mode < NUM_MATRIX_MODES, "Invalid Source matrix mode"); return matrices[size_t(mode)].back();
}
void SourceStateAPI::MatrixMode(MaterialMatrixMode_t mode) { matrix(mode); matrixMode = mode; }
void SourceStateAPI::PushMatrix() {
    auto& stack = matrices[size_t(matrixMode)]; require(stack.size() < 64, "Source matrix stack overflow"); stack.push_back(stack.back());
}
void SourceStateAPI::PopMatrix() {
    auto& stack = matrices[size_t(matrixMode)]; require(stack.size() > 1, "Source matrix stack underflow"); stack.pop_back();
}
void SourceStateAPI::LoadMatrix(float* value) {
    require(value && std::all_of(value, value+16, [](float v) { return std::isfinite(v); }), "Invalid Source matrix");
    std::copy_n(value,16,currentMatrix().begin());
}
void SourceStateAPI::MultMatrix(float* value) { require(value, "Null Source matrix"); Matrix other; std::copy_n(value,16,other.begin()); currentMatrix() = multiply(currentMatrix(),other); }
void SourceStateAPI::MultMatrixLocal(float* value) { require(value, "Null Source matrix"); Matrix other; std::copy_n(value,16,other.begin()); currentMatrix() = multiply(other,currentMatrix()); }
void SourceStateAPI::LoadIdentity() { currentMatrix() = identity(); }
void SourceStateAPI::LoadCameraToWorld() {
    auto value = inverse(matrix(MATERIAL_VIEW)); value[12] = value[13] = value[14] = 0; currentMatrix() = value;
}
void SourceStateAPI::GetMatrix(MaterialMatrixMode_t mode, float* value) { require(value, "Null Source matrix output"); const auto& source = matrix(mode); std::copy(source.begin(),source.end(),value); }
void SourceStateAPI::GetActualProjectionMatrix(float* value) {
    require(value, "Null Source projection output");
    const auto& source = projection(); std::copy(source.begin(), source.end(), value);
}
void SourceStateAPI::EnableUserClipTransformOverride(bool enabled) {
    if (userClipOverride != enabled) { userClipOverride = enabled; fastClipDirty = true; }
}
void SourceStateAPI::UserClipTransform(const VMatrix& worldToView) {
    Matrix value;
    // This API takes a column-vector VMatrix, unlike LoadMatrix's D3DX layout.
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        value[row*4+col] = worldToView[col][row];
    require(std::all_of(value.begin(), value.end(), [](float v) { return std::isfinite(v); }),
        "Invalid Source user clip transform");
    if (userClipTransform != value) { userClipTransform = value; fastClipDirty = true; }
}
void SourceStateAPI::EnableFastClip(bool enabled) { fastClipEnabled = enabled; }
void SourceStateAPI::SetFastClipPlane(const float* plane) {
    require(plane, "Null Source fast clip plane");
    // Source supplies n.dot(position) - distance; disabled planes may contain NaNs.
    const std::array<float,4> value {plane[0], plane[1], plane[2], -plane[3]};
    if (fastClipPlane != value) { fastClipPlane = value; fastClipDirty = true; }
}
const SourceStateAPI::Matrix& SourceStateAPI::projection() {
    const auto& source = matrix(MATERIAL_PROJECTION);
    if (!fastClipEnabled) return source;
    const auto& view = userClipOverride ? userClipTransform : matrix(MATERIAL_VIEW);
    if (!fastClipDirty && fastClipView == view && fastClipProjection == source) return clippedProjection;
    require(std::all_of(fastClipPlane.begin(), fastClipPlane.end(), [](float v) { return std::isfinite(v); }) &&
        (fastClipPlane[0] != 0 || fastClipPlane[1] != 0 || fastClipPlane[2] != 0), "Invalid active Source fast clip plane");
    // Replace the near plane of the 0..w projection, as in Source's default fast
    // clipping path. Inverses are cached across draws sharing the view and plane.
    const auto inverseView = inverse(view), inverseProjection = inverse(source);
    std::array<float,4> plane {}, corner {};
    for (int row = 0; row < 4; ++row) for (int col = 0; col < 4; ++col)
        plane[row] += inverseView[row*4+col] * fastClipPlane[col];
    const auto sign = [](float v) { return float((v > 0) - (v < 0)); };
    const std::array<float,4> clipCorner {sign(plane[0]), sign(plane[1]), 1, 1};
    for (int col = 0; col < 4; ++col) for (int row = 0; row < 4; ++row)
        corner[col] += clipCorner[row] * inverseProjection[row*4+col];
    float denominator = 0;
    for (int i = 0; i < 4; ++i) denominator += plane[i] * corner[i];
    clippedProjection = source;
    // A plane at the opposite clip corner cannot define a finite projection.
    if (std::abs(denominator) > 1e-8f)
        for (int row = 0; row < 4; ++row) clippedProjection[row*4+2] = plane[row] / denominator;
    fastClipView = view; fastClipProjection = source; fastClipDirty = false;
    return clippedProjection;
}
void SourceStateAPI::Ortho(double left, double top, double right, double bottom, double nearZ, double farZ) {
    require(left != right && top != bottom && nearZ != farZ, "Degenerate Source orthographic projection");
    Matrix m {};
    m[0] = float(2/(right-left)); m[5] = float(2/(bottom-top)); m[10] = float(1/(nearZ-farZ));
    m[12] = float((left+right)/(left-right)); m[13] = float((top+bottom)/(top-bottom));
    m[14] = float(nearZ/(nearZ-farZ)); m[15] = 1;
    currentMatrix() = multiply(m,currentMatrix());
}
void SourceStateAPI::PerspectiveX(double fov, double aspect, double nearZ, double farZ) { PerspectiveOffCenterX(fov,aspect,nearZ,farZ,0,1,0,1); }
void SourceStateAPI::PerspectiveOffCenterX(double fov, double aspect, double nearZ, double farZ,
        double bottom, double top, double left, double right) {
    require(fov > 0 && fov < 180 && aspect > 0 && nearZ > 0 && farZ > nearZ && left != right && top != bottom, "Invalid Source perspective projection");
    const double width = 2*nearZ*std::tan(fov*3.141592653589793/360), height = width/aspect;
    const double l = width*(left-0.5), r = width*(right-0.5), b = height*(bottom-0.5), t = height*(top-0.5);
    Matrix m {};
    m[0] = float(2*nearZ/(r-l)); m[5] = float(2*nearZ/(t-b)); m[8] = float((l+r)/(r-l)); m[9] = float((t+b)/(t-b));
    m[10] = float(farZ/(nearZ-farZ)); m[11] = -1; m[14] = float(nearZ*farZ/(nearZ-farZ));
    currentMatrix() = multiply(m,currentMatrix());
}
void SourceStateAPI::PickMatrix(int x, int y, int width, int height) {
    ShaderViewport_t view; GetViewports(&view,1); require(width > 0 && height > 0, "Invalid Source pick extent");
    auto m = identity(); m[0] = float(view.m_nWidth)/width; m[5] = float(view.m_nHeight)/height;
    m[12] = float(view.m_nWidth-2*(x-view.m_nTopLeftX))/width; m[13] = float(view.m_nHeight-2*(y-view.m_nTopLeftY))/height;
    currentMatrix() = multiply(m,currentMatrix());
}
void SourceStateAPI::Rotate(float angle, float x, float y, float z) {
    const float length = std::sqrt(x*x+y*y+z*z); require(length > 0 && std::isfinite(length), "Invalid Source rotation axis");
    x /= length; y /= length; z /= length;
    const float s = std::sin(angle*float(3.141592653589793/180)), c = std::cos(angle*float(3.141592653589793/180)), t = 1-c;
    auto m = identity();
    m[0]=t*x*x+c; m[1]=t*x*y+s*z; m[2]=t*x*z-s*y;
    m[4]=t*x*y-s*z; m[5]=t*y*y+c; m[6]=t*y*z+s*x;
    m[8]=t*x*z+s*y; m[9]=t*y*z-s*x; m[10]=t*z*z+c;
    currentMatrix()=multiply(m,currentMatrix());
}
void SourceStateAPI::Translate(float x, float y, float z) { auto m=identity(); m[12]=x; m[13]=y; m[14]=z; currentMatrix()=multiply(m,currentMatrix()); }
void SourceStateAPI::Scale(float x, float y, float z) { auto m=identity(); m[0]=x; m[5]=y; m[10]=z; currentMatrix()=multiply(m,currentMatrix()); }
void SourceStateAPI::LoadBoneMatrix(int index, const float* value) {
    require(index >= 0 && index < NUM_MODEL_TRANSFORMS && value, "Source bone matrix is out of range");
    std::copy_n(value,12,bones[size_t(index)].begin());
    constants.floats(SourceShaderStage::Vertex,VERTEX_SHADER_BONE_TRANSFORM(index),value,3);
    if (index == 0) {
        auto m=identity();
        for (int row=0;row<3;++row) for (int col=0;col<4;++col) m[col*4+row]=value[row*4+col];
        matrices[MATERIAL_MODEL].back()=m;
    }
}
void SourceStateAPI::SetNumBoneWeights(int count) { require(count >= 0 && count <= NUM_MODEL_TRANSFORMS, "Source skinning matrix count is out of range"); boneWeights=count; }
bool SourceStateAPI::SetSkinningMatrices(const MeshInstanceData_t& instance) {
    require(instance.m_nBoneCount >= 0 && instance.m_nBoneCount <= NUM_MODEL_TRANSFORMS, "Source bone count exceeds the register bank");
    if (!instance.m_pPoseToWorld) { UpdateVertexShaderMatrix(0); return false; }
    for (int i=0;i<std::max(1,instance.m_nBoneCount);++i) {
        int destination=i, source=i;
        if(instance.m_pBoneRemap) { destination=instance.m_pBoneRemap[i].m_nActualBoneIndex; source=instance.m_pBoneRemap[i].m_nSrcBoneIndex; }
        require(source >= 0 && source < 256, "Invalid Source pose remap");
        LoadBoneMatrix(destination, instance.m_pPoseToWorld[source].Base());
    }
    return true;
}
void SourceStateAPI::SetVertexShaderViewProj() { const auto m=transpose(multiply(matrix(MATERIAL_VIEW),projection())); constants.floats(SourceShaderStage::Vertex,VERTEX_SHADER_VIEWPROJ,m.data(),4); }
void SourceStateAPI::SetVertexShaderModelViewProjAndModelView() {
    const auto mv=multiply(matrix(MATERIAL_MODEL),matrix(MATERIAL_VIEW));
    const auto mvp=transpose(multiply(mv,projection())), viewModel=transpose(mv);
    constants.floats(SourceShaderStage::Vertex,VERTEX_SHADER_MODELVIEWPROJ,mvp.data(),4);
    constants.floats(SourceShaderStage::Vertex,VERTEX_SHADER_VIEWMODEL,viewModel.data(),4);
}
void SourceStateAPI::UpdateVertexShaderMatrix(int index) {
    require(index >= 0 && index < NUM_MODEL_TRANSFORMS, "Invalid Source model transform");
    if (!index && !boneWeights) { const auto m=transpose(matrix(MATERIAL_MODEL)); constants.floats(SourceShaderStage::Vertex,VERTEX_SHADER_MODEL,m.data(),3); }
    else constants.floats(SourceShaderStage::Vertex,VERTEX_SHADER_BONE_TRANSFORM(index),bones[size_t(index)].data(),3);
}
void SourceStateAPI::commitTransforms() { SetVertexShaderViewProj(); SetVertexShaderModelViewProjAndModelView(); UpdateVertexShaderMatrix(0); SetVertexShaderCameraPos(); }
void SourceStateAPI::GetWorldSpaceCameraPosition(float* position) const { require(position, "Null Source camera output"); const auto m=inverse(matrix(MATERIAL_VIEW)); std::copy_n(m.data()+12,3,position); }
void SourceStateAPI::GetWorldSpaceCameraDirection(float* direction) const { require(direction, "Null Source camera direction"); const auto m=inverse(matrix(MATERIAL_VIEW)); for(int i=0;i<3;++i) direction[i]=-m[8+i]; }
void SourceStateAPI::SetVertexShaderCameraPos() { float value[4]={0,0,0,1}; GetWorldSpaceCameraPosition(value); constants.floats(SourceShaderStage::Vertex,VERTEX_SHADER_CAMERA_POS,value,1); }
void SourceStateAPI::SetStandardVertexShaderConstants(float overbright) { const float value[8]={0,1,2,0.5f,1/2.2f,overbright,1/3.f,overbright ? 1/overbright : 1}; constants.floats(SourceShaderStage::Vertex,0,value,2); }
float SourceStateAPI::GetFarZ() { const auto& p=matrix(MATERIAL_PROJECTION); return std::abs(p[10]+1)>1e-8f ? p[14]/(p[10]+1) : 100000; }
void SourceStateAPI::SetVSNearAndFarZ(int index) { const auto& p=matrix(MATERIAL_PROJECTION); float value[4]={p[10] ? p[14]/p[10] : 0,GetFarZ(),0,0}; constants.floats(SourceShaderStage::Vertex,index,value,1); }
void SourceStateAPI::SetScreenSizeForVPOS(int index) { int w,h; GetCurrentRenderTargetDimensions(w,h); const float value[4]={float(w),float(h),w ? 1.f/w : 0,h ? 1.f/h : 0}; constants.floats(SourceShaderStage::Pixel,index,value,1); }
void SourceStateAPI::SetLights(int count, const LightDesc_t* lights) {
    require(count>=0 && count<=MATERIAL_MAX_LIGHT_COUNT && (!count || lights), "Source light count is out of range");
    lighting.m_nLocalLightCount=count;
    if (count) std::copy_n(lights,count,lighting.m_pLocalLightDesc);
    commitLighting();
}
void SourceStateAPI::SetLightingState(const MaterialLightingState_t& state) {
    require(state.m_nLocalLightCount>=0 && state.m_nLocalLightCount<=MATERIAL_MAX_LIGHT_COUNT, "Invalid Source lighting state");
    lighting=state;
    for(int i=0;i<6;++i) ambient[i].Init(state.m_vecAmbientCube[i].x,state.m_vecAmbientCube[i].y,state.m_vecAmbientCube[i].z,1);
    commitLighting();
}
void SourceStateAPI::SetAmbientLightCube(Vector4D cube[6]) { require(cube, "Null Source ambient cube"); std::copy_n(cube,6,ambient.begin()); commitLighting(); }
void SourceStateAPI::commitLighting() {
    constants.floats(SourceShaderStage::Vertex,VERTEX_SHADER_AMBIENT_LIGHT,ambient[0].Base(),6);
    std::array<float,80> values {};
    for(int i=0;i<lighting.m_nLocalLightCount;++i) {
        const auto& light=lighting.m_pLocalLightDesc[i]; auto* p=values.data()+i*20;
        std::copy_n(light.m_Color.Base(),3,p); p[3]=float(light.m_Type);
        std::copy_n(light.m_Position.Base(),3,p+4); p[7]=light.m_Range;
        std::copy_n(light.m_Direction.Base(),3,p+8); p[11]=light.m_Falloff;
        p[12]=light.m_Attenuation0; p[13]=light.m_Attenuation1; p[14]=light.m_Attenuation2; p[15]=light.m_ThetaDot;
        p[16]=light.m_PhiDot; p[17]=light.m_OneOverThetaDotMinusPhiDot;
    }
    constants.floats(SourceShaderStage::Vertex,VERTEX_SHADER_LIGHTS,values.data(),20);
}
void SourceStateAPI::GetDX9LightState(LightState_t* state) const {
    require(state, "Null Source lighting output"); *state={}; state->m_nNumLights=lighting.m_nLocalLightCount; state->m_bStaticLight=staticLighting;
    for(const auto& side:ambient) state->m_bAmbientLight |= side.x!=0 || side.y!=0 || side.z!=0;
}
void SourceStateAPI::SetFloatRenderingParameter(int index,float value) { parameter(index); floatParameters[index]=value; }
void SourceStateAPI::SetIntRenderingParameter(int index,int value) { parameter(index); intParameters[index]=value; }
void SourceStateAPI::SetVectorRenderingParameter(int index,const Vector& value) { parameter(index); vectorParameters[index]=value; }
void SourceStateAPI::SetTextureRenderingParameter(int index,ITexture* value) { parameter(index); textureParameters[index]=value; }
float SourceStateAPI::GetFloatRenderingParameter(int index) const { parameter(index); return floatParameters[index]; }
int SourceStateAPI::GetIntRenderingParameter(int index) const { parameter(index); return intParameters[index]; }
Vector SourceStateAPI::GetVectorRenderingParameter(int index) const { parameter(index); return vectorParameters[index]; }
ITexture* SourceStateAPI::GetTextureRenderingParameter(int index) const { parameter(index); return textureParameters[index]; }
void SourceStateAPI::SetClientRenderStateBufferState(ClientRenderStateBuffer_t type,const void* data,int bytes) { require(bytes>=0 && (data || !bytes), "Invalid client render-state buffer"); clientBuffers[int(type)]={data,bytes}; }
bool SourceStateAPI::GetClientRenderStateBufferState(ClientRenderStateBuffer_t type,const void*& data,int& bytes) { const auto found=clientBuffers.find(int(type)); if(found==clientBuffers.end()) {data=nullptr;bytes=0;return false;} data=found->second.first;bytes=found->second.second;return data!=nullptr; }
void SourceStateAPI::GetLightmapDimensions(int* w,int* h) { require(w && h,"Null lightmap dimensions"); if(shaderUtil) shaderUtil->GetLightmapDimensions(w,h); else *w=*h=1; }
ITexture* SourceStateAPI::GetRenderTargetEx(int target) const { return shaderUtil ? shaderUtil->GetRenderTargetEx(target) : nullptr; }
void SourceStateAPI::GetCurrentColorCorrection(ShaderColorCorrectionInfo_t* info) { require(info,"Null color-correction output"); if(shaderUtil) shaderUtil->GetCurrentColorCorrection(info); else *info={}; }
void SourceStateAPI::GetSceneFogColor(unsigned char* rgb) { require(rgb,"Null fog color output"); std::copy(fogColor.begin(),fogColor.end(),rgb); }
void SourceStateAPI::GetFogDistances(float* start,float* end,float* z) { if(start)*start=fogStart; if(end)*end=fogEnd; if(z)*z=fogZ; }
void SourceStateAPI::SetPixelShaderFogParams(int index) {
    float value[4]={0,-std::numeric_limits<float>::max(),0,0};
    if(fogMode!=MATERIAL_FOG_NONE) {
        const float inverseRange=fogEnd!=fogStart?1.f/(fogEnd-fogStart):1.f;
        value[0]=1-inverseRange*fogEnd;value[1]=fogZ;value[2]=std::clamp(fogDensity,0.f,1.f);value[3]=inverseRange;
    }
    constants.floats(SourceShaderStage::Pixel,index,value,1);
}

void SourceStateAPI::ExecuteCommandBuffer(uint8* commands) {
    require(commands,"Null Source command buffer");
    const unsigned char* cursor=commands;
    std::vector<const unsigned char*> returns;
    for (int instruction=0;instruction<65536;++instruction) {
        const int op=consume<int>(cursor);
        switch(op) {
        case CBCMD_END: if(returns.empty())return; cursor=returns.back();returns.pop_back();break;
        case CBCMD_JUMP: cursor=consume<const unsigned char*>(cursor);require(cursor,"Null command jump");break;
        case CBCMD_JSR: {const auto* destination=consume<const unsigned char*>(cursor); require(destination && returns.size()<16,"Invalid command subroutine"); returns.push_back(cursor);cursor=destination;break;}
        case CBCMD_SET_PIXEL_SHADER_FLOAT_CONST:
        case CBCMD_SET_VERTEX_SHADER_FLOAT_CONST:
        case CBCMD_SET_VERTEX_SHADER_FLOAT_CONST_REF: {
            const int first=consume<int>(cursor),count=consume<int>(cursor);
            require(count>=0 && count<=256,"Invalid command register count");
            const void* source=cursor;
            if(op==CBCMD_SET_VERTEX_SHADER_FLOAT_CONST_REF)source=consume<const float*>(cursor); else cursor+=size_t(count)*16;
            require(source,"Null register command payload");
            std::array<float,1024> values; std::memcpy(values.data(),source,size_t(count)*16);
            constants.floats(op==CBCMD_SET_PIXEL_SHADER_FLOAT_CONST ? SourceShaderStage::Pixel : SourceShaderStage::Vertex,first,values.data(),count);break;
        }
        case CBCMD_SETPIXELSHADERFOGPARAMS: SetPixelShaderFogParams(consume<int>(cursor));break;
        case CBCMD_STORE_EYE_POS_IN_PSCONST: {const int first=consume<int>(cursor);float p[4]={0,0,0,consume<float>(cursor)};GetWorldSpaceCameraPosition(p);constants.floats(SourceShaderStage::Pixel,first,p,1);break;}
        case CBCMD_SET_PSHINDEX: SetPixelShaderIndex(consume<int>(cursor));break;
        case CBCMD_SET_VSHINDEX: SetVertexShaderIndex(consume<int>(cursor));break;
        case CBCMD_BIND_STANDARD_TEXTURE:
        case CBCMD_BIND_SHADERAPI_TEXTURE_HANDLE: {
            const int sampler=consume<int>(cursor);
            const auto flags=TextureBindFlags_t(sampler & TEXTURE_BINDFLAGS_VALID_MASK);
            if(op==CBCMD_BIND_STANDARD_TEXTURE) BindStandardTexture(Sampler_t(sampler & ~TEXTURE_BINDFLAGS_VALID_MASK),flags,StandardTextureId_t(consume<int>(cursor)));
            else BindTexture(Sampler_t(sampler & ~TEXTURE_BINDFLAGS_VALID_MASK),flags,consume<ShaderAPITextureHandle_t>(cursor));
            break;
        }
        case CBCMD_SET_VERTEX_SHADER_NEARZFARZ_STATE: SetVSNearAndFarZ(consume<int>(cursor));break;
        default: throw std::logic_error("Unported Source material command opcode "+std::to_string(op));
        }
    }
    throw std::logic_error("Source material command buffer did not terminate");
}
void SourceStateAPI::executeInstanceCommands(const unsigned char* cursor,const std::array<float,4>& modulation) {
    if(!cursor)return;
    for(int instruction=0;instruction<4096;++instruction) {
        const int op=consume<int>(cursor);
        switch(op) {
        case CBICMD_END:return;
        case CBICMD_SETSKINNINGMATRICES: for(int i=0;i<(boneWeights?NUM_MODEL_TRANSFORMS:1);++i)UpdateVertexShaderMatrix(i);break;
        case CBICMD_SETVERTEXSHADERLOCALLIGHTING:
        case CBICMD_SETVERTEXSHADERAMBIENTLIGHTCUBE:commitLighting();break;
        case CBICMD_SETPIXELSHADERAMBIENTLIGHTCUBE: {int reg=consume<int>(cursor);constants.floats(SourceShaderStage::Pixel,reg,ambient[0].Base(),6);break;}
        case CBICMD_SETMODULATIONPIXELSHADERDYNAMICSTATE_IDENTITY: {int reg=consume<int>(cursor);constants.floats(SourceShaderStage::Pixel,reg,modulation.data(),1);break;}
        default:throw std::logic_error("Unported Source instance command opcode "+std::to_string(op));
        }
    }
    throw std::logic_error("Source instance command buffer did not terminate");
}
} // namespace sourcevk::detail
