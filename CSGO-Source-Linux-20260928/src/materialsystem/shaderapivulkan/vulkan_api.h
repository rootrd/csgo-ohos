#pragma once

#include "vulkan_device.h"
#include "vulkan_target.h"
#include "vulkan_texture.h"
#include "vulkan_queries.h"

class IShaderAPI;
class IMaterial;
class IShaderUtil;

namespace sourcevk {
struct SourceAPILimits {
    size_t maximumSnapshots = 32768, maximumShaderInputs = 512, maximumLayouts = 1024;
    SourceTextureLimits textures;
    size_t maximumOcclusionQueries = 4096;
};
struct SourceAPIStatistics {
    uint64_t draws = 0, descriptorSets = 0, descriptorHits = 0, constantUploads = 0;
    size_t snapshots = 0;
    SourceTextureStatistics textures;
    SourceQueryStatistics queries;
};
struct alignas(16) SourceDrawConstants {
    SourceAlphaState alpha;
    std::array<float, 4> modulation {1, 1, 1, 1};
};
static_assert(sizeof(SourceDrawConstants) == 32);

// The ShaderApi029 vtable for the attached Source device. Implemented calls
// feed real Vulkan resources/draws; remaining entry points throw by name.
// Shaders must use the explicit source_api.hlsl resource/push-constant contract.
// Register their semantic inputs before taking or drawing material snapshots.
class SourceAPI {
public:
    SourceAPI(Context& context, GraphicsDevice& graphics, FrameArena& arena, SourceDevice& device,
        SourceShadow& shadow, SourceShaderLibrary& shaders, SourceAPILimits limits = {});
    ~SourceAPI();
    SourceAPI(const SourceAPI&) = delete;
    SourceAPI& operator=(const SourceAPI&) = delete;
    IShaderAPI& interface();
    void registerShaderInputs(const std::string& vertexName, std::vector<SourceShaderInput> inputs,
        uint32_t staticIndex = UINT32_MAX, uint32_t dynamicIndex = UINT32_MAX,
        bool dynamicTextureReads = false);
    // Returns false for a temporarily unavailable surface. EndFrame closes the
    // passes, then ShaderDevice001::Present submits, matching the Source ABI.
    bool beginFrame();
    const Frame& frame() const;
    // Explicit offscreen attachment bridge until texture render targets are
    // integrated. Call before the frame's present pass; target outlives the pass.
    void beginTarget(RenderTarget& target, const std::array<float, 4>& clear = {0,0,0,0}, float depth = 1);
    void endTarget();
    void flushUploads();
    // The engine supplies its IMaterialInternal::DrawMesh bridge here. The
    // callback selects snapshots/constants, then calls BeginPass/RenderPass.
    void setMaterialPassCallback(std::function<void(IMaterial*, const SourceMeshDraw&)> callback);
    void setShaderUtil(IShaderUtil* util);
    SourceAPIStatistics statistics() const;
    std::vector<SourceTextureDebugInfo> debugTextures() const;
    void debugTextureRendering(bool enabled);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sourcevk
