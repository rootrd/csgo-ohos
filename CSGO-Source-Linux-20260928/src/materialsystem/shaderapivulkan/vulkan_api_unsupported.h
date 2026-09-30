#pragma once

#include <stdexcept>
#include <string>
#include "shaderapi/ishaderapi.h"

namespace sourcevk::detail {
[[noreturn]] inline void unsupportedSourceAPI(const char* method) {
    throw std::logic_error(std::string("Source Vulkan has not implemented IShaderAPI::") + method);
}
// Desktop/Android ABI coverage from ishaderdynamic.h and ishaderapi.h. These
// explicit failures keep an unported engine path from silently drawing nothing.
// SourceAPI::Impl overrides the implemented subset. Console-only slots are not
// part of this vtable. Keep this list in step with the actual interface headers.
class SourceAPIUnsupported : public IShaderAPI {
public:
    void SetVertexShaderViewProj() override { unsupportedSourceAPI("SetVertexShaderViewProj"); }
    void UpdateVertexShaderMatrix( int m ) override { unsupportedSourceAPI("UpdateVertexShaderMatrix"); }
    void SetVertexShaderModelViewProjAndModelView() override { unsupportedSourceAPI("SetVertexShaderModelViewProjAndModelView"); }
    void SetVertexShaderCameraPos() override { unsupportedSourceAPI("SetVertexShaderCameraPos"); }
    bool SetSkinningMatrices( const MeshInstanceData_t &instance ) override { unsupportedSourceAPI("SetSkinningMatrices"); }
    void BindTexture( Sampler_t sampler, TextureBindFlags_t nBindFlags, ShaderAPITextureHandle_t textureHandle ) override { unsupportedSourceAPI("BindTexture"); }
    double CurrentTime() const override { unsupportedSourceAPI("CurrentTime"); }
    void GetLightmapDimensions( int *w, int *h ) override { unsupportedSourceAPI("GetLightmapDimensions"); }
    MaterialFogMode_t GetSceneFogMode( ) override { unsupportedSourceAPI("GetSceneFogMode"); }
    void GetSceneFogColor( unsigned char *rgb ) override { unsupportedSourceAPI("GetSceneFogColor"); }
    void SetVertexShaderConstant( int var, float const* pVec, int numConst, bool bForce) override { unsupportedSourceAPI("SetVertexShaderConstant"); }
    void SetPixelShaderConstant( int var, float const* pVec, int numConst, bool bForce) override { unsupportedSourceAPI("SetPixelShaderConstant"); }
    void SetDefaultState() override { unsupportedSourceAPI("SetDefaultState"); }
    void GetWorldSpaceCameraPosition( float* pPos ) const override { unsupportedSourceAPI("GetWorldSpaceCameraPosition"); }
    void GetWorldSpaceCameraDirection( float* pDir ) const override { unsupportedSourceAPI("GetWorldSpaceCameraDirection"); }
    int GetCurrentNumBones( void ) const override { unsupportedSourceAPI("GetCurrentNumBones"); }
    MaterialFogMode_t GetCurrentFogType( void ) const override { unsupportedSourceAPI("GetCurrentFogType"); }
    void SetVertexShaderIndex( int vshIndex) override { unsupportedSourceAPI("SetVertexShaderIndex"); }
    void SetPixelShaderIndex( int pshIndex) override { unsupportedSourceAPI("SetPixelShaderIndex"); }
    void GetBackBufferDimensions( int& width, int& height ) const override { unsupportedSourceAPI("GetBackBufferDimensions"); }
    const AspectRatioInfo_t &GetAspectRatioInfo( void ) const override { unsupportedSourceAPI("GetAspectRatioInfo"); }
    void GetCurrentRenderTargetDimensions( int& nWidth, int& nHeight ) const override { unsupportedSourceAPI("GetCurrentRenderTargetDimensions"); }
    void GetCurrentViewport( int& nX, int& nY, int& nWidth, int& nHeight ) const override { unsupportedSourceAPI("GetCurrentViewport"); }
    void SetPixelShaderFogParams( int reg ) override { unsupportedSourceAPI("SetPixelShaderFogParams"); }
    bool InFlashlightMode() const override { unsupportedSourceAPI("InFlashlightMode"); }
    const FlashlightState_t &GetFlashlightState( VMatrix &worldToTexture ) const override { unsupportedSourceAPI("GetFlashlightState"); }
    bool InEditorMode() const override { unsupportedSourceAPI("InEditorMode"); }
    bool IsCascadedShadowMapping() const override { unsupportedSourceAPI("IsCascadedShadowMapping"); }
    const CascadedShadowMappingState_t &GetCascadedShadowMappingState( ITexture **pDepthTextureAtlas, bool bLightMapScale) const override { unsupportedSourceAPI("GetCascadedShadowMappingState"); }
    void BindStandardTexture( Sampler_t sampler, TextureBindFlags_t nBindFlags, StandardTextureId_t id ) override { unsupportedSourceAPI("BindStandardTexture"); }
    ITexture *GetRenderTargetEx( int nRenderTargetID ) const override { unsupportedSourceAPI("GetRenderTargetEx"); }
    void SetToneMappingScaleLinear( const Vector &scale ) override { unsupportedSourceAPI("SetToneMappingScaleLinear"); }
    const Vector &GetToneMappingScaleLinear( void ) const override { unsupportedSourceAPI("GetToneMappingScaleLinear"); }
    void SetFloatRenderingParameter(int parm_number, float value) override { unsupportedSourceAPI("SetFloatRenderingParameter"); }
    void SetIntRenderingParameter(int parm_number, int value) override { unsupportedSourceAPI("SetIntRenderingParameter"); }
    void SetVectorRenderingParameter(int parm_number, Vector const &value) override { unsupportedSourceAPI("SetVectorRenderingParameter"); }
    float GetFloatRenderingParameter(int parm_number) const override { unsupportedSourceAPI("GetFloatRenderingParameter"); }
    int GetIntRenderingParameter(int parm_number) const override { unsupportedSourceAPI("GetIntRenderingParameter"); }
    Vector GetVectorRenderingParameter(int parm_number) const override { unsupportedSourceAPI("GetVectorRenderingParameter"); }
    const FlashlightState_t &GetFlashlightStateEx( VMatrix &worldToTexture, ITexture **pFlashlightDepthTexture ) const override { unsupportedSourceAPI("GetFlashlightStateEx"); }
    void GetDX9LightState( LightState_t *state ) const override { unsupportedSourceAPI("GetDX9LightState"); }
    int GetPixelFogCombo( ) override { unsupportedSourceAPI("GetPixelFogCombo"); }
    void BindStandardVertexTexture( VertexTextureSampler_t sampler, StandardTextureId_t id ) override { unsupportedSourceAPI("BindStandardVertexTexture"); }
    bool IsHWMorphingEnabled( ) const override { unsupportedSourceAPI("IsHWMorphingEnabled"); }
    void GetStandardTextureDimensions( int *pWidth, int *pHeight, StandardTextureId_t id ) override { unsupportedSourceAPI("GetStandardTextureDimensions"); }
    void SetBooleanVertexShaderConstant( int var, BOOL const* pVec, int numBools, bool bForce) override { unsupportedSourceAPI("SetBooleanVertexShaderConstant"); }
    void SetIntegerVertexShaderConstant( int var, int const* pVec, int numIntVecs, bool bForce) override { unsupportedSourceAPI("SetIntegerVertexShaderConstant"); }
    void SetBooleanPixelShaderConstant( int var, BOOL const* pVec, int numBools, bool bForce) override { unsupportedSourceAPI("SetBooleanPixelShaderConstant"); }
    void SetIntegerPixelShaderConstant( int var, int const* pVec, int numIntVecs, bool bForce) override { unsupportedSourceAPI("SetIntegerPixelShaderConstant"); }
    bool ShouldWriteDepthToDestAlpha( void ) const override { unsupportedSourceAPI("ShouldWriteDepthToDestAlpha"); }
    void GetMatrix( MaterialMatrixMode_t matrixMode, float *dst ) override { unsupportedSourceAPI("GetMatrix"); }
    void PushDeformation( DeformationBase_t const *Deformation ) override { unsupportedSourceAPI("PushDeformation"); }
    void PopDeformation( ) override { unsupportedSourceAPI("PopDeformation"); }
    int GetNumActiveDeformations() const override { unsupportedSourceAPI("GetNumActiveDeformations"); }
    int GetPackedDeformationInformation( int nMaskOfUnderstoodDeformations, float *pConstantValuesOut, int nBufferSize, int nMaximumDeformations, int *pNumDefsOut ) const override { unsupportedSourceAPI("GetPackedDeformationInformation"); }
    void MarkUnusedVertexFields( unsigned int nFlags, int nTexCoordCount, bool *pUnusedTexCoords ) override { unsupportedSourceAPI("MarkUnusedVertexFields"); }
    void ExecuteCommandBuffer( uint8 *pCmdBuffer ) override { unsupportedSourceAPI("ExecuteCommandBuffer"); }
    void GetCurrentColorCorrection( ShaderColorCorrectionInfo_t* pInfo ) override { unsupportedSourceAPI("GetCurrentColorCorrection"); }
    ITexture *GetTextureRenderingParameter(int parm_number) const override { unsupportedSourceAPI("GetTextureRenderingParameter"); }
    void SetScreenSizeForVPOS( int pshReg) override { unsupportedSourceAPI("SetScreenSizeForVPOS"); }
    void SetVSNearAndFarZ( int vshReg ) override { unsupportedSourceAPI("SetVSNearAndFarZ"); }
    float GetFarZ() override { unsupportedSourceAPI("GetFarZ"); }
    bool SinglePassFlashlightModeEnabled( void ) override { unsupportedSourceAPI("SinglePassFlashlightModeEnabled"); }
    void GetActualProjectionMatrix( float *pMatrix ) override { unsupportedSourceAPI("GetActualProjectionMatrix"); }
    void SetDepthFeatheringShaderConstants( int iConstant, float fDepthBlendScale ) override { unsupportedSourceAPI("SetDepthFeatheringShaderConstants"); }
    void GetFlashlightShaderInfo( bool *pShadowsEnabled, bool *pUberLight ) const override { unsupportedSourceAPI("GetFlashlightShaderInfo"); }
    float GetFlashlightAmbientOcclusion( ) const override { unsupportedSourceAPI("GetFlashlightAmbientOcclusion"); }
    void SetTextureFilterMode( Sampler_t sampler, TextureFilterMode_t nMode ) override { unsupportedSourceAPI("SetTextureFilterMode"); }
    TessellationMode_t GetTessellationMode() const override { unsupportedSourceAPI("GetTessellationMode"); }
    float GetSubDHeight() override { unsupportedSourceAPI("GetSubDHeight"); }
    bool IsRenderingPaint() const override { unsupportedSourceAPI("IsRenderingPaint"); }
    bool IsStereoActiveThisFrame() const override { unsupportedSourceAPI("IsStereoActiveThisFrame"); }
    bool IsStandardTextureHandleValid( StandardTextureId_t textureId ) override { unsupportedSourceAPI("IsStandardTextureHandleValid"); }
    bool GetClientRenderStateBufferState( ClientRenderStateBuffer_t eBuffer, void const * &pvBufferBase, int &numBufferBytes ) override { unsupportedSourceAPI("GetClientRenderStateBufferState"); }
    void SetViewports( int nCount, const ShaderViewport_t* pViewports, bool setImmediately) override { unsupportedSourceAPI("SetViewports"); }
    int GetViewports( ShaderViewport_t* pViewports, int nMax ) const override { unsupportedSourceAPI("GetViewports"); }
    void ClearBuffers( bool bClearColor, bool bClearDepth, bool bClearStencil, int renderTargetWidth, int renderTargetHeight ) override { unsupportedSourceAPI("ClearBuffers"); }
    void ClearColor3ub( unsigned char r, unsigned char g, unsigned char b ) override { unsupportedSourceAPI("ClearColor3ub"); }
    void ClearColor4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a ) override { unsupportedSourceAPI("ClearColor4ub"); }
    void BindVertexShader( VertexShaderHandle_t hVertexShader ) override { unsupportedSourceAPI("BindVertexShader"); }
    void BindGeometryShader( GeometryShaderHandle_t hGeometryShader ) override { unsupportedSourceAPI("BindGeometryShader"); }
    void BindPixelShader( PixelShaderHandle_t hPixelShader ) override { unsupportedSourceAPI("BindPixelShader"); }
    void SetRasterState( const ShaderRasterState_t& state ) override { unsupportedSourceAPI("SetRasterState"); }
    bool SetMode( void* hwnd, int nAdapter, const ShaderDeviceInfo_t &info ) override { unsupportedSourceAPI("SetMode"); }
    void ChangeVideoMode( const ShaderDeviceInfo_t &info ) override { unsupportedSourceAPI("ChangeVideoMode"); }
    StateSnapshot_t TakeSnapshot( ) override { unsupportedSourceAPI("TakeSnapshot"); }
    void TexMinFilter( ShaderTexFilterMode_t texFilterMode ) override { unsupportedSourceAPI("TexMinFilter"); }
    void TexMagFilter( ShaderTexFilterMode_t texFilterMode ) override { unsupportedSourceAPI("TexMagFilter"); }
    void TexWrap( ShaderTexCoordComponent_t coord, ShaderTexWrapMode_t wrapMode ) override { unsupportedSourceAPI("TexWrap"); }
    void CopyRenderTargetToTexture( ShaderAPITextureHandle_t textureHandle ) override { unsupportedSourceAPI("CopyRenderTargetToTexture"); }
    void Bind( IMaterial* pMaterial ) override { unsupportedSourceAPI("Bind"); }
    IMesh* GetDynamicMesh( IMaterial* pMaterial, int nHWSkinBoneCount, bool bBuffered, IMesh* pVertexOverride, IMesh* pIndexOverride) override { unsupportedSourceAPI("GetDynamicMesh"); }
    IMesh* GetDynamicMeshEx( IMaterial* pMaterial, VertexFormat_t vertexFormat, int nHWSkinBoneCount, bool bBuffered, IMesh* pVertexOverride, IMesh* pIndexOverride) override { unsupportedSourceAPI("GetDynamicMeshEx"); }
    bool IsTranslucent( StateSnapshot_t id ) const override { unsupportedSourceAPI("IsTranslucent"); }
    bool IsAlphaTested( StateSnapshot_t id ) const override { unsupportedSourceAPI("IsAlphaTested"); }
    bool UsesVertexAndPixelShaders( StateSnapshot_t id ) const override { unsupportedSourceAPI("UsesVertexAndPixelShaders"); }
    bool IsDepthWriteEnabled( StateSnapshot_t id ) const override { unsupportedSourceAPI("IsDepthWriteEnabled"); }
    VertexFormat_t ComputeVertexFormat( int numSnapshots, StateSnapshot_t* pIds ) const override { unsupportedSourceAPI("ComputeVertexFormat"); }
    VertexFormat_t ComputeVertexUsage( int numSnapshots, StateSnapshot_t* pIds ) const override { unsupportedSourceAPI("ComputeVertexUsage"); }
    void BeginPass( StateSnapshot_t snapshot ) override { unsupportedSourceAPI("BeginPass"); }
    void RenderPass( const unsigned char *pInstanceCommandBuffer, int nPass, int nPassCount ) override { unsupportedSourceAPI("RenderPass"); }
    void SetNumBoneWeights( int numBones ) override { unsupportedSourceAPI("SetNumBoneWeights"); }
    void SetLights( int nCount, const LightDesc_t *pDesc ) override { unsupportedSourceAPI("SetLights"); }
    void SetLightingOrigin( Vector vLightingOrigin ) override { unsupportedSourceAPI("SetLightingOrigin"); }
    void SetLightingState( const MaterialLightingState_t& state ) override { unsupportedSourceAPI("SetLightingState"); }
    void SetAmbientLightCube( Vector4D cube[6] ) override { unsupportedSourceAPI("SetAmbientLightCube"); }
    void ShadeMode( ShaderShadeMode_t mode ) override { unsupportedSourceAPI("ShadeMode"); }
    void CullMode( MaterialCullMode_t cullMode ) override { unsupportedSourceAPI("CullMode"); }
    void FlipCullMode( void ) override { unsupportedSourceAPI("FlipCullMode"); }
    void BeginGeneratingCSMs() override { unsupportedSourceAPI("BeginGeneratingCSMs"); }
    void EndGeneratingCSMs() override { unsupportedSourceAPI("EndGeneratingCSMs"); }
    void PerpareForCascadeDraw( int cascade, float fShadowSlopeScaleDepthBias, float fShadowDepthBias ) override { unsupportedSourceAPI("PerpareForCascadeDraw"); }
    void ForceDepthFuncEquals( bool bEnable ) override { unsupportedSourceAPI("ForceDepthFuncEquals"); }
    void OverrideDepthEnable( bool bEnable, bool bDepthWriteEnable, bool bDepthTestEnable) override { unsupportedSourceAPI("OverrideDepthEnable"); }
    void SetHeightClipZ( float z ) override { unsupportedSourceAPI("SetHeightClipZ"); }
    void SetHeightClipMode( enum MaterialHeightClipMode_t heightClipMode ) override { unsupportedSourceAPI("SetHeightClipMode"); }
    void SetClipPlane( int index, const float *pPlane ) override { unsupportedSourceAPI("SetClipPlane"); }
    void EnableClipPlane( int index, bool bEnable ) override { unsupportedSourceAPI("EnableClipPlane"); }
    ImageFormat GetNearestSupportedFormat( ImageFormat fmt, bool bFilteringRequired) const override { unsupportedSourceAPI("GetNearestSupportedFormat"); }
    ImageFormat GetNearestRenderTargetFormat( ImageFormat fmt ) const override { unsupportedSourceAPI("GetNearestRenderTargetFormat"); }
    bool DoRenderTargetsNeedSeparateDepthBuffer() const override { unsupportedSourceAPI("DoRenderTargetsNeedSeparateDepthBuffer"); }
    ShaderAPITextureHandle_t CreateTexture( int width, int height, int depth, ImageFormat dstImageFormat, int numMipLevels, int numCopies, int flags, const char *pDebugName, const char *pTextureGroupName ) override { unsupportedSourceAPI("CreateTexture"); }
    void DeleteTexture( ShaderAPITextureHandle_t textureHandle ) override { unsupportedSourceAPI("DeleteTexture"); }
    ShaderAPITextureHandle_t CreateDepthTexture( ImageFormat renderTargetFormat, int width, int height, const char *pDebugName, bool bTexture, bool bAliasDepthSurfaceOverColorX360) override { unsupportedSourceAPI("CreateDepthTexture"); }
    bool IsTexture( ShaderAPITextureHandle_t textureHandle ) override { unsupportedSourceAPI("IsTexture"); }
    bool IsTextureResident( ShaderAPITextureHandle_t textureHandle ) override { unsupportedSourceAPI("IsTextureResident"); }
    void ModifyTexture( ShaderAPITextureHandle_t textureHandle ) override { unsupportedSourceAPI("ModifyTexture"); }
    void TexImage2D( int level, int cubeFaceID, ImageFormat dstFormat, int zOffset, int width, int height, ImageFormat srcFormat, bool bSrcIsTiled, void *imageData ) override { unsupportedSourceAPI("TexImage2D"); }
    void TexSubImage2D( int level, int cubeFaceID, int xOffset, int yOffset, int zOffset, int width, int height, ImageFormat srcFormat, int srcStride, bool bSrcIsTiled, void *imageData ) override { unsupportedSourceAPI("TexSubImage2D"); }
    bool TexLock( int level, int cubeFaceID, int xOffset, int yOffset, int width, int height, CPixelWriter& writer ) override { unsupportedSourceAPI("TexLock"); }
    void TexUnlock( ) override { unsupportedSourceAPI("TexUnlock"); }
    void UpdateTexture( int xOffset, int yOffset, int w, int h, ShaderAPITextureHandle_t hDstTexture, ShaderAPITextureHandle_t hSrcTexture ) override { unsupportedSourceAPI("UpdateTexture"); }
    void *LockTex( ShaderAPITextureHandle_t hTexture ) override { unsupportedSourceAPI("LockTex"); }
    void UnlockTex( ShaderAPITextureHandle_t hTexture ) override { unsupportedSourceAPI("UnlockTex"); }
    void TexSetPriority( int priority ) override { unsupportedSourceAPI("TexSetPriority"); }
    void SetRenderTarget( ShaderAPITextureHandle_t colorTextureHandle, ShaderAPITextureHandle_t depthTextureHandle) override { unsupportedSourceAPI("SetRenderTarget"); }
    void ClearBuffersObeyStencil( bool bClearColor, bool bClearDepth ) override { unsupportedSourceAPI("ClearBuffersObeyStencil"); }
    void ReadPixels( int x, int y, int width, int height, unsigned char *data, ImageFormat dstFormat, ITexture *pRenderTargetTexture) override { unsupportedSourceAPI("ReadPixels"); }
    void ReadPixelsAsync( int x, int y, int width, int height, unsigned char *data, ImageFormat dstFormat, ITexture *pRenderTargetTexture, CThreadEvent *pPixelsReadEvent) override { unsupportedSourceAPI("ReadPixelsAsync"); }
    void ReadPixelsAsyncGetResult( int x, int y, int width, int height, unsigned char *data, ImageFormat dstFormat, CThreadEvent *pGetResultEvent) override { unsupportedSourceAPI("ReadPixelsAsyncGetResult"); }
    void ReadPixels( Rect_t *pSrcRect, Rect_t *pDstRect, unsigned char *data, ImageFormat dstFormat, int nDstStride ) override { unsupportedSourceAPI("ReadPixels"); }
    void FlushHardware() override { unsupportedSourceAPI("FlushHardware"); }
    void BeginFrame() override { unsupportedSourceAPI("BeginFrame"); }
    void EndFrame() override { unsupportedSourceAPI("EndFrame"); }
    int SelectionMode( bool selectionMode ) override { unsupportedSourceAPI("SelectionMode"); }
    void SelectionBuffer( unsigned int* pBuffer, int size ) override { unsupportedSourceAPI("SelectionBuffer"); }
    void ClearSelectionNames( ) override { unsupportedSourceAPI("ClearSelectionNames"); }
    void LoadSelectionName( int name ) override { unsupportedSourceAPI("LoadSelectionName"); }
    void PushSelectionName( int name ) override { unsupportedSourceAPI("PushSelectionName"); }
    void PopSelectionName() override { unsupportedSourceAPI("PopSelectionName"); }
    void ForceHardwareSync() override { unsupportedSourceAPI("ForceHardwareSync"); }
    void ClearSnapshots() override { unsupportedSourceAPI("ClearSnapshots"); }
    void FogStart( float fStart ) override { unsupportedSourceAPI("FogStart"); }
    void FogEnd( float fEnd ) override { unsupportedSourceAPI("FogEnd"); }
    void SetFogZ( float fogZ ) override { unsupportedSourceAPI("SetFogZ"); }
    void SceneFogColor3ub( unsigned char r, unsigned char g, unsigned char b ) override { unsupportedSourceAPI("SceneFogColor3ub"); }
    void SceneFogMode( MaterialFogMode_t fogMode ) override { unsupportedSourceAPI("SceneFogMode"); }
    bool CanDownloadTextures() const override { unsupportedSourceAPI("CanDownloadTextures"); }
    void ResetRenderState( bool bFullReset, bool bPanorama) override { unsupportedSourceAPI("ResetRenderState"); }
    int GetCurrentDynamicVBSize( void ) override { unsupportedSourceAPI("GetCurrentDynamicVBSize"); }
    void DestroyVertexBuffers( bool bExitingLevel) override { unsupportedSourceAPI("DestroyVertexBuffers"); }
    void EvictManagedResources() override { unsupportedSourceAPI("EvictManagedResources"); }
    void GetGPUMemoryStats( GPUMemoryStats &stats ) override { unsupportedSourceAPI("GetGPUMemoryStats"); }
    void SetAnisotropicLevel( int nAnisotropyLevel ) override { unsupportedSourceAPI("SetAnisotropicLevel"); }
    void SyncToken( const char *pToken ) override { unsupportedSourceAPI("SyncToken"); }
    void SetStandardVertexShaderConstants( float fOverbright ) override { unsupportedSourceAPI("SetStandardVertexShaderConstants"); }
    ShaderAPIOcclusionQuery_t CreateOcclusionQueryObject( void ) override { unsupportedSourceAPI("CreateOcclusionQueryObject"); }
    void DestroyOcclusionQueryObject( ShaderAPIOcclusionQuery_t ) override { unsupportedSourceAPI("DestroyOcclusionQueryObject"); }
    void BeginOcclusionQueryDrawing( ShaderAPIOcclusionQuery_t ) override { unsupportedSourceAPI("BeginOcclusionQueryDrawing"); }
    void EndOcclusionQueryDrawing( ShaderAPIOcclusionQuery_t ) override { unsupportedSourceAPI("EndOcclusionQueryDrawing"); }
    int OcclusionQuery_GetNumPixelsRendered( ShaderAPIOcclusionQuery_t hQuery, bool bFlush) override { unsupportedSourceAPI("OcclusionQuery_GetNumPixelsRendered"); }
    void SetFlashlightState( const FlashlightState_t &state, const VMatrix &worldToTexture ) override { unsupportedSourceAPI("SetFlashlightState"); }
    void SetCascadedShadowMappingState( const CascadedShadowMappingState_t &state, ITexture *pDepthTextureAtlas ) override { unsupportedSourceAPI("SetCascadedShadowMappingState"); }
    void ClearVertexAndPixelShaderRefCounts() override { unsupportedSourceAPI("ClearVertexAndPixelShaderRefCounts"); }
    void PurgeUnusedVertexAndPixelShaders() override { unsupportedSourceAPI("PurgeUnusedVertexAndPixelShaders"); }
    void DXSupportLevelChanged( int nDXLevel ) override { unsupportedSourceAPI("DXSupportLevelChanged"); }
    void EnableUserClipTransformOverride( bool bEnable ) override { unsupportedSourceAPI("EnableUserClipTransformOverride"); }
    void UserClipTransform( const VMatrix &worldToView ) override { unsupportedSourceAPI("UserClipTransform"); }
    void SetRenderTargetEx( int nRenderTargetID, ShaderAPITextureHandle_t colorTextureHandle, ShaderAPITextureHandle_t depthTextureHandle) override { unsupportedSourceAPI("SetRenderTargetEx"); }
    void CopyRenderTargetToTextureEx( ShaderAPITextureHandle_t textureHandle, int nRenderTargetID, Rect_t *pSrcRect, Rect_t *pDstRect) override { unsupportedSourceAPI("CopyRenderTargetToTextureEx"); }
    void CopyTextureToRenderTargetEx( int nRenderTargetID, ShaderAPITextureHandle_t textureHandle, Rect_t *pSrcRect, Rect_t *pDstRect) override { unsupportedSourceAPI("CopyTextureToRenderTargetEx"); }
    void HandleDeviceLost() override { unsupportedSourceAPI("HandleDeviceLost"); }
    void EnableLinearColorSpaceFrameBuffer( bool bEnable ) override { unsupportedSourceAPI("EnableLinearColorSpaceFrameBuffer"); }
    void SetFullScreenTextureHandle( ShaderAPITextureHandle_t h ) override { unsupportedSourceAPI("SetFullScreenTextureHandle"); }
    void SetFastClipPlane( const float *pPlane ) override { unsupportedSourceAPI("SetFastClipPlane"); }
    void EnableFastClip( bool bEnable ) override { unsupportedSourceAPI("EnableFastClip"); }
    void GetMaxToRender( IMesh *pMesh, bool bMaxUntilFlush, int *pMaxVerts, int *pMaxIndices ) override { unsupportedSourceAPI("GetMaxToRender"); }
    int GetMaxVerticesToRender( IMaterial *pMaterial ) override { unsupportedSourceAPI("GetMaxVerticesToRender"); }
    int GetMaxIndicesToRender( ) override { unsupportedSourceAPI("GetMaxIndicesToRender"); }
    void SetStencilState( const ShaderStencilState_t& state ) override { unsupportedSourceAPI("SetStencilState"); }
    void ClearStencilBufferRectangle(int xmin, int ymin, int xmax, int ymax, int value) override { unsupportedSourceAPI("ClearStencilBufferRectangle"); }
    void DisableAllLocalLights() override { unsupportedSourceAPI("DisableAllLocalLights"); }
    int CompareSnapshots( StateSnapshot_t snapshot0, StateSnapshot_t snapshot1 ) override { unsupportedSourceAPI("CompareSnapshots"); }
    IMesh *GetFlexMesh() override { unsupportedSourceAPI("GetFlexMesh"); }
    void SetFlashlightStateEx( const FlashlightState_t &state, const VMatrix &worldToTexture, ITexture *pFlashlightDepthTexture ) override { unsupportedSourceAPI("SetFlashlightStateEx"); }
    bool SupportsMSAAMode( int nMSAAMode ) override { unsupportedSourceAPI("SupportsMSAAMode"); }
    void AntiAliasingHint( int nHint ) override { unsupportedSourceAPI("AntiAliasingHint"); }
    bool OwnGPUResources( bool bEnable ) override { unsupportedSourceAPI("OwnGPUResources"); }
    void GetFogDistances( float *fStart, float *fEnd, float *fFogZ ) override { unsupportedSourceAPI("GetFogDistances"); }
    void BeginPIXEvent( unsigned long color, const char *szName ) override { unsupportedSourceAPI("BeginPIXEvent"); }
    void EndPIXEvent() override { unsupportedSourceAPI("EndPIXEvent"); }
    void SetPIXMarker( unsigned long color, const char *szName ) override { unsupportedSourceAPI("SetPIXMarker"); }
    void EnableAlphaToCoverage() override { unsupportedSourceAPI("EnableAlphaToCoverage"); }
    void DisableAlphaToCoverage() override { unsupportedSourceAPI("DisableAlphaToCoverage"); }
    void ComputeVertexDescription( unsigned char* pBuffer, VertexFormat_t vertexFormat, MeshDesc_t& desc ) const override { unsupportedSourceAPI("ComputeVertexDescription"); }
    int VertexFormatSize( VertexFormat_t vertexFormat ) const override { unsupportedSourceAPI("VertexFormatSize"); }
    void SetDisallowAccess( bool ) override { unsupportedSourceAPI("SetDisallowAccess"); }
    void EnableShaderShaderMutex( bool ) override { unsupportedSourceAPI("EnableShaderShaderMutex"); }
    void ShaderLock() override { unsupportedSourceAPI("ShaderLock"); }
    void ShaderUnlock() override { unsupportedSourceAPI("ShaderUnlock"); }
    void SetShadowDepthBiasFactors( float fShadowSlopeScaleDepthBias, float fShadowDepthBias ) override { unsupportedSourceAPI("SetShadowDepthBiasFactors"); }
    void BindVertexBuffer( int nStreamID, IVertexBuffer *pVertexBuffer, int nOffsetInBytes, int nFirstVertex, int nVertexCount, VertexFormat_t fmt, int nRepetitions) override { unsupportedSourceAPI("BindVertexBuffer"); }
    void BindIndexBuffer( IIndexBuffer *pIndexBuffer, int nOffsetInBytes ) override { unsupportedSourceAPI("BindIndexBuffer"); }
    void Draw( MaterialPrimitiveType_t primitiveType, int nFirstIndex, int nIndexCount ) override { unsupportedSourceAPI("Draw"); }
    void PerformFullScreenStencilOperation( void ) override { unsupportedSourceAPI("PerformFullScreenStencilOperation"); }
    void SetScissorRect( const int nLeft, const int nTop, const int nRight, const int nBottom, const bool bEnableScissor ) override { unsupportedSourceAPI("SetScissorRect"); }
    void ForceCommitScissorRect() override { unsupportedSourceAPI("ForceCommitScissorRect"); }
    bool SupportsCSAAMode( int nNumSamples, int nQualityLevel ) override { unsupportedSourceAPI("SupportsCSAAMode"); }
    void InvalidateDelayedShaderConstants( void ) override { unsupportedSourceAPI("InvalidateDelayedShaderConstants"); }
    float GammaToLinear_HardwareSpecific( float fGamma ) const override { unsupportedSourceAPI("GammaToLinear_HardwareSpecific"); }
    float LinearToGamma_HardwareSpecific( float fLinear ) const override { unsupportedSourceAPI("LinearToGamma_HardwareSpecific"); }
    void SetLinearToGammaConversionTextures( ShaderAPITextureHandle_t hSRGBWriteEnabledTexture, ShaderAPITextureHandle_t hIdentityTexture ) override { unsupportedSourceAPI("SetLinearToGammaConversionTextures"); }
    void BindVertexTexture( VertexTextureSampler_t nSampler, ShaderAPITextureHandle_t textureHandle ) override { unsupportedSourceAPI("BindVertexTexture"); }
    void EnableHWMorphing( bool bEnable ) override { unsupportedSourceAPI("EnableHWMorphing"); }
    void SetFlexWeights( int nFirstWeight, int nCount, const MorphWeight_t* pWeights ) override { unsupportedSourceAPI("SetFlexWeights"); }
    void FogMaxDensity( float flMaxDensity ) override { unsupportedSourceAPI("FogMaxDensity"); }
    void *GetD3DTexturePtr( ShaderAPITextureHandle_t hTexture ) override { unsupportedSourceAPI("GetD3DTexturePtr"); }
    void CreateTextures( ShaderAPITextureHandle_t *pHandles, int count, int width, int height, int depth, ImageFormat dstImageFormat, int numMipLevels, int numCopies, int flags, const char *pDebugName, const char *pTextureGroupName ) override { unsupportedSourceAPI("CreateTextures"); }
    void AcquireThreadOwnership() override { unsupportedSourceAPI("AcquireThreadOwnership"); }
    void ReleaseThreadOwnership() override { unsupportedSourceAPI("ReleaseThreadOwnership"); }
    void EnableBuffer2FramesAhead( bool bEnable ) override { unsupportedSourceAPI("EnableBuffer2FramesAhead"); }
    void FlipCulling( bool bFlipCulling ) override { unsupportedSourceAPI("FlipCulling"); }
    void SetTextureRenderingParameter(int parm_number, ITexture *pTexture) override { unsupportedSourceAPI("SetTextureRenderingParameter"); }
    void EnableSinglePassFlashlightMode( bool bEnable ) override { unsupportedSourceAPI("EnableSinglePassFlashlightMode"); }
    void MatrixMode( MaterialMatrixMode_t matrixMode ) override { unsupportedSourceAPI("MatrixMode"); }
    void PushMatrix() override { unsupportedSourceAPI("PushMatrix"); }
    void PopMatrix() override { unsupportedSourceAPI("PopMatrix"); }
    void LoadMatrix( float *m ) override { unsupportedSourceAPI("LoadMatrix"); }
    void MultMatrix( float *m ) override { unsupportedSourceAPI("MultMatrix"); }
    void MultMatrixLocal( float *m ) override { unsupportedSourceAPI("MultMatrixLocal"); }
    void LoadIdentity( void ) override { unsupportedSourceAPI("LoadIdentity"); }
    void LoadCameraToWorld( void ) override { unsupportedSourceAPI("LoadCameraToWorld"); }
    void Ortho( double left, double right, double bottom, double top, double zNear, double zFar ) override { unsupportedSourceAPI("Ortho"); }
    void PerspectiveX( double fovx, double aspect, double zNear, double zFar ) override { unsupportedSourceAPI("PerspectiveX"); }
    void PickMatrix( int x, int y, int width, int height ) override { unsupportedSourceAPI("PickMatrix"); }
    void Rotate( float angle, float x, float y, float z ) override { unsupportedSourceAPI("Rotate"); }
    void Translate( float x, float y, float z ) override { unsupportedSourceAPI("Translate"); }
    void Scale( float x, float y, float z ) override { unsupportedSourceAPI("Scale"); }
    void ScaleXY( float x, float y ) override { unsupportedSourceAPI("ScaleXY"); }
    void PerspectiveOffCenterX( double fovx, double aspect, double zNear, double zFar, double bottom, double top, double left, double right ) override { unsupportedSourceAPI("PerspectiveOffCenterX"); }
    void LoadBoneMatrix( int boneIndex, const float *m ) override { unsupportedSourceAPI("LoadBoneMatrix"); }
    void SetStandardTextureHandle( StandardTextureId_t nId, ShaderAPITextureHandle_t nHandle ) override { unsupportedSourceAPI("SetStandardTextureHandle"); }
    void DrawInstances( int nInstanceCount, const MeshInstanceData_t *pInstance ) override { unsupportedSourceAPI("DrawInstances"); }
    void OverrideAlphaWriteEnable( bool bOverrideEnable, bool bAlphaWriteEnable ) override { unsupportedSourceAPI("OverrideAlphaWriteEnable"); }
    void OverrideColorWriteEnable( bool bOverrideEnable, bool bColorWriteEnable ) override { unsupportedSourceAPI("OverrideColorWriteEnable"); }
    void ClearBuffersObeyStencilEx( bool bClearColor, bool bClearAlpha, bool bClearDepth ) override { unsupportedSourceAPI("ClearBuffersObeyStencilEx"); }
    void OnPresent( void ) override { unsupportedSourceAPI("OnPresent"); }
    void UpdateGameTime( float flTime ) override { unsupportedSourceAPI("UpdateGameTime"); }
    bool IsStereoSupported() const override { unsupportedSourceAPI("IsStereoSupported"); }
    void UpdateStereoTexture( ShaderAPITextureHandle_t texHandle, bool *pStereoActiveThisFrame ) override { unsupportedSourceAPI("UpdateStereoTexture"); }
    void SetSRGBWrite( bool bState ) override { unsupportedSourceAPI("SetSRGBWrite"); }
    void PrintfVA( char *fmt, va_list vargs ) override { unsupportedSourceAPI("PrintfVA"); }
    void Printf( char *fmt, ... ) override { unsupportedSourceAPI("Printf"); }
    float Knob( char *knobname, float *setvalue) override { unsupportedSourceAPI("Knob"); }
    void AddShaderComboInformation( const ShaderComboSemantics_t *pSemantics ) override { unsupportedSourceAPI("AddShaderComboInformation"); }
    float GetLightMapScaleFactor() const override { unsupportedSourceAPI("GetLightMapScaleFactor"); }
    ShaderAPITextureHandle_t FindTexture( const char *pDebugName ) override { unsupportedSourceAPI("FindTexture"); }
    void GetTextureDimensions( ShaderAPITextureHandle_t hTexture, int &nWidth, int &nHeight, int &nDepth ) override { unsupportedSourceAPI("GetTextureDimensions"); }
    ShaderAPITextureHandle_t GetStandardTextureHandle(StandardTextureId_t id) override { unsupportedSourceAPI("GetStandardTextureHandle"); }
    void SetClientRenderStateBufferState( ClientRenderStateBuffer_t eBuffer, void const *pvBufferBase, int numBufferBytes ) override { unsupportedSourceAPI("SetClientRenderStateBufferState"); }
    void *GetOSVertexShader( const char* pszName, int nIndex ) override { unsupportedSourceAPI("GetOSVertexShader"); }
    void *GetOSPixelShader( const char* pszName, int nIndex) override { unsupportedSourceAPI("GetOSPixelShader"); }
    void GetShaderApiDynamicState( uint32 **ppRenderStates, SamplerStateCopy_t ** ppSamplerStates, bool *srgbWrite ) override { unsupportedSourceAPI("GetShaderApiDynamicState"); }
    void TexImageFromVTF( IVTFTexture *pVTF, int iVTFFrame ) override { unsupportedSourceAPI("TexImageFromVTF"); }
};
} // namespace sourcevk::detail
