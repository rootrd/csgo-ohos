#include "vulkan_api.h"
#include "vulkan_platform.h"

#include <algorithm>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "shaderapi/IShaderDevice.h"
#include "shaderapi/ishaderapi.h"
#include "shaderapi/ishaderutil.h"
#include "shaderapi/ishadershadow.h"
#include "tier0/icommandline.h"
#include "tier1/keyvalues.h"
#include "appframework/ilaunchermgr.h"
#include "../IHardwareConfigInternal.h"
#include "../imaterialinternal.h"
#include "materialsystem/idebugtextureinfo.h"

namespace sourcevk {
namespace {
// MaterialSystem::Connect queries all five interfaces before app-system Init.
// These front objects have module lifetime; only Init creates GPU resources.
struct Module {
    CreateInterfaceFn factory = nullptr;
    ILauncherMgr* launcher = nullptr;
    IShaderUtil* util = nullptr;
    SourceShadow shadow {4.59479342f}; // LDR lightmaps store gamma-encoded light / 2.
    std::unique_ptr<Context> context;
    std::unique_ptr<GraphicsDevice> graphics;
    std::unique_ptr<FrameArena> arena;
    std::unique_ptr<SourceShaderLibrary> shaders;
    std::unique_ptr<SourceDevice> device;
    std::unique_ptr<SourceAPI> api;
    std::vector<ShaderModeChangeCallbackFunc_t> callbacks;
    std::vector<IShaderDeviceDependentObject*> dependents;
    int adapter = 0, flags = 0;
    bool initialized = false;
    bool discoveryVideo = false;
    std::recursive_mutex mutex;

    void shutdown() {
        if (context) context->waitIdle();
        if (api && device) {
            const auto stats=api->statistics(); const auto meshes=device->statistics();
            std::fprintf(stderr,"VK_MODULE_STATS: draws=%llu snapshots=%zu textures=%zu texture_shadow_bytes=%zu mesh_shadow_bytes=%zu\n",
                static_cast<unsigned long long>(stats.draws),stats.snapshots,stats.textures.textures,stats.textures.shadowBytes,meshes.shadowBytes);
            std::fprintf(stderr,"VK_UPLOAD_STATS: texture_inspections=%llu image_uploads=%llu mip_uploads=%llu buffer_inspections=%llu buffer_uploads=%llu submissions=%llu dynamic_copies=%llu\n",
                static_cast<unsigned long long>(stats.textures.uploadInspections),static_cast<unsigned long long>(stats.textures.imageUploads),
                static_cast<unsigned long long>(stats.textures.mipUploads),static_cast<unsigned long long>(meshes.uploadInspections),
                static_cast<unsigned long long>(meshes.bufferUploads),static_cast<unsigned long long>(meshes.uploadSubmissions),
                static_cast<unsigned long long>(meshes.dynamicCopies));
            std::fprintf(stderr,"VK_QUERY_STATS: begins=%llu segments=%llu results=%llu forced_waits=%llu prefix_flushes=%llu live=%zu pools=%zu\n",
                static_cast<unsigned long long>(stats.queries.begins),static_cast<unsigned long long>(stats.queries.segments),
                static_cast<unsigned long long>(stats.queries.resultReads),static_cast<unsigned long long>(stats.queries.forcedWaits),
                static_cast<unsigned long long>(stats.queries.prefixFlushes),stats.queries.liveQueries,stats.queries.pools);
        }
        if (graphics) {
            const auto& stats=graphics->statistics();
            std::fprintf(stderr,"VK_SAMPLER_STATS: creations=%llu cache_hits=%llu format_checks=%llu\n",
                static_cast<unsigned long long>(stats.samplerCreations),static_cast<unsigned long long>(stats.samplerHits),
                static_cast<unsigned long long>(api?api->statistics().textures.filterFormatChecks:0));
        }
        if (api) { api->interface().Bind(nullptr); api->setMaterialPassCallback({}); }
        api.reset();
        device.reset();
        shaders.reset();
        arena.reset();
        graphics.reset();
        if (context) {
            const auto allocations = context->allocationStatistics();
            std::fprintf(stderr, "VK_MODULE_SHUTDOWN: validation_errors=%u live_allocations=%u\n",
                context->validationErrors(), allocations.allocations);
        }
        context.reset();
        shadow.clearSnapshots();
        initialized = false;
    }
};
Module& module() { static Module value; return value; }

IShaderAPI& moduleAPI() {
    if (!module().api) throw std::logic_error("Source Vulkan ShaderAPI called before device initialization");
    return module().api->interface();
}
IShaderDevice& moduleDevice() {
    if (!module().device) throw std::logic_error("Source Vulkan ShaderDevice called before device initialization");
    return module().device->interface();
}
void updateRenderedSize() {
    int width,height; moduleDevice().GetBackBufferDimensions(width,height);
    uint w=uint(width),h=uint(height);
    module().launcher->RenderedSize(w,h,true);
}
void updateWindowMode(const ShaderDeviceInfo_t& mode) {
    int width,height;moduleDevice().GetBackBufferDimensions(width,height);
    module().launcher->SetWindowFullScreen(!mode.m_bWindowed,width,height,false);
    updateRenderedSize();
    auto* window=static_cast<SDL_Window*>(module().launcher->GetWindowRef());
    int windowWidth=0,windowHeight=0,pixelWidth=0,pixelHeight=0;
    SDL_GetWindowSize(window,&windowWidth,&windowHeight);SDL_GetWindowSizeInPixels(window,&pixelWidth,&pixelHeight);
    module().context->requestResize();
    std::fprintf(stderr,"VK_WINDOW_MODE: windowed=%d requested=%dx%d backbuffer=%dx%d window=%dx%d pixels=%dx%d flags=%llu\n",
        mode.m_bWindowed,mode.m_DisplayMode.m_nWidth,mode.m_DisplayMode.m_nHeight,width,height,
        windowWidth,windowHeight,pixelWidth,pixelHeight,static_cast<unsigned long long>(SDL_GetWindowFlags(window)));
}
#include "vulkan_interface_proxies.h"
class ModuleAPI final : public ModuleAPIForwarder {
public:
    // Engine Connect can lock the material system before a GPU device exists.
    // Ownership and nested material locks must always refer to the same mutex.
    void ShaderLock() override { module().mutex.lock(); }
    void ShaderUnlock() override { module().mutex.unlock(); }
    void AcquireThreadOwnership() override { ShaderLock(); }
    void ReleaseThreadOwnership() override { ShaderUnlock(); }
    void EnableShaderShaderMutex(bool) override {}
    void SetDisallowAccess(bool) override {}
    bool SetMode(void* window,int adapter,const ShaderDeviceInfo_t& mode) override {
        if (!moduleAPI().SetMode(window,adapter,mode)) return false;
        updateWindowMode(mode); return true;
    }
    void ChangeVideoMode(const ShaderDeviceInfo_t& mode) override {
        // Resizing retains Vulkan's device and managed data. Source still has
        // to recreate size-dependent targets, standard textures and UI caches.
        module().context->waitIdle();
        const auto started=SDL_GetTicksNS();
        std::fprintf(stderr,"VK_MODE_CHANGE_BEGIN: width=%d height=%d\n",mode.m_DisplayMode.m_nWidth,mode.m_DisplayMode.m_nHeight);
        const auto uploads=module().device->statistics().bufferUploads;
        module().util->ReleaseShaderObjects(MATERIAL_RESTORE_RESIZE_ONLY);
        const auto released=SDL_GetTicksNS();
        moduleAPI().ChangeVideoMode(mode);updateWindowMode(mode);
        const auto changed=SDL_GetTicksNS();
        std::fprintf(stderr,"VK_MODE_CHANGE_RESTORE: release_ms=%.3f window_ms=%.3f\n",(released-started)/1e6,(changed-released)/1e6);
        module().util->RestoreShaderObjects(nullptr,MATERIAL_RESTORE_RESIZE_ONLY);
        std::fprintf(stderr,"VK_MODE_CHANGE_DONE: restore_ms=%.3f total_ms=%.3f buffer_uploads_before=%llu buffer_uploads_after=%llu\n",
            (SDL_GetTicksNS()-changed)/1e6,(SDL_GetTicksNS()-started)/1e6,static_cast<unsigned long long>(uploads),
            static_cast<unsigned long long>(module().device->statistics().bufferUploads));
    }
} apiFront;
ModuleDevice deviceFront;

class HardwareConfig final : public IHardwareConfigInternal {
    bool hdr = false, accurateCSM = false;
public:
    int GetFrameBufferColorDepth() const override { return 32; }
    int GetSamplerCount() const override { return 16; }
    bool HasSetDeviceGammaRamp() const override { return false; }
    bool SupportsStaticControlFlow() const override { return true; }
    VertexCompressionType_t SupportsCompressedVertices() const override { return VERTEX_COMPRESSION_NONE; }
    int MaximumAnisotropicLevel() const override {
        if(!module().context || !module().context->capabilities().enabledFeatures.samplerAnisotropy)return 1;
        return int(module().context->capabilities().properties.limits.maxSamplerAnisotropy);
    }
    int MaxTextureWidth() const override {
        return module().context ? int(std::min(16384u, module().context->capabilities().properties.limits.maxImageDimension2D)) : 4096;
    }
    int MaxTextureHeight() const override { return MaxTextureWidth(); }
    int TextureMemorySize() const override { return 512 * 1024 * 1024; }
    bool SupportsMipmappedCubemaps() const override { return false; }
    bool SupportsNPO2Textures() const override { return true; }
    int NumVertexShaderConstants() const override { return 256; }
    int NumPixelShaderConstants() const override { return 224; }
    int MaxNumLights() const override { return 4; }
    int MaxTextureAspectRatio() const override { return MaxTextureWidth(); }
    int MaxVertexShaderBlendMatrices() const override { return 53; }
    int MaxUserClipPlanes() const override { return 0; }
    bool UseFastClipping() const override { return true; }
    int GetDXSupportLevel() const override { return 95; }
    const char* GetShaderDLLName() const override { return "stdshader_vulkan"; }
    const char* GetHWSpecificShaderDLLName() const override { return "stdshader_vulkan"; }
    bool ReadPixelsFromFrontBuffer() const override { return false; }
    bool PreferDynamicTextures() const override { return true; }
    bool SupportsHDR() const override { return false; }
    bool NeedsAAClamp() const override { return false; }
    bool NeedsATICentroidHack() const override { return false; }
    int GetMaxDXSupportLevel() const override { return 95; }
    bool SpecifiesFogColorInLinearSpace() const override { return true; }
    bool SupportsSRGB() const override { return true; }
    bool FakeSRGBWrite() const override { return false; }
    bool CanDoSRGBReadFromRTs() const override { return true; }
    bool SupportsGLMixedSizeTargets() const override { return false; }
    bool IsAAEnabled() const override { return false; }
    int GetVertexSamplerCount() const override { return 4; }
    int GetMaxVertexTextureDimension() const override { return MaxTextureWidth(); }
    int GetFirstVertexSampler() const override { return 0; }
    bool HasSharedVertexSamplers() const override { return false; }
    int MaxTextureDepth() const override { return 1; }
    HDRType_t GetHDRType() const override { return hdr ? HDR_TYPE_INTEGER : HDR_TYPE_NONE; }
    HDRType_t GetHardwareHDRType() const override { return HDR_TYPE_INTEGER; }
    bool SupportsStreamOffset() const override { return true; }
    int StencilBufferBits() const override { return 8; }
    int MaxViewports() const override { return 1; }
    void OverrideStreamOffsetSupport(bool, bool) override {}
    ShadowFilterMode_t GetShadowFilterMode(bool, bool) const override { return ATI_NOPCF; }
    int NeedsShaderSRGBConversion() const override { return 0; }
    bool UsesSRGBCorrectBlending() const override { return true; }
    bool HasFastVertexTextures() const override { return false; }
    int MaxHWMorphBatchCount() const override { return 0; }
    bool SupportsHDRMode(HDRType_t type) const override { return type == HDR_TYPE_NONE || type == HDR_TYPE_INTEGER; }
    bool GetHDREnabled() const override { return hdr; }
    void SetHDREnabled(bool enabled) override {
        const bool changed=hdr!=enabled;
        hdr=enabled;module().shadow.setLightMapScale(GetLightMapScaleFactor());
        if(changed)std::fprintf(stderr,"VK_HDR_MODE: enabled=%d type=%s lightmap_scale=%g\n",hdr,hdr?"integer":"none",GetLightMapScaleFactor());
    }
    bool SupportsBorderColor() const override { return true; }
    bool SupportsFetch4() const override { return false; }
    float GetShadowDepthBias() const override { return 0.0005f; }
    float GetShadowSlopeScaleDepthBias() const override { return 2; }
    bool PreferZPrepass() const override { return false; }
    bool SuppressPixelShaderCentroidHackFixup() const override { return true; }
    bool PreferTexturesInHWMemory() const override { return false; }
    bool PreferHardwareSync() const override { return false; }
    bool ActualHasFastVertexTextures() const override { return false; }
    bool SupportsShadowDepthTextures() const override { return false; }
    ImageFormat GetShadowDepthTextureFormat() const override { return IMAGE_FORMAT_D24S8; }
    ImageFormat GetHighPrecisionShadowDepthTextureFormat() const override { return IMAGE_FORMAT_D32; }
    ImageFormat GetNullTextureFormat() const override { return IMAGE_FORMAT_RGBA8888; }
    int GetMinDXSupportLevel() const override { return 95; }
    bool IsUnsupported() const override { return false; }
    float GetLightMapScaleFactor() const override { return hdr ? 16.f : 4.59479342f; }
    bool SupportsCascadedShadowMapping() const override { return false; }
    CSMQualityMode_t GetCSMQuality() const override { return CSMQUALITY_LOW; }
    bool SupportsBilinearPCFSampling() const override { return false; }
    CSMShaderMode_t GetCSMShaderMode(CSMQualityMode_t) const override { return CSMSHADERMODE_LOW_OR_VERY_LOW; }
    bool GetCSMAccurateBlending() const override { return accurateCSM; }
    void SetCSMAccurateBlending(bool enabled) override { accurateCSM = enabled; }
    bool SupportsResolveDepth() const override { return false; }
    bool HasFullResolutionDepthTexture() const override { return false; }
} hardware;

class DebugTextureInfo final : public IDebugTextureInfo {
    bool enabled = false, all = false, rendering = false;
    KeyValues* list = nullptr;
    static int bounded(size_t bytes) { return int(std::min(bytes,size_t(INT_MAX))); }
public:
    void clear() { if (list) list->deleteThis(); list = nullptr; }
    void EnableDebugTextureList(bool value) override { enabled = value; }
    void EnableGetAllTextures(bool value) override { all = value; }
    KeyValues* LockDebugTextureList() override {
        module().mutex.lock();
        clear();
        if (!enabled || !module().api) return nullptr;
        list = new KeyValues("TextureList");
        for (const auto& texture : module().api->debugTextures()) {
            if (!all && !texture.binds) continue;
            auto* entry = list->CreateNewKey();
            entry->SetString("Name",texture.name.c_str()); entry->SetString("TexGroup",texture.group.c_str());
            entry->SetInt("Width",int(texture.info.width)); entry->SetInt("Height",int(texture.info.height));
            entry->SetInt("Depth",1); entry->SetInt("Count",1);
            entry->SetString("Format",("Source format "+std::to_string(int(texture.info.sourceFormat))).c_str());
            entry->SetInt("Size",bounded(texture.bytes)); entry->SetInt("BindsFrame",int(texture.binds));
            entry->SetInt("BindsMax",int(texture.maximumBinds));
        }
        return list;
    }
    void UnlockDebugTextureList() override { module().mutex.unlock(); }
    int GetTextureMemoryUsed(TextureMemoryType type) override {
        std::lock_guard<std::recursive_mutex> lock(module().mutex);
        if (!module().api) return 0;
        size_t bytes = 0;
        for (const auto& texture : module().api->debugTextures()) {
            switch (type) {
            case MEMORY_BOUND_LAST_FRAME: if (texture.binds) bytes += texture.bytes; break;
            case MEMORY_TOTAL_LOADED: bytes += texture.bytes; break;
            case MEMORY_ESTIMATE_PICMIP_1: bytes += texture.picmip1Bytes; break;
            case MEMORY_ESTIMATE_PICMIP_2: bytes += texture.picmip2Bytes; break;
            default: break;
            }
        }
        return bounded(bytes);
    }
    bool IsDebugTextureListFresh(int) override {
        // The table is rebuilt on demand under the same material-system lock.
        return enabled && module().context && module().context->statistics().submittedFrames;
    }
    bool SetDebugTextureRendering(bool value) override {
        const bool old = rendering; rendering = value;
        if (module().api) module().api->debugTextureRendering(value);
        return old;
    }
} debugTextures;

void* query(const char* name, int* status);
class DeviceManager final : public IShaderDeviceMgr {
    IShaderDeviceMgr& target() const {
        if (!module().device) throw std::logic_error("Source Vulkan manager used before Init");
        return module().device->manager();
    }
    SDL_Window* window() const {
        return module().launcher ? static_cast<SDL_Window*>(module().launcher->GetWindowRef()) : nullptr;
    }
    static void displayMode(ShaderDisplayMode_t* mode, const platform::DisplayMode& value) {
        if (!mode) throw std::invalid_argument("Null Source display mode output");
        *mode = ShaderDisplayMode_t();
        mode->m_nWidth = value.width; mode->m_nHeight = value.height;
        mode->m_Format = IMAGE_FORMAT_BGRA8888;
        mode->m_nRefreshRateNumerator = value.refreshNumerator;
        mode->m_nRefreshRateDenominator = value.refreshDenominator;
    }
public:
    bool Connect(CreateInterfaceFn factory) override {
        auto& m = module();
        if (!factory || m.factory) return false;
        m.launcher = static_cast<ILauncherMgr*>(factory("SDLMgrInterface001", nullptr));
        m.util = static_cast<IShaderUtil*>(factory(SHADER_UTIL_INTERFACE_VERSION, nullptr));
        if (!m.launcher || !m.util) return false;
        // CMaterialSystem::SetAdapter queries the desktop during Connect, before
        // the launcher's Init creates a window. Keep an SDL video reference for
        // these display queries; the launcher owns the window and its own ref.
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "VK_MODULE_CONNECT_FAILED: %s\n", SDL_GetError());
            return false;
        }
        m.discoveryVideo = true;
        m.factory = factory;
        return true;
    }
    void Disconnect() override {
        auto& m = module();
        m.shutdown(); m.factory = nullptr; m.launcher = nullptr; m.util = nullptr;
        m.callbacks.clear(); m.dependents.clear();
        debugTextures.clear();
        if (m.discoveryVideo) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        m.discoveryVideo = false;
    }
    void* QueryInterface(const char* name) override { return query(name, nullptr); }
    InitReturnVal_t Init() override {
        auto& m = module();
        if (!m.factory || m.initialized) return INIT_FAILED;
        try {
            auto* window = static_cast<SDL_Window*>(m.launcher->GetWindowRef());
            if (!window) throw std::runtime_error("SDL3 launcher has no initialized Vulkan window");
            ContextOptions options;
            options.validation = CommandLine()->FindParm("-vulkan-validation") != 0;
            options.log = [](const std::string& message) { std::fprintf(stderr, "%s\n", message.c_str()); };
            m.context = std::make_unique<Context>(window, options);
            const char* cache = std::getenv("SOURCE_VULKAN_PIPELINE_CACHE");
            m.graphics = std::make_unique<GraphicsDevice>(*m.context, cache ? cache : "");
            m.arena = std::make_unique<FrameArena>(*m.context,1024*1024,64*1024*1024);
            const char* shaderPath = std::getenv("SOURCE_VULKAN_SHADERS");
            m.shaders = std::make_unique<SourceShaderLibrary>(*m.graphics, shaderPath ? shaderPath : "platform/shaders/vulkan");
            SourceDeviceLimits deviceLimits;
            deviceLimits.maximumShadowBytes=512*1024*1024;
            deviceLimits.dynamicVertexBytes=4*1024*1024;deviceLimits.dynamicIndexBytes=2*1024*1024;
            m.device = std::make_unique<SourceDevice>(*m.context, *m.graphics, *m.arena, m.shadow, window,deviceLimits);
            if (!m.device->manager().Connect(m.factory) || m.device->manager().Init() != INIT_OK)
                throw std::runtime_error("Attached Source Vulkan device initialization failed");
            SourceAPILimits apiLimits;
            apiLimits.textures.maximumTextureBytes=128*1024*1024;
            apiLimits.textures.maximumShadowBytes=size_t(2)*1024*1024*1024;
            m.api = std::make_unique<SourceAPI>(*m.context, *m.graphics, *m.arena, *m.device, m.shadow, *m.shaders,apiLimits);
            m.api->setShaderUtil(m.util);
            m.api->registerShaderInputs("native_shadow_vs",{{SourceSemantic::Position,0,0},
                {SourceSemantic::TexCoord,0,1},{SourceSemantic::TexCoord,1,2}});
            for(uint32_t skin=0;skin<2;++skin) {
                std::vector<SourceShaderInput> inputs={{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}};
                if(skin) {inputs.push_back({SourceSemantic::BoneWeights,0,2});inputs.push_back({SourceSemantic::BoneIndices,0,3});}
                m.api->registerShaderInputs("native_shadowbuild_vs",std::move(inputs),0,skin);
            }
            for(int kind=0;kind<3;++kind)for(uint32_t color=0;color<2;++color)for(uint32_t skin=0;skin<(kind==2?4u:1u);++skin) {
                std::vector<SourceShaderInput> inputs={{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}};
                if(color)inputs.push_back({SourceSemantic::Color,0,2});
                if(kind==1)inputs.push_back({SourceSemantic::TexCoord,1,3});
                if(kind==2)inputs.push_back({SourceSemantic::Normal,0,4});
                if(skin&1) {inputs.push_back({SourceSemantic::BoneWeights,0,5});inputs.push_back({SourceSemantic::BoneIndices,0,6});}
                if(skin&2)for(uint32_t component=0;component<3;++component)inputs.push_back({SourceSemantic::Color,component+1,component+7});
                m.api->registerShaderInputs(kind==0?"native_unlit_vs":kind==1?"native_lightmap_vs":"native_model_vs",std::move(inputs),color,skin);
            }
            m.api->registerShaderInputs("native_rope_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::Color,0,1},
                {SourceSemantic::TexCoord,0,2},{SourceSemantic::TexCoord,1,3},{SourceSemantic::TexCoord,2,4},{SourceSemantic::TexCoord,3,5}});
            m.api->registerShaderInputs("native_screen_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}});
            for(uint32_t color=0;color<2;++color)for(uint32_t skin=0;skin<2;++skin) {
                std::vector<SourceShaderInput> inputs={{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}};
                if(color)inputs.push_back({SourceSemantic::Color,0,2});
                if(skin) {inputs.push_back({SourceSemantic::BoneWeights,0,3});inputs.push_back({SourceSemantic::BoneIndices,0,4});}
                m.api->registerShaderInputs("native_twotexture_vs",std::move(inputs),color,skin);
            }
            m.api->registerShaderInputs("native_clear_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::Color,0,1}});
            for(uint32_t variant=0;variant<4;++variant) {
                std::vector<SourceShaderInput> inputs={{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}};
                if(variant&1)inputs.push_back({SourceSemantic::Color,0,2});
                m.api->registerShaderInputs("native_general_vs",std::move(inputs),variant,0);
            }
            for(int fancy=0;fancy<2;++fancy) {
                std::vector<SourceShaderInput> inputs={{SourceSemantic::Position,0,0}};
                for(uint32_t uv=0;uv<(fancy?5u:3u);++uv)inputs.push_back({SourceSemantic::TexCoord,uv,uv+1});
                m.api->registerShaderInputs(fancy?"panoramafancy_vs30":"panorama_vs30",std::move(inputs),UINT32_MAX,UINT32_MAX,true);
            }
            m.api->setMaterialPassCallback([&](IMaterial* material, const SourceMeshDraw& draw) {
                m.util->SyncMatrices();
                static_cast<IMaterialInternal*>(material)->DrawMesh(VERTEX_COMPRESSION_NONE, draw.modulation[3] != 1, false);
            });
            for (auto callback : m.callbacks) target().AddModeChangeCallback(callback);
            for (auto* dependent : m.dependents) target().AddDeviceDependentObject(dependent);
            m.initialized = true;
            std::fprintf(stderr, "VK_MODULE_READY: ShaderApi029 Vulkan 1.1, SDL3 window=%p\n", static_cast<void*>(window));
            return INIT_OK;
        } catch (const std::exception& error) {
            std::fprintf(stderr, "VK_MODULE_INIT_FAILED: %s\n", error.what());
            m.shutdown();
            return INIT_FAILED;
        }
    }
    void Shutdown() override { module().shutdown(); }
    int GetAdapterCount() const override { return module().device ? target().GetAdapterCount() : 1; }
    void GetAdapterInfo(int adapter, MaterialAdapterInfo_t& info) const override { target().GetAdapterInfo(adapter, info); }
    bool GetRecommendedConfigurationInfo(int adapter, int level, KeyValues* config) override {
        if (adapter != 0 || !config) return false;
        config->SetInt("ConVar.mat_dxlevel", 95);
        config->SetInt("ConVar.mat_antialias", 0);
        config->SetInt("ConVar.mat_forceaniso", 1);
        config->SetInt("ConVar.mat_vsync", 1);
        config->SetInt("ConVar.mat_queue_mode", 0);
        return level == 0 || level == 95;
    }
    bool GetRecommendedVideoConfig(int, KeyValues*) override { return false; }
    int GetModeCount(int adapter) const override {
        if (adapter != 0) return 0;
        return int(platform::displayModes(window()).size());
    }
    void GetModeInfo(ShaderDisplayMode_t* mode, int adapter, int index) const override {
        const auto modes = platform::displayModes(window());
        if (adapter != 0 || index < 0 || size_t(index) >= modes.size())
            throw std::invalid_argument("Invalid Source adapter/display mode");
        displayMode(mode, modes[size_t(index)]);
    }
    void GetCurrentModeInfo(ShaderDisplayMode_t* mode, int adapter) const override {
        if (adapter != 0) throw std::invalid_argument("Only the attached Vulkan adapter is exposed");
        displayMode(mode, platform::currentMode(window(), true));
    }
    bool SetAdapter(int adapter, int flags) override {
        if (adapter != 0 || (flags & ~MATERIAL_INIT_ALLOCATE_FULLSCREEN_TEXTURE)) return false;
        module().adapter = adapter; module().flags = flags;
        return true;
    }
    CreateInterfaceFn SetMode(void* window, int adapter, const ShaderDeviceInfo_t& info) override {
        if (!target().SetMode(window, adapter, info)) return nullptr;
        updateWindowMode(info); return query;
    }
    void AddModeChangeCallback(ShaderModeChangeCallbackFunc_t callback) override {
        auto& m = module();
        if (std::find(m.callbacks.begin(), m.callbacks.end(), callback) == m.callbacks.end()) m.callbacks.push_back(callback);
        if (m.device) target().AddModeChangeCallback(callback);
    }
    void RemoveModeChangeCallback(ShaderModeChangeCallbackFunc_t callback) override {
        auto& m = module();
        m.callbacks.erase(std::remove(m.callbacks.begin(), m.callbacks.end(), callback), m.callbacks.end());
        if (m.device) target().RemoveModeChangeCallback(callback);
    }
    void AddDeviceDependentObject(IShaderDeviceDependentObject* object) override {
        auto& m = module();
        if (std::find(m.dependents.begin(), m.dependents.end(), object) == m.dependents.end()) m.dependents.push_back(object);
        if (m.device) target().AddDeviceDependentObject(object);
    }
    void RemoveDeviceDependentObject(IShaderDeviceDependentObject* object) override {
        auto& m = module();
        m.dependents.erase(std::remove(m.dependents.begin(), m.dependents.end(), object), m.dependents.end());
        if (m.device) target().RemoveDeviceDependentObject(object);
    }
} manager;

void* query(const char* name, int* status) {
    void* result = nullptr;
    if (name) {
        if (!std::strcmp(name, SHADER_DEVICE_MGR_INTERFACE_VERSION)) result = static_cast<IShaderDeviceMgr*>(&manager);
        else if (!std::strcmp(name,"SourceVulkanBackend001")) result=static_cast<IShaderDeviceMgr*>(&manager);
        else if (!std::strcmp(name, SHADER_DEVICE_INTERFACE_VERSION)) result = static_cast<IShaderDevice*>(&deviceFront);
        else if (!std::strcmp(name, SHADERAPI_INTERFACE_VERSION)) result = static_cast<IShaderAPI*>(&apiFront);
        else if (!std::strcmp(name, SHADERDYNAMIC_INTERFACE_VERSION)) result = static_cast<IShaderDynamicAPI*>(&apiFront);
        else if (!std::strcmp(name, SHADERSHADOW_INTERFACE_VERSION)) result = &module().shadow.interface();
        else if (!std::strcmp(name, MATERIALSYSTEM_HARDWARECONFIG_INTERFACE_VERSION)) result = static_cast<IHardwareConfigInternal*>(&hardware);
        else if (!std::strcmp(name, DEBUG_TEXTURE_INFO_VERSION)) result = static_cast<IDebugTextureInfo*>(&debugTextures);
    }
    if (status) *status = result ? IFACE_OK : IFACE_FAILED;
    return result;
}
} // namespace
} // namespace sourcevk

extern "C" __attribute__((visibility("default"))) void* CreateInterface(const char* name, int* status) {
    return sourcevk::query(name, status);
}
