#include "vulkan_texture.h"
#include "vulkan_device.h"
#include "vulkan_internal.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include "shaderapi/ishaderapi.h"

namespace sourcevk {
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::invalid_argument(message); }
std::atomic<uintptr_t> nextTexture {1};
VkFormat compressedFormat(ImageFormat format) {
    switch(format) {
    case IMAGE_FORMAT_DXT1:case IMAGE_FORMAT_LINEAR_DXT1:case IMAGE_FORMAT_DXT1_RUNTIME:
    case IMAGE_FORMAT_DXT1_ONEBITALPHA:return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case IMAGE_FORMAT_DXT3:case IMAGE_FORMAT_LINEAR_DXT3:case IMAGE_FORMAT_DXT3_RUNTIME:return VK_FORMAT_BC2_UNORM_BLOCK;
    case IMAGE_FORMAT_DXT5:case IMAGE_FORMAT_LINEAR_DXT5:case IMAGE_FORMAT_DXT5_RUNTIME:return VK_FORMAT_BC3_UNORM_BLOCK;
    default:return VK_FORMAT_UNDEFINED;
    }
}
struct Mip {
    uint32_t width = 0, height = 0;
    bool initialized = false;
    std::vector<uint8_t> pixels;
};
struct Texture {
    SourceTextureInfo info;
    ImageDescription description;
    uint32_t flags = 0, texelBytes = 0;
    std::string name, group;
    uint32_t binds = 0, lastBinds = 0, maximumBinds = 0;
    std::vector<Mip> mips;
    size_t bytes = 0;
    uint64_t revision = 1, gpuRevision = 0;
    Image gpu;
    SamplerDescription sampling;
    bool mipmapped = true;
    bool minAnisotropic = false, magAnisotropic = false;
    float cachedAnisotropy = 1;
    uint8_t linearViews = 0;
    std::array<Sampler, 4> samplers;
    bool complete() const {
        return info.renderTarget || std::all_of(mips.begin(), mips.end(), [](const Mip& mip) { return mip.initialized; });
    }
    size_t mipBytes(uint32_t width,uint32_t height) const {
        const auto block=detail::compressedBlockBytes(description.format);
        return block?size_t((width+3)/4)*((height+3)/4)*block:size_t(width)*height*texelBytes;
    }
};
} // namespace

struct SourceTextures::Impl {
    Context& context;
    GraphicsDevice& graphics;
    SourceTextureLimits limits;
    std::map<SourceTextureHandle, Texture> textures;
    std::set<SourceTextureHandle> pendingUploads;
    SourceTextureStatistics counts;
    float anisotropy = 1;
    bool debugRendering = false;
    Impl(Context& ctx, GraphicsDevice& gfx, SourceTextureLimits budget) : context(ctx), graphics(gfx), limits(budget) {
        require(limits.maximumTextures && limits.maximumTextureBytes && limits.maximumTextureBytes <= 256 * 1024 * 1024 &&
            limits.maximumShadowBytes >= limits.maximumTextureBytes, "Invalid Source texture limits");
    }
    Texture& get(SourceTextureHandle handle) {
        const auto found = textures.find(handle);
        if (handle <= 0 || found == textures.end())
            throw std::invalid_argument("Foreign or deleted Source texture handle "+std::to_string(handle));
        return found->second;
    }
    const Texture& get(SourceTextureHandle handle) const { return const_cast<Impl*>(this)->get(handle); }
    void changed(SourceTextureHandle handle,Texture& texture) {
        ++texture.revision;
        if(texture.complete())pendingUploads.insert(handle);
    }
    Mip& mip(Texture& texture, int level, int face, int z) {
        require(!texture.info.renderTarget && level >= 0 && uint32_t(level) < texture.info.mipLevels &&
            face >= 0 && uint32_t(face) < texture.info.layers && !z, "Invalid Source mip, cube face or depth slice");
        return texture.mips[size_t(face) * texture.info.mipLevels + size_t(level)];
    }
};
SourceTextures::SourceTextures(Context& ctx, GraphicsDevice& gfx, SourceTextureLimits limits)
    : impl_(std::make_unique<Impl>(ctx, gfx, limits)) { setAnisotropicLevel(0); }
SourceTextures::~SourceTextures() = default;

SourceTextureHandle SourceTextures::create(int width, int height, int depth, ImageFormat format, int levels,
        int copies, uint32_t flags, const char* name, const char* group) {
    auto& store = *impl_;
    constexpr uint32_t supportedFlags = TEXTURE_CREATE_MANAGED | TEXTURE_CREATE_DYNAMIC |
        TEXTURE_CREATE_UNFILTERABLE_OK | TEXTURE_CREATE_SRGB | TEXTURE_CREATE_VERTEXTEXTURE | TEXTURE_CREATE_DEFAULT_POOL |
        TEXTURE_CREATE_CUBEMAP | TEXTURE_CREATE_RENDERTARGET | TEXTURE_CREATE_DEPTHBUFFER | TEXTURE_CREATE_SYSMEM |
        TEXTURE_CREATE_ANISOTROPIC;
    require(width > 0 && height > 0 && depth == 1 && copies >= 1 && levels >= 0 && !(flags & ~supportedFlags),
        "Source textures require 2D/cube data and supported creation flags");
    const bool renderTarget = flags & (TEXTURE_CREATE_RENDERTARGET | TEXTURE_CREATE_DEPTHBUFFER);
    const bool depthTarget = flags & TEXTURE_CREATE_DEPTHBUFFER;
    const uint32_t layers = flags & TEXTURE_CREATE_CUBEMAP ? 6 : 1;
    require(layers == 1 || (!renderTarget && width == height), "Invalid Source cubemap target or extent");
    require(uint32_t(width) <= store.context.capabilities().properties.limits.maxImageDimension2D &&
        uint32_t(height) <= store.context.capabilities().properties.limits.maxImageDimension2D &&
        store.textures.size() < store.limits.maximumTextures, "Source texture dimensions or handle budget exceeded");
    uint32_t maximumMips = 0;
    for (uint32_t size = uint32_t(std::max(width, height)); size; size >>= 1) ++maximumMips;
    if (!levels) levels = renderTarget ? 1 : int(maximumMips);
    require(!renderTarget || levels == 1, "Render targets use one mip level");
    require(uint32_t(levels) <= maximumMips, "Source texture has too many mip levels");
    SourceTextureDataLayout layout;
    if (depthTarget) {
        if (format == IMAGE_FORMAT_D16) layout.format = VK_FORMAT_D16_UNORM;
        else if (format == IMAGE_FORMAT_D24S8 || format == IMAGE_FORMAT_D24X8)
            layout.format = store.context.supportsFormat(VK_FORMAT_D24_UNORM_S8_UINT,
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ?
                VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        else layout.format = VK_FORMAT_D32_SFLOAT;
    } else layout = sourceTextureDataLayout(format, uint32_t(width), uint32_t(height));
    if(!renderTarget && store.context.capabilities().enabledFeatures.textureCompressionBC) {
        const auto compressed=compressedFormat(format);
        if(compressed!=VK_FORMAT_UNDEFINED && store.context.supportsFormat(compressed,
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
            layout.format=compressed;
    }
    Texture texture;
    texture.info = {uint32_t(width), uint32_t(height), uint32_t(levels), format};
    texture.info.layers = layers; texture.info.renderTarget = renderTarget; texture.info.depthTarget = depthTarget;
    texture.description.width = uint32_t(width); texture.description.height = uint32_t(height);
    texture.description.mipLevels = uint32_t(levels); texture.description.format = layout.format;
    texture.description.layers = layers; texture.description.cube = layers == 6;
    texture.description.srgbViews = detail::alternateColorSpace(layout.format)!=VK_FORMAT_UNDEFINED;
    texture.flags = flags; texture.texelBytes = layout.outputTexelBytes;
    texture.minAnisotropic=texture.magAnisotropic=(flags & TEXTURE_CREATE_ANISOTROPIC)!=0;
    texture.name = name ? name : "";
    texture.group = group ? group : "";
    require(texture.name.size() <= 1024, "Source texture debug name is too long");
    require(!(flags & TEXTURE_CREATE_SRGB) || texture.description.srgbViews, "This Source format cannot provide an sRGB view");
    require(store.context.supportsFormat(layout.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        (renderTarget ? (depthTarget ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) : VK_FORMAT_FEATURE_TRANSFER_DST_BIT)),
        "Source texture storage format is unsupported on this device");
    texture.sampling.maxLod = float(levels - 1);
    if (renderTarget) {
        texture.description.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            (depthTarget ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        if (depthTarget) {
            texture.description.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (layout.format == VK_FORMAT_D24_UNORM_S8_UINT || layout.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
                texture.description.aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        texture.gpu = store.context.createImage(texture.description); texture.gpuRevision = texture.revision;
        const auto id = nextTexture.fetch_add(1,std::memory_order_relaxed);
        require(id < uintptr_t(0x7fffffff), "Source texture handle space exhausted");
        store.textures.emplace(SourceTextureHandle(id),std::move(texture));
        return SourceTextureHandle(id);
    }
    // Preflight the entire chain before allocating any CPU pixel storage.
    for (int index = 0; index < levels * int(layers); ++index) {
        const int i = index % levels;
        const size_t bytes = texture.mipBytes(std::max(1,width>>i),std::max(1,height>>i));
        require(bytes <= store.limits.maximumTextureBytes - texture.bytes, "Source texture mip chain exceeds its byte budget");
        texture.bytes += bytes;
    }
    if(texture.bytes > store.limits.maximumShadowBytes - store.counts.shadowBytes)
        throw std::length_error("Source texture CPU shadow budget exhausted: used="+std::to_string(store.counts.shadowBytes)+
            " requested="+std::to_string(texture.bytes)+" textures="+std::to_string(store.textures.size()));
    texture.mips.reserve(size_t(levels) * layers);
    size_t allocated = 0;
    for (int index = 0; index < levels * int(layers); ++index) {
        const int i = index % levels;
        Mip mip;
        mip.width = uint32_t(std::max(1, width >> i)); mip.height = uint32_t(std::max(1, height >> i));
        mip.pixels.resize(texture.mipBytes(mip.width,mip.height));
        allocated += mip.pixels.capacity();
        texture.mips.push_back(std::move(mip));
    }
    require(allocated <= store.limits.maximumTextureBytes && allocated <= store.limits.maximumShadowBytes - store.counts.shadowBytes,
        "Source texture allocation capacity exceeds its byte budget");
    texture.bytes = allocated;
    const auto id = nextTexture.fetch_add(1, std::memory_order_relaxed);
    require(id < uintptr_t(0x7fffffff), "Source texture handle space exhausted");
    store.textures.emplace(SourceTextureHandle(id), std::move(texture));
    store.counts.shadowBytes += allocated;
    return SourceTextureHandle(id);
}
void SourceTextures::destroy(SourceTextureHandle handle) {
    auto& store = *impl_;
    const auto bytes = store.get(handle).bytes;
    store.pendingUploads.erase(handle);
    store.textures.erase(handle);
    store.counts.shadowBytes -= bytes;
}
bool SourceTextures::exists(SourceTextureHandle handle) const { return handle > 0 && impl_->textures.count(handle); }
bool SourceTextures::resident(SourceTextureHandle handle) const {
    if (!exists(handle)) return false;
    const auto& texture = impl_->get(handle);
    return texture.gpu && texture.gpuRevision == texture.revision && texture.complete();
}
SourceTextureInfo SourceTextures::info(SourceTextureHandle handle) const { return impl_->get(handle).info; }
SourceTextureHandle SourceTextures::find(const char* name) const {
    require(name, "Null Source texture name");
    for (const auto& [id, texture] : impl_->textures) if (texture.name == name) return id;
    return 0;
}
void SourceTextures::image(SourceTextureHandle handle, int level, int face, ImageFormat destination, int z,
        int width, int height, ImageFormat source, const void* data, size_t bytes) {
    auto& texture = impl_->get(handle);
    auto& mip = impl_->mip(texture, level, face, z);
    // Source's standard-color initialization sometimes requests BGRX for an
    // already-created RGBA handle. TexImage2D must use the existing storage;
    // channel conversion is determined by the actual source pixels below.
    const bool compatible = destination==texture.info.sourceFormat ||
        (detail::compressedBlockBytes(texture.description.format) && compressedFormat(destination)==texture.description.format) ||
        sourceTextureDataLayout(destination,1,1).format==texture.description.format;
    if (!compatible || width <= 0 || height <= 0 ||
        uint32_t(width) != mip.width || uint32_t(height) != mip.height)
        throw std::invalid_argument("Source TexImage2D '"+texture.name+"' mip="+std::to_string(level)+
            " received="+std::to_string(width)+"x"+std::to_string(height)+" format="+std::to_string(int(destination))+
            " expected="+std::to_string(mip.width)+"x"+std::to_string(mip.height)+" format="+std::to_string(int(texture.info.sourceFormat)));
    if (data) {
        if(detail::compressedBlockBytes(texture.description.format)) {
            require(compressedFormat(source)==texture.description.format && bytes>=mip.pixels.size(),"Compressed Source mip format/size mismatch");
            if(mip.initialized && !std::memcmp(data,mip.pixels.data(),mip.pixels.size()))return;
            std::memcpy(mip.pixels.data(),data,mip.pixels.size());mip.initialized=true;impl_->changed(handle,texture);return;
        }
        auto prepared = prepareSourceTexture(source, mip.width, mip.height, data, bytes);
        require(prepared.description.format == texture.description.format && prepared.pixels.size() == mip.pixels.size(),
            "Source upload cannot be converted to the allocated texture storage format");
        if (mip.initialized && prepared.pixels == mip.pixels) return;
        std::copy(prepared.pixels.begin(), prepared.pixels.end(), mip.pixels.begin());
    } else {
        if (mip.initialized && std::all_of(mip.pixels.begin(), mip.pixels.end(), [](uint8_t byte) { return byte == 0; })) return;
        std::fill(mip.pixels.begin(), mip.pixels.end(), 0);
    }
    mip.initialized = true; impl_->changed(handle,texture);
}
void SourceTextures::subImage(SourceTextureHandle handle, int level, int face, int x, int y, int z,
        int width, int height, ImageFormat source, const void* data, size_t bytes, size_t rowPitch) {
    auto& texture = impl_->get(handle);
    auto& mip = impl_->mip(texture, level, face, z);
    require(x >= 0 && y >= 0 && width > 0 && height > 0 && uint32_t(x) <= mip.width && uint32_t(y) <= mip.height &&
        uint32_t(width) <= mip.width - uint32_t(x) && uint32_t(height) <= mip.height - uint32_t(y),
        "Source texture subimage is outside its allocated mip");
    if(const auto block=detail::compressedBlockBytes(texture.description.format)) {
        require(data && compressedFormat(source)==texture.description.format && x%4==0 && y%4==0 &&
            (width%4==0 || uint32_t(x+width)==mip.width) && (height%4==0 || uint32_t(y+height)==mip.height),
            "Compressed Source updates require complete blocks or mip edges");
        const auto input=sourceTextureDataLayout(source,width,height,rowPitch);
        require(bytes>=input.sourceBytes,"Compressed Source update is truncated");
        const size_t targetPitch=size_t((mip.width+3)/4)*block;
        bool changed=!mip.initialized;
        for(size_t row=0;row<input.rows;++row) {
            auto* target=mip.pixels.data()+(y/4+row)*targetPitch+size_t(x/4)*block;
            const auto* from=static_cast<const uint8_t*>(data)+row*input.rowPitch;
            if(std::memcmp(target,from,input.rowBytes)) {std::memcpy(target,from,input.rowBytes);changed=true;}
        }
        mip.initialized=true;if(changed)impl_->changed(handle,texture);return;
    }
    auto prepared = prepareSourceTexture(source, uint32_t(width), uint32_t(height), data, bytes, rowPitch);
    require(prepared.description.format == texture.description.format, "Source subimage format cannot be converted to texture storage");
    const auto rowBytes = size_t(width) * texture.texelBytes;
    // Dynamic atlases can receive their first texels through a partial upload.
    // Newly allocated CPU storage is zeroed, so untouched pixels are defined.
    bool changed = !mip.initialized;
    for (int row = 0; row < height; ++row) {
        auto* target = mip.pixels.data() + (size_t(y + row) * mip.width + x) * texture.texelBytes;
        const auto* input = prepared.pixels.data() + size_t(row) * rowBytes;
        if (std::memcmp(target, input, rowBytes)) { std::memcpy(target, input, rowBytes); changed = true; }
    }
    mip.initialized=true;
    if (changed) impl_->changed(handle,texture);
}
void SourceTextures::minFilter(SourceTextureHandle handle, int mode) {
    auto& texture = impl_->get(handle);
    require(mode >= SHADER_TEXFILTERMODE_NEAREST && mode <= SHADER_TEXFILTERMODE_ANISOTROPIC,
        "Invalid Source minification filter");
    texture.minAnisotropic=mode==SHADER_TEXFILTERMODE_ANISOTROPIC;
    texture.sampling.minFilter = mode == SHADER_TEXFILTERMODE_NEAREST || mode == SHADER_TEXFILTERMODE_NEAREST_MIPMAP_NEAREST ||
        mode == SHADER_TEXFILTERMODE_NEAREST_MIPMAP_LINEAR ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    texture.sampling.mipFilter = mode >= SHADER_TEXFILTERMODE_NEAREST_MIPMAP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    texture.mipmapped = mode >= SHADER_TEXFILTERMODE_NEAREST_MIPMAP_NEAREST;
    texture.samplers = {};
}
void SourceTextures::magFilter(SourceTextureHandle handle, int mode) {
    require(mode == SHADER_TEXFILTERMODE_NEAREST || mode == SHADER_TEXFILTERMODE_LINEAR || mode == SHADER_TEXFILTERMODE_ANISOTROPIC,
        "Invalid Source magnification filter");
    auto& texture = impl_->get(handle);
    texture.magAnisotropic=mode==SHADER_TEXFILTERMODE_ANISOTROPIC;
    texture.sampling.magFilter = mode == SHADER_TEXFILTERMODE_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    texture.samplers = {};
}
void SourceTextures::setAnisotropicLevel(int level) {
    require(level>=0,"Negative Source anisotropy level");
    const auto& caps=impl_->context.capabilities();
    const int maximum=caps.enabledFeatures.samplerAnisotropy?int(caps.properties.limits.maxSamplerAnisotropy):1;
    impl_->anisotropy=float(std::min(maximum,std::max(level,std::max(2,maximum/2))));
    // Bindings invalidate lazily; changing this setting never walks all textures
    // or mutates samplers retained by in-flight descriptor sets.
}
void SourceTextures::wrap(SourceTextureHandle handle, int coordinate, int mode) {
    require(coordinate >= SHADER_TEXCOORD_S && coordinate <= SHADER_TEXCOORD_U &&
        mode >= SHADER_TEXWRAPMODE_CLAMP && mode <= SHADER_TEXWRAPMODE_BORDER, "Invalid Source texture wrap mode or coordinate");
    const VkSamplerAddressMode modes[] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER};
    auto& texture = impl_->get(handle);
    if (coordinate == SHADER_TEXCOORD_S) texture.sampling.addressU = modes[mode];
    else if (coordinate == SHADER_TEXCOORD_T) texture.sampling.addressV = modes[mode];
    else texture.sampling.addressW = modes[mode];
    texture.samplers = {};
}
SourceTextureBinding SourceTextures::binding(SourceTextureHandle handle, bool srgb, bool noMip, bool point) {
    auto& texture = impl_->get(handle);
    if (!resident(handle))
        throw std::invalid_argument("Source texture '"+texture.name+"' is not resident: initialized="+
            std::to_string(std::count_if(texture.mips.begin(),texture.mips.end(),[](const Mip& mip){return mip.initialized;}))+
            "/"+std::to_string(texture.mips.size())+" revision="+std::to_string(texture.revision)+
            " gpu_revision="+std::to_string(texture.gpuRevision));
    if (!impl_->debugRendering) ++texture.binds;
    auto view = texture.gpu.samplingView(srgb);
    const float anisotropy=(texture.minAnisotropic || texture.magAnisotropic)?impl_->anisotropy:1;
    if(texture.cachedAnisotropy!=anisotropy) {texture.samplers={};texture.cachedAnisotropy=anisotropy;}
    const bool linear = !point && (texture.sampling.minFilter==VK_FILTER_LINEAR || texture.sampling.magFilter==VK_FILTER_LINEAR ||
        (!noMip && texture.mipmapped && texture.sampling.maxLod>0 && texture.sampling.mipFilter==VK_SAMPLER_MIPMAP_MODE_LINEAR));
    const uint8_t viewBit=srgb?2:1;
    if(linear && !(texture.linearViews & viewBit)) {
        ++impl_->counts.filterFormatChecks;
        require(impl_->context.supportsFormat(view.description().format,VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
            "Source sampler requests linear filtering on an unsupported format");
        texture.linearViews|=viewBit;
    }
    auto& sampler = texture.samplers[(noMip ? 1 : 0) | (point ? 2 : 0)];
    if (!sampler) {
        auto description = texture.sampling;
        description.maxAnisotropy=point?1:anisotropy;
        if (noMip || !texture.mipmapped) description.maxLod = 0;
        if (point) { description.minFilter = description.magFilter = VK_FILTER_NEAREST; description.mipFilter = VK_SAMPLER_MIPMAP_MODE_NEAREST; }
        sampler = impl_->graphics.sampler(description);
    }
    return {view, sampler};
}
Image SourceTextures::renderImage(SourceTextureHandle handle) {
    auto& texture = impl_->get(handle);
    require(texture.info.renderTarget, "Source texture is not a render target");
    if (!texture.gpu) { texture.gpu=impl_->context.createImage(texture.description); texture.gpuRevision=texture.revision; }
    return texture.gpu;
}
void SourceTextures::flushUploads(SourceDevice& device) {
    if(impl_->pendingUploads.empty()) {device.flushUploads();return;}
    struct Pending { SourceTextureHandle handle;Texture* texture;Image image;uint64_t revision; };
    std::vector<Pending> pending;
    for(auto it=impl_->pendingUploads.begin();it!=impl_->pendingUploads.end();) {
        auto& texture=impl_->get(*it);++impl_->counts.uploadInspections;
        if (!texture.info.renderTarget && texture.complete() && texture.gpuRevision != texture.revision) {
            pending.push_back({*it,&texture,{},texture.revision});++it;
        } else it=impl_->pendingUploads.erase(it);
    }
    if (pending.empty()) { device.flushUploads(); return; }
    // Commit only after SourceDevice's combined buffer/image submission succeeds.
    device.flushUploads([&](UploadBatch& batch) {
        for (auto& upload : pending) {
            upload.image = impl_->context.createImage(upload.texture->description);
            for (uint32_t index = 0; index < upload.texture->mips.size(); ++index) {
                const uint32_t level = index % upload.texture->info.mipLevels, face = index / upload.texture->info.mipLevels;
                const auto& mip = upload.texture->mips[index];
                ImageUse after;
                after.access = VK_ACCESS_SHADER_READ_BIT;
                after.stages = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
                after.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                batch.image(upload.image, level, face, mip.pixels.data(), mip.pixels.size(), {}, after);
            }
        }
    });
    for (auto& upload : pending) {
        upload.texture->gpu = std::move(upload.image);
        upload.texture->gpuRevision = upload.revision;
        if(upload.texture->revision==upload.revision)impl_->pendingUploads.erase(upload.handle);
        ++impl_->counts.imageUploads; impl_->counts.mipUploads += upload.texture->mips.size();
    }
}
void SourceTextures::releaseResources(bool managed) {
    for (auto& [id, texture] : impl_->textures) if (managed || !(texture.flags & TEXTURE_CREATE_MANAGED)) {
        texture.gpu = {}; texture.gpuRevision = 0;
        if(!texture.info.renderTarget && texture.complete())impl_->pendingUploads.insert(id);
    }
}
void SourceTextures::evictManagedResources() {
    // D3D's managed-pool eviction does not discard default-pool render targets.
    // Panorama may still need to composite their contents after a map reload.
    for (auto& [id,texture]:impl_->textures) if (!texture.info.renderTarget && (texture.flags & TEXTURE_CREATE_MANAGED)) {
        texture.gpu={}; texture.gpuRevision=0;
        if(texture.complete())impl_->pendingUploads.insert(id);
    }
}
SourceTextureStatistics SourceTextures::statistics() const {
    auto counts = impl_->counts; counts.textures = impl_->textures.size(); return counts;
}
void SourceTextures::finishFrame() {
    for (auto& [id, texture] : impl_->textures) {
        texture.lastBinds = texture.binds;
        texture.maximumBinds = std::max(texture.maximumBinds, texture.binds);
        texture.binds = 0;
    }
}
void SourceTextures::debugRendering(bool enabled) { impl_->debugRendering = enabled; }
std::vector<SourceTextureDebugInfo> SourceTextures::debugInfo() const {
    std::vector<SourceTextureDebugInfo> result;
    result.reserve(impl_->textures.size());
    for (const auto& [id, texture] : impl_->textures) {
        SourceTextureDebugInfo value;
        value.name=texture.name; value.group=texture.group; value.info=texture.info;
        value.binds=texture.lastBinds; value.maximumBinds=texture.maximumBinds;
        value.resident=bool(texture.gpu);
        if (value.resident) {
            if (texture.info.renderTarget) {
                const auto texelBytes = texture.info.depthTarget ?
                    (texture.description.format == VK_FORMAT_D16_UNORM ? 2u :
                     texture.description.format == VK_FORMAT_D32_SFLOAT_S8_UINT ? 8u : 4u) : texture.texelBytes;
                value.bytes=size_t(texture.info.width)*texture.info.height*texelBytes;
                value.picmip1Bytes=value.picmip2Bytes=value.bytes;
            } else for (size_t i=0;i<texture.mips.size();++i) {
                const auto bytes=texture.mips[i].pixels.size();
                value.bytes+=bytes;
                const auto level=i%texture.info.mipLevels;
                if (level>=std::min(1u,texture.info.mipLevels-1)) value.picmip1Bytes+=bytes;
                if (level>=std::min(2u,texture.info.mipLevels-1)) value.picmip2Bytes+=bytes;
            }
        }
        result.push_back(std::move(value));
    }
    return result;
}
} // namespace sourcevk
