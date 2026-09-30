#pragma once
#include "vulkan_api_unsupported.h"
#include "vulkan_source.h"
#include "shaderapi/ishaderutil.h"

#include <array>
#include <map>
#include <mutex>
#include <vector>

namespace sourcevk::detail {
// CPU render state shared by the engine ABI and the offline shader contract.
// Matrices at the ABI boundary have Source's row-vector/D3DX memory layout;
// shader registers contain its transpose, including the 53 3x4 bone matrices.
class SourceStateAPI : public SourceAPIUnsupported {
protected:
    SourceConstants constants;
    IShaderUtil* shaderUtil = nullptr;
    using Matrix = std::array<float, 16>;
    std::array<std::vector<Matrix>, NUM_MATRIX_MODES> matrices;
    std::array<std::array<float,12>, NUM_MODEL_TRANSFORMS> bones;
    MaterialMatrixMode_t matrixMode = MATERIAL_MODEL;
    bool userClipOverride = false, fastClipEnabled = false, fastClipDirty = true;
    Matrix userClipTransform = identity();
    Matrix fastClipView {}, fastClipProjection {}, clippedProjection {};
    std::array<float,4> fastClipPlane {};
    int boneWeights = 0;
    MaterialLightingState_t lighting {};
    bool staticLighting = false;
    std::array<Vector4D, 6> ambient;
    Vector toneMapping {1,1,1};
    std::array<float, 128> floatParameters {};
    std::array<int, 128> intParameters {};
    std::array<Vector, 128> vectorParameters;
    std::array<ITexture*, 128> textureParameters {};
    std::map<int, std::pair<const void*,int>> clientBuffers;
    MaterialFogMode_t fogMode = MATERIAL_FOG_NONE;
    std::array<unsigned char,3> fogColor {};
    float fogStart = 0, fogEnd = 1, fogZ = 0, fogDensity = 1;
    std::recursive_mutex stateMutex;
    static Matrix identity();
    static Matrix multiply(const Matrix& a, const Matrix& b);
    static Matrix transpose(const Matrix& matrix);
    static Matrix inverse(const Matrix& matrix);
    Matrix& currentMatrix();
    const Matrix& matrix(MaterialMatrixMode_t mode) const;
    const Matrix& projection();
    void commitTransforms();
    void commitLighting();
    void executeInstanceCommands(const unsigned char* commands, const std::array<float,4>& modulation);
public:
    SourceStateAPI(Context& context, FrameArena& arena);
    void MatrixMode(MaterialMatrixMode_t mode) override;
    void PushMatrix() override;
    void PopMatrix() override;
    void LoadMatrix(float* value) override;
    void MultMatrix(float* value) override;
    void MultMatrixLocal(float* value) override;
    void LoadIdentity() override;
    void LoadCameraToWorld() override;
    void GetMatrix(MaterialMatrixMode_t mode, float* value) override;
    void GetActualProjectionMatrix(float* value) override;
    void EnableUserClipTransformOverride(bool enabled) override;
    void UserClipTransform(const VMatrix& worldToView) override;
    void EnableFastClip(bool enabled) override;
    void SetFastClipPlane(const float* plane) override;
    void Ortho(double left, double top, double right, double bottom, double nearZ, double farZ) override;
    void PerspectiveX(double fov, double aspect, double nearZ, double farZ) override;
    void PerspectiveOffCenterX(double fov, double aspect, double nearZ, double farZ,
        double bottom, double top, double left, double right) override;
    void PickMatrix(int x, int y, int width, int height) override;
    void Rotate(float angle, float x, float y, float z) override;
    void Translate(float x, float y, float z) override;
    void Scale(float x, float y, float z) override;
    void ScaleXY(float x, float y) override { Scale(x,y,1); }
    void LoadBoneMatrix(int index, const float* value) override;
    void SetNumBoneWeights(int count) override;
    int GetCurrentNumBones() const override { return boneWeights; }
    bool SetSkinningMatrices(const MeshInstanceData_t& instance) override;
    void SetVertexShaderViewProj() override;
    void SetVertexShaderModelViewProjAndModelView() override;
    void UpdateVertexShaderMatrix(int index) override;
    void SetVertexShaderCameraPos() override;
    void GetWorldSpaceCameraPosition(float* position) const override;
    void GetWorldSpaceCameraDirection(float* direction) const override;
    void SetStandardVertexShaderConstants(float overbright) override;
    void SetVSNearAndFarZ(int index) override;
    float GetFarZ() override;
    void SetScreenSizeForVPOS(int index) override;
    void SetLights(int count, const LightDesc_t* lights) override;
    void SetLightingOrigin(Vector origin) override { lighting.m_vecLightingOrigin = origin; }
    void SetLightingState(const MaterialLightingState_t& state) override;
    void SetAmbientLightCube(Vector4D cube[6]) override;
    void DisableAllLocalLights() override { lighting.m_nLocalLightCount = 0; commitLighting(); }
    void GetDX9LightState(LightState_t* state) const override;
    void SetToneMappingScaleLinear(const Vector& scale) override { toneMapping = scale; }
    const Vector& GetToneMappingScaleLinear() const override { return toneMapping; }
    void SetFloatRenderingParameter(int index, float value) override;
    void SetIntRenderingParameter(int index, int value) override;
    void SetVectorRenderingParameter(int index, const Vector& value) override;
    void SetTextureRenderingParameter(int index, ITexture* value) override;
    float GetFloatRenderingParameter(int index) const override;
    int GetIntRenderingParameter(int index) const override;
    Vector GetVectorRenderingParameter(int index) const override;
    ITexture* GetTextureRenderingParameter(int index) const override;
    void SetClientRenderStateBufferState(ClientRenderStateBuffer_t type, const void* data, int bytes) override;
    bool GetClientRenderStateBufferState(ClientRenderStateBuffer_t type, const void*& data, int& bytes) override;
    void GetLightmapDimensions(int* width, int* height) override;
    ITexture* GetRenderTargetEx(int target) const override;
    void GetCurrentColorCorrection(ShaderColorCorrectionInfo_t* info) override;
    float GetSubDHeight() override { return shaderUtil ? shaderUtil->GetSubDHeight() : 0; }
    TessellationMode_t GetTessellationMode() const override { return TESSELLATION_MODE_DISABLED; }
    MaterialFogMode_t GetSceneFogMode() override { return fogMode; }
    MaterialFogMode_t GetCurrentFogType() const override { return fogMode; }
    void SceneFogMode(MaterialFogMode_t mode) override { fogMode = mode; }
    void SceneFogColor3ub(unsigned char r, unsigned char g, unsigned char b) override { fogColor = {r,g,b}; }
    void GetSceneFogColor(unsigned char* rgb) override;
    void FogStart(float value) override { fogStart = value; }
    void FogEnd(float value) override { fogEnd = value; }
    void SetFogZ(float value) override { fogZ = value; }
    void FogMaxDensity(float value) override { fogDensity = value; }
    void GetFogDistances(float* start, float* end, float* z) override;
    int GetPixelFogCombo() override { return fogMode == MATERIAL_FOG_LINEAR_BELOW_FOG_Z ? 1 : 0; }
    void SetPixelShaderFogParams(int index) override;
    void ExecuteCommandBuffer(uint8* commands) override;
    void ShaderLock() override { stateMutex.lock(); }
    void ShaderUnlock() override { stateMutex.unlock(); }
    void EnableShaderShaderMutex(bool) override {} // All ownership changes use the same recursive lock.
    void AcquireThreadOwnership() override { stateMutex.lock(); }
    void ReleaseThreadOwnership() override { stateMutex.unlock(); }
    void SetDisallowAccess(bool) override {} // No GL-style thread-bound device context exists.
};
} // namespace sourcevk::detail
