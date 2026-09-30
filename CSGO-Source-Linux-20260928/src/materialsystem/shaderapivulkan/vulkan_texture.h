#pragma once

#include "vulkan_graphics.h"
#include "vulkan_source.h"

namespace sourcevk {
class SourceDevice;
using SourceTextureHandle = intptr_t;
struct SourceTextureLimits {
    size_t maximumTextures = 16384;
    size_t maximumTextureBytes = 64 * 1024 * 1024;
    size_t maximumShadowBytes = 256 * 1024 * 1024;
};
struct SourceTextureInfo {
    uint32_t width = 0, height = 0, mipLevels = 0;
    ImageFormat sourceFormat = IMAGE_FORMAT_UNKNOWN;
    uint32_t layers = 1;
    bool renderTarget = false, depthTarget = false;
};
struct SourceTextureBinding { Image image; Sampler sampler; };
struct SourceTextureStatistics {
    size_t textures = 0, shadowBytes = 0;
    uint64_t imageUploads = 0, mipUploads = 0;
    uint64_t uploadInspections = 0;
    uint64_t filterFormatChecks = 0;
};
struct SourceTextureDebugInfo {
    std::string name, group;
    SourceTextureInfo info;
    size_t bytes = 0, picmip1Bytes = 0, picmip2Bytes = 0;
    uint32_t binds = 0, maximumBinds = 0;
    bool resident = false;
};

// 2D/cube textures with checked CPU mip storage, plus color/depth render targets.
// Updates share SourceDevice's upload submissions and create new GPU images;
// previously recorded descriptors retain their original images, also mid-frame.
class SourceTextures {
public:
    SourceTextures(Context& context, GraphicsDevice& graphics, SourceTextureLimits limits = {});
    ~SourceTextures();
    SourceTextures(const SourceTextures&) = delete;
    SourceTextures& operator=(const SourceTextures&) = delete;
    SourceTextureHandle create(int width, int height, int depth, ImageFormat format, int mipLevels,
        int copies, uint32_t flags, const char* name, const char* group = "");
    void destroy(SourceTextureHandle handle);
    bool exists(SourceTextureHandle handle) const;
    bool resident(SourceTextureHandle handle) const;
    SourceTextureInfo info(SourceTextureHandle handle) const;
    SourceTextureHandle find(const char* name) const;
    void image(SourceTextureHandle handle, int mip, int face, ImageFormat destination, int z,
        int width, int height, ImageFormat source, const void* data, size_t bytes);
    void subImage(SourceTextureHandle handle, int mip, int face, int x, int y, int z,
        int width, int height, ImageFormat source, const void* data, size_t bytes, size_t rowPitch);
    void minFilter(SourceTextureHandle handle, int mode);
    void magFilter(SourceTextureHandle handle, int mode);
    // Source's per-texture anisotropic hint uses half the hardware maximum
    // (at least 2x), even when the global setting selects bilinear/trilinear.
    void setAnisotropicLevel(int level);
    void wrap(SourceTextureHandle handle, int coordinate, int mode);
    SourceTextureBinding binding(SourceTextureHandle handle, bool srgb, bool noMip = false, bool point = false);
    Image renderImage(SourceTextureHandle handle);
    void flushUploads(SourceDevice& device);
    void releaseResources(bool managed);
    void evictManagedResources();
    SourceTextureStatistics statistics() const;
    std::vector<SourceTextureDebugInfo> debugInfo() const;
    void finishFrame();
    void debugRendering(bool enabled);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sourcevk
