#include "vulkan_api.h"
#include "vulkan_internal.h"

#include "vulkan_platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include "../shaderapidx9/meshbase.h"
#include "shaderapi/ishadershadow.h"
#include "shaderapi/ishaderutil.h"
#include "shaderapi/gpumemorystats.h"
#include "pixelwriter.h"
#include "vtf/vtf.h"
#include "vulkan_state.h"

namespace sourcevk {
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::invalid_argument(message); }
std::string shaderName(std::string name) {
    require(!name.empty() && name.size() <= 96, "Invalid Source shader input contract name");
    for (auto& c : name) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_', "Invalid Source shader identifier");
    }
    return name;
}
template<class T> void keyPart(std::string& key, T value) { key.append(reinterpret_cast<const char*>(&value), sizeof(value)); }
VkPrimitiveTopology topology(MaterialPrimitiveType_t type, int count) {
    require(count >= 0, "Negative Source primitive count");
    switch (type) {
    case MATERIAL_TRIANGLES: require(count % 3 == 0, "Incomplete Source triangle"); return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case MATERIAL_TRIANGLE_STRIP: require(!count || count >= 3, "Incomplete Source triangle strip"); return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case MATERIAL_LINES: require(count % 2 == 0, "Incomplete Source line"); return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    default: detail::unsupportedSourceAPI("Draw primitive type");
    }
}
} // namespace

struct SourceAPI::Impl final : detail::SourceStateAPI {
    using SourceStateAPI::constants;
    using SourceStateAPI::shaderUtil;
    Context& context;
    GraphicsDevice& graphics;
    SourceDevice& device;
    SourceShadow& shadow;
    SourceShaderLibrary& shaders;
    SourceAPILimits limits;
    SourceTextures textures;
    SourceQueries queries;
    ShaderAPIOcclusionQuery_t skippedQuery=nullptr;
    DescriptorArena descriptors;
    SourceAPIStatistics counts;
    std::map<StateSnapshot_t, SourceSnapshot> snapshots;
    int nextSnapshot = 0;
    SourceSnapshot selected;
    std::map<std::string, std::vector<SourceShaderInput>> shaderInputs;
    std::set<std::string> dynamicTextureReadShaders;
    struct Layout { DescriptorLayout descriptors; PipelineLayout pipeline; };
    std::map<uint32_t, Layout> layouts;
    std::map<std::string, DescriptorSet> cachedSets;
    struct TextureState { SourceTextureHandle handle = 0; uint32_t flags = 0; bool point = false; };
    std::array<TextureState, 16> pixelTextures;
    std::array<TextureState, 4> vertexTextures;
    std::array<SourceTextureHandle, TEXTURE_MAX_STD_TEXTURES> standardTextures {};
    SourceTextureHandle modifying = 0;
    std::array<SourceTextureHandle,TEXTURE_MAX_STD_TEXTURES> fallbackTextures {};
    struct TextureLock {
        SourceTextureHandle texture = 0;
        int level=0, face=0, x=0, y=0, width=0, height=0;
        ImageFormat format=IMAGE_FORMAT_UNKNOWN;
        size_t pitch=0;
        std::vector<uint8_t> pixels;
    } textureLock;
    Frame currentFrame;
    bool active = false, presentRecorded = false;
    RenderTarget* target = nullptr;
    Image backColor, backDepth;
    std::map<std::pair<VkImageView,VkImageView>,std::unique_ptr<RenderTarget>> engineTargets;
    ShaderAPITextureHandle_t colorTarget = SHADER_RENDERTARGET_BACKBUFFER, depthTarget = SHADER_RENDERTARGET_DEPTHBUFFER;
    ShaderAPITextureHandle_t fullScreenTexture = INVALID_SHADERAPI_TEXTURE_HANDLE;
    DescriptorLayout presentDescriptors;
    PipelineLayout presentLayout;
    Sampler presentSampler;
    std::unique_ptr<DrawEncoder> encoder;
    VkExtent2D extent {};
    VkFormat color = VK_FORMAT_UNDEFINED, depth = VK_FORMAT_UNDEFINED;
    ShaderViewport_t viewport;
    VkRect2D scissor {};
    bool scissorEnabled = false;
    std::array<float, 4> clearColor {0,0,0,1};
    uint32_t vertexDynamic = 0, pixelDynamic = 0;
    VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE;
    bool noCulling=false,depthOverride=false,depthWriteOverride=false,depthTestOverride=false,depthEquals=false;
    bool alphaOverride=false,alphaWriteOverride=false,colorOverride=false,colorWriteOverride=false;
    ShaderStencilState_t stencilState;
    void* modeWindow=nullptr;
    IMaterial* material = nullptr;
    bool traceDraws=std::getenv("SOURCE_VULKAN_TRACE_DRAWS")!=nullptr;
    std::set<IMaterial*> tracedMaterials;
    std::function<void(IMaterial*, const SourceMeshDraw&)> materialPass;
    const SourceMeshDraw* pendingMesh = nullptr;
    const MeshInstanceData_t* currentInstance = nullptr;
    struct BoundVertex {
        IVertexBuffer* buffer = nullptr;
        uint32_t offset = 0, first = 0, count = 0;
        VertexFormat_t format = 0;
    } boundVertex;
    IIndexBuffer* boundIndex = nullptr;
    uint32_t boundIndexOffset = 0;
    double gameTime = 0;
    bool gameTimeSet = false;
    bool singlePassFlashlight = false;

    Impl(Context& ctx, GraphicsDevice& gfx, FrameArena& arena, SourceDevice& dev, SourceShadow& shadows,
            SourceShaderLibrary& library, SourceAPILimits budget)
        : SourceStateAPI(ctx, arena), context(ctx), graphics(gfx), device(dev), shadow(shadows), shaders(library), limits(budget),
          textures(ctx, gfx, budget.textures), queries(ctx,budget.maximumOcclusionQueries), descriptors(ctx) {
        require(limits.maximumSnapshots && limits.maximumSnapshots <= 32768 && limits.maximumShaderInputs && limits.maximumLayouts,
            "Invalid Source API budgets");
        viewport.Init();
        int width, height;
        device.interface().GetBackBufferDimensions(width,height);
        extent = {uint32_t(std::max(1,width)),uint32_t(std::max(1,height))};
        viewport.Init(0,0,int(extent.width),int(extent.height));
        device.attachAPI(this, [&](bool managed) { cachedSets.clear(); textures.releaseResources(managed); });
        try { device.setDrawSink([&](const SourceMeshDraw& draw) { meshDraw(draw); }); }
        catch (...) { device.detachAPI(this); throw; }
    }
    ~Impl() { device.detachAPI(this); }
    const SourceSnapshot& snapshot(StateSnapshot_t id) const {
        const auto found = snapshots.find(id);
        require(id >= 0 && found != snapshots.end(), "Foreign or cleared Source material snapshot handle");
        return found->second;
    }
    void configureTarget(VkExtent2D size, VkFormat colorFormat, VkFormat depthFormat, bool resetViewport) {
        extent = size; color = colorFormat; depth = depthFormat;
        if (resetViewport) {
            viewport.Init(0, 0, int(size.width), int(size.height));
            scissor = {{0,0}, size}; scissorEnabled = false;
        }
        encoder = std::make_unique<DrawEncoder>(context, currentFrame, size, colorFormat, depthFormat);
    }
    void ensurePass() {
        require(active, "Source draw/clear requires BeginFrame");
        detail::Access::validate(context, currentFrame);
        if (shaderUtil) {
            const bool srgb = selected && selected.state().srgbWrite;
            Image image = colorTarget == SHADER_RENDERTARGET_BACKBUFFER ? backColor : textures.renderImage(colorTarget);
            if (image.description().srgbViews) image = image.samplingView(srgb);
            Image z;
            if (depthTarget == SHADER_RENDERTARGET_DEPTHBUFFER) z = backDepth;
            else if (depthTarget != SHADER_RENDERTARGET_NONE) z = textures.renderImage(depthTarget);
            const auto key = std::make_pair(image.view(),z.view());
            auto found = engineTargets.find(key);
            if (found == engineTargets.end()) found = engineTargets.emplace(key,std::make_unique<RenderTarget>(context,image,z)).first;
            if (encoder && target == found->second.get()) return;
            closePass();
            target = found->second.get();
            queries.preparePass(currentFrame);
            target->begin(currentFrame,clearColor);
            configureTarget(target->extent(),image.description().format,z ? z.description().format : VK_FORMAT_UNDEFINED,false);
            return;
        }
        if (encoder) return;
        require(!presentRecorded, "Source present pass has already ended");
        queries.preparePass(currentFrame);
        context.beginPresentPass(currentFrame, clearColor);
        configureTarget(currentFrame.extent, currentFrame.colorFormat, VK_FORMAT_UNDEFINED, false);
    }
    void closePass() {
        if (!encoder) return;
        queries.endPass(currentFrame);
        encoder.reset();
        if (target) { target->end(currentFrame); target = nullptr; }
        else { context.endPresentPass(currentFrame); presentRecorded = true; }
    }
    bool startFrame() {
        require(!active && !device.frameActive(), "Present the previous Source frame before beginning another");
        if (!device.interface().IsUsingGraphics()) return false;
        textures.flushUploads(device);
        if (!device.beginFrame(currentFrame)) return false;
        active = true; presentRecorded = false; target = nullptr;
        extent = currentFrame.extent; color = currentFrame.colorFormat; depth = VK_FORMAT_UNDEFINED;
        if (shaderUtil) {
            int width,height;device.interface().GetBackBufferDimensions(width,height);
            extent = {uint32_t(width),uint32_t(height)};
            if (!backColor || backColor.description().width != extent.width || backColor.description().height != extent.height) {
                engineTargets.clear();
                ImageDescription image;
                image.width=extent.width;image.height=extent.height;image.srgbViews=true;
                image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                backColor=context.createImage(image);
                image.srgbViews=false;image.format=VK_FORMAT_D24_UNORM_S8_UINT;
                if(!context.supportsFormat(image.format,VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))image.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
                image.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                image.aspect=VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                backDepth=context.createImage(image);
            }
            colorTarget=SHADER_RENDERTARGET_BACKBUFFER;depthTarget=SHADER_RENDERTARGET_DEPTHBUFFER;
            color=backColor.description().format;depth=backDepth.description().format;
        }
        viewport.Init(0, 0, int(extent.width), int(extent.height));
        scissor = {{0,0}, extent}; scissorEnabled = false;
        cachedSets.clear();
        return true;
    }
    bool ensureEngineFrame() {
        if (active) return true;
        require(shaderUtil,"Source drawing requires BeginFrame");
        const auto savedColor=colorTarget, savedDepth=depthTarget;
        const auto savedViewport=viewport;
        const auto savedScissor=scissor; const bool savedScissorEnabled=scissorEnabled;
        if (!startFrame()) return false;
        SetRenderTarget(savedColor,savedDepth);
        viewport=savedViewport; scissor=savedScissor; scissorEnabled=savedScissorEnabled;
        return true;
    }
    void presentBackbuffer() {
        if (!presentLayout) {
            presentDescriptors=graphics.descriptorLayout({
                {0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT},
                {1,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT}});
            presentLayout=graphics.pipelineLayout({presentDescriptors},sizeof(float)*4);
            presentSampler=graphics.sampler();
        }
        context.beginPresentPass(currentFrame,clearColor);
        DrawEncoder draw(context,currentFrame);
        GraphicsPipelineDescription pipeline;
        pipeline.layout=presentLayout;pipeline.renderPass=currentFrame.renderPass;pipeline.colorFormat=currentFrame.colorFormat;
        pipeline.vertex=shaders.shader(SourceShaderStage::Vertex,{"native_present_vs",0});
        const bool srgb=currentFrame.colorFormat==VK_FORMAT_B8G8R8A8_SRGB || currentFrame.colorFormat==VK_FORMAT_R8G8B8A8_SRGB;
        pipeline.fragment=shaders.shader(SourceShaderStage::Pixel,{"native_present_ps",0},srgb?1:0);
        pipeline.depthTest=pipeline.depthWrite=false;pipeline.cull=VK_CULL_MODE_NONE;
        draw.pipeline(graphics.pipeline(pipeline));
        DescriptorWrite image;image.binding=0;image.type=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;image.image=backColor.samplingView(false);
        DescriptorWrite sampler;sampler.binding=1;sampler.type=VK_DESCRIPTOR_TYPE_SAMPLER;sampler.sampler=presentSampler;
        draw.descriptors(0,descriptors.allocate(currentFrame,presentDescriptors,{image,sampler}));
        const auto gamma = device.outputGamma();
        draw.pushConstants(gamma.data(),sizeof(gamma));
        draw.draw(3);
        context.endPresentPass(currentFrame);presentRecorded=true;
    }
    void startTarget(RenderTarget& value, const std::array<float,4>& clear, float depthClear) {
        require(active && !presentRecorded && (!encoder || target), "Offscreen Source targets must precede the present pass");
        require(std::isfinite(depthClear) && depthClear >= 0 && depthClear <= 1, "Invalid Source depth clear value");
        closePass();
        queries.preparePass(currentFrame);
        value.begin(currentFrame, clear, depthClear);
        target = &value;
        configureTarget(value.extent(), value.color().description().format,
            value.depth() ? value.depth().description().format : VK_FORMAT_UNDEFINED, true);
    }
    Layout& layout(const SourceMaterialState& state) {
        const uint32_t key = state.textures | (state.vertexTextures << 16);
        const auto found = layouts.find(key);
        if (found != layouts.end()) return found->second;
        require(layouts.size() < limits.maximumLayouts, "Source material descriptor layout budget exhausted");
        std::vector<DescriptorBinding> bindings {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT}
        };
        auto add = [&](uint32_t mask, uint32_t count, uint32_t base, VkShaderStageFlags stage) {
            for (uint32_t i = 0; i < count; ++i) if (mask & (1u << i)) {
                bindings.push_back({base + 2*i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stage});
                bindings.push_back({base + 2*i + 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stage});
            }
        };
        add(state.textures, 16, 2, VK_SHADER_STAGE_FRAGMENT_BIT);
        add(state.vertexTextures, 4, 34, VK_SHADER_STAGE_VERTEX_BIT);
        Layout value;
        value.descriptors = graphics.descriptorLayout(std::move(bindings));
        value.pipeline = graphics.pipelineLayout({value.descriptors}, sizeof(SourceDrawConstants));
        return layouts.emplace(key, std::move(value)).first->second;
    }
    SourceTextureHandle constantTexture(StandardTextureId_t id) {
        require(id>=TEXTURE_WHITE && id<=TEXTURE_SSBUMP_FLAT,"No constant fallback exists for this standard texture");
        auto& handle=fallbackTextures[size_t(id)];
        if (!handle) {
            std::array<uint8_t,4> pixel {255,255,255,255};
            if (id==TEXTURE_BLACK || id==TEXTURE_BLACK_ALPHA_ZERO) pixel={0,0,0,255};
            if (id==TEXTURE_GREY || id==TEXTURE_GREY_ALPHA_ZERO) pixel={128,128,128,255};
            if (id==TEXTURE_BLACK_ALPHA_ZERO || id==TEXTURE_GREY_ALPHA_ZERO) pixel[3]=0;
            if (id==TEXTURE_NORMALMAP_FLAT) pixel={128,128,255,255};
            if (id==TEXTURE_SSBUMP_FLAT) pixel={147,147,147,255};
            const auto name="[Vulkan constant "+std::to_string(int(id))+"]";
            handle=textures.create(1,1,1,IMAGE_FORMAT_RGBA8888,1,1,TEXTURE_CREATE_MANAGED,name.c_str(),"Vulkan internal");
            textures.image(handle,0,0,IMAGE_FORMAT_RGBA8888,0,1,1,IMAGE_FORMAT_RGBA8888,pixel.data(),pixel.size());
        }
        return handle;
    }
    void bindResources(Layout& bindings) {
        textures.flushUploads(device);
        const auto vertex = constants.snapshot(SourceShaderStage::Vertex, currentFrame);
        const auto pixel = constants.snapshot(SourceShaderStage::Pixel, currentFrame);
        std::vector<DescriptorWrite> writes {
            {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, {vertex.buffer, 0, vertex.size}, {}, {}},
            {1, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, {pixel.buffer, 0, pixel.size}, {}, {}}
        };
        std::string key;
        keyPart(key, bindings.descriptors.handle());
        keyPart(key, vertex.buffer.handle()); keyPart(key, pixel.buffer.handle());
        const auto& state = selected.state();
        const bool dynamicReads=dynamicTextureReadShaders.count(state.vertex.name)!=0;
        auto add = [&](const auto& texturesByStage, uint32_t mask, uint32_t base, SourceShaderStage stage) {
            for (uint32_t i = 0; i < texturesByStage.size(); ++i) if (mask & (1u << i)) {
                auto texture = texturesByStage[i];
                if(dynamicReads && !texture.handle && shaderUtil) {
                    texture.handle=shaderUtil->GetStandardTexture(TEXTURE_WHITE);
                    if (!textures.exists(texture.handle)) {
                        // Panorama can draw before the first map allocates the
                        // engine's standard textures; unused optional samplers
                        // still need complete descriptors on Vulkan 1.1.
                        texture.handle=constantTexture(TEXTURE_WHITE); textures.flushUploads(device);
                    }
                }
                const bool srgb = texture.flags & uint32_t(TEXTURE_BINDFLAGS_SRGBREAD);
                auto resource = textures.binding(texture.handle, srgb,
                    texture.flags & uint32_t(TEXTURE_BINDFLAGS_NOMIP), texture.point);
                selected.validateTexture(stage, i, resource.image,dynamicReads);
                keyPart(key, resource.image.view()); keyPart(key, resource.sampler.handle());
                DescriptorWrite imageWrite;
                imageWrite.binding = base + 2*i; imageWrite.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; imageWrite.image = resource.image;
                writes.push_back(std::move(imageWrite));
                DescriptorWrite samplerWrite;
                samplerWrite.binding = base + 2*i + 1; samplerWrite.type = VK_DESCRIPTOR_TYPE_SAMPLER; samplerWrite.sampler = resource.sampler;
                writes.push_back(std::move(samplerWrite));
            }
        };
        add(pixelTextures, state.textures, 2, SourceShaderStage::Pixel);
        add(vertexTextures, state.vertexTextures, 34, SourceShaderStage::Vertex);
        auto found = cachedSets.find(key);
        if (found == cachedSets.end()) {
            auto set = descriptors.allocate(currentFrame, bindings.descriptors, writes);
            found = cachedSets.emplace(std::move(key), std::move(set)).first; ++counts.descriptorSets;
        } else ++counts.descriptorHits;
        encoder->descriptors(0, found->second, {uint32_t(vertex.offset), uint32_t(pixel.offset)});
    }
    void encode(const BufferSlice& vertices, const BufferSlice& indices, VkIndexType indexType, uint64_t format,
            VkPrimitiveTopology primitive, uint32_t firstIndex, uint32_t indexCount, int32_t firstVertex,
            const std::array<float,4>& modulation, const IVertexBuffer* colors=nullptr, uint32_t colorOffset=0) {
        require(active && selected, "Source draw requires an active frame and material snapshot");
        selected.validateVertexFormat(format);
        const auto name=shaderName(selected.state().vertex.name);
        auto inputs = shaderInputs.find(name+":"+std::to_string(selected.state().vertex.staticIndex)+":"+std::to_string(vertexDynamic));
        if(inputs==shaderInputs.end())inputs=shaderInputs.find(name);
        require(inputs != shaderInputs.end(), "Source vertex shader has no registered semantic/binding contract");
        const auto vertexLayout = sourceVertexLayout(format);
        std::vector<SourceShaderInput> primaryInputs;
        std::vector<VkVertexInputAttributeDescription> attributes;
        BufferSlice colorSlice;
        uint32_t colorStride=0;
        for (const auto& input:inputs->second) {
            if (input.semantic!=SourceSemantic::Color || !input.index) { primaryInputs.push_back(input); continue; }
            require(colors && input.index<=3,"Source material requires a baked-lighting color stream");
            if (!colorSlice.buffer) {
                colorSlice=device.vertexSlice(colors,currentFrame);
                colorStride=sourceVertexLayout(device.vertexFormat(colors)).stride;
                require((colorStride==4 || colorStride==12) && colorOffset<colorSlice.size,"Invalid Source baked-lighting stream");
                colorSlice.offset+=colorOffset; colorSlice.size-=colorOffset;
            }
            attributes.push_back({input.location,1,VK_FORMAT_B8G8R8A8_UNORM,colorStride==4?0u:4*(input.index-1)});
        }
        auto primary=vertexLayout.attributes(primaryInputs);
        attributes.insert(attributes.end(),primary.begin(),primary.end());
        ensurePass();
        auto& bindings = layout(selected.state());
        GraphicsPipelineDescription description;
        description.layout = bindings.pipeline;
        description.renderPass = target ? target->renderPass() : currentFrame.renderPass;
        description.colorFormat = color; description.depthFormat = depth; description.frontFace = frontFace;
        description.topology = primitive;
        description.overrideCull=noCulling;description.cull=VK_CULL_MODE_NONE;
        description.overrideDepth=depthOverride;description.depthWrite=depthWriteOverride;description.depthTest=depthTestOverride;
        description.overrideDepthCompare=depthEquals;description.depthCompare=VK_COMPARE_OP_EQUAL;
        description.overrideAlpha=alphaOverride;description.overrideColor=colorOverride;
        description.colorWriteMask=(alphaWriteOverride?VK_COLOR_COMPONENT_A_BIT:0) |
            (colorWriteOverride?VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT:0);
        if(stencilState.m_bEnable) {
            description.stencilTest=true;
            description.stencil={VkStencilOp(stencilState.m_FailOp-1),VkStencilOp(stencilState.m_PassOp-1),
                VkStencilOp(stencilState.m_ZFailOp-1),VkCompareOp(stencilState.m_CompareFunc-1),
                stencilState.m_nTestMask,stencilState.m_nWriteMask,uint32_t(stencilState.m_nReferenceValue)};
        }
        description.vertexBindings = {{0, vertexLayout.stride, VK_VERTEX_INPUT_RATE_VERTEX}};
        if (colorSlice.buffer) description.vertexBindings.push_back({1,colorStride,VK_VERTEX_INPUT_RATE_VERTEX});
        description.attributes = std::move(attributes);
        encoder->pipeline(shaders.pipeline(selected, std::move(description), vertexDynamic, pixelDynamic));
        bindResources(bindings);
        encoder->vertexBuffer(0, vertices); encoder->indexBuffer(indices, indexType);
        if (colorSlice.buffer) encoder->vertexBuffer(1,colorSlice);
        encoder->viewport({float(viewport.m_nTopLeftX), float(viewport.m_nTopLeftY), float(viewport.m_nWidth),
            float(viewport.m_nHeight), viewport.m_flMinZ, viewport.m_flMaxZ});
        encoder->scissor(scissorEnabled ? scissor : VkRect2D{{0,0}, extent});
        SourceDrawConstants draw {selected.state().alpha, modulation};
        encoder->pushConstants(&draw, sizeof(draw));
        queries.beginDraw(currentFrame);
        encoder->drawIndexed(indexCount, 1, firstIndex, firstVertex);
        ++counts.draws;
    }
    void renderMesh(const SourceMeshDraw& draw) {
        if (shaderUtil) commitTransforms();
        const auto* vertices=draw.vertices?draw.vertices:static_cast<IVertexBuffer*>(draw.mesh);
        const auto* indices=draw.indices?draw.indices:static_cast<IIndexBuffer*>(draw.mesh);
        auto vertex=device.vertexSlice(vertices,currentFrame);
        require(draw.vertexOffset<vertex.size,"Source instance vertex offset is out of bounds");
        vertex.offset+=draw.vertexOffset; vertex.size-=draw.vertexOffset;
        encode(vertex, device.indexSlice(indices, currentFrame),device.indexType(indices),device.vertexFormat(vertices),
            draw.topology, draw.firstIndex,draw.indexCount,0,draw.modulation,draw.colors,draw.colorOffset);
    }
    void meshDraw(const SourceMeshDraw& draw) {
        if (shaderUtil && !ensureEngineFrame()) return;
        require(active && !pendingMesh, "Source material draw is outside a frame or recursively entered");
        staticLighting=draw.colors!=nullptr;
        if (!material) { renderMesh(draw); return; }
        require(bool(materialPass), "Source IMaterialInternal draw callback is not installed");
        pendingMesh = &draw; currentInstance=draw.instance;
        try {
            materialPass(material, draw);
            if(traceDraws && tracedMaterials.size()<512 && tracedMaterials.insert(material).second) {
                std::fprintf(stderr,"VK_DRAW_TRACE: material=%s shader=%s/%u pixel=%s/%u colors=%d offset=%u ambient=%g,%g,%g lights=%d target=%lld viewport=%d,%d,%d,%d\n",
                    material->GetName(),selected?selected.state().vertex.name.c_str():"",vertexDynamic,
                    selected?selected.state().pixel.name.c_str():"",pixelDynamic,draw.colors!=nullptr,
                    draw.colorOffset,ambient[0].x,ambient[0].y,ambient[0].z,lighting.m_nLocalLightCount,
                    colorTarget,viewport.m_nTopLeftX,viewport.m_nTopLeftY,viewport.m_nWidth,viewport.m_nHeight);
                if(draw.colors)device.traceVertexData(draw.colors,draw.colorOffset);
                if(selected && selected.state().vertex.name.rfind("panorama",0)==0)
                    device.traceVertexData(draw.vertices?draw.vertices:static_cast<IVertexBuffer*>(draw.mesh),draw.vertexOffset);
            }
        }
        catch (const std::exception& error) {
            if (shaderUtil) std::fprintf(stderr,"VK_MATERIAL_FAIL: %s vertex=%s/%u pixel=%s/%u: %s\n",
                material->GetName(),selected?selected.state().vertex.name.c_str():"",vertexDynamic,
                selected?selected.state().pixel.name.c_str():"",pixelDynamic,error.what());
            pendingMesh=nullptr; currentInstance=nullptr; throw;
        }
        catch (...) { pendingMesh = nullptr; currentInstance=nullptr; throw; }
        pendingMesh = nullptr; currentInstance=nullptr;
    }

    void BeginFrame() override {
        if (shaderUtil) { if (!active) startFrame(); }
        else require(startFrame(), "Source surface is temporarily unavailable; use the frame-availability bridge");
    }
    bool SetMode(void* window, int adapter, const ShaderDeviceInfo_t& mode) override {
        if(!device.manager().SetMode(window, adapter, mode))return false;
        modeWindow=window;extent={uint32_t(mode.m_DisplayMode.m_nWidth),uint32_t(mode.m_DisplayMode.m_nHeight)};
        return true;
    }
    void ChangeVideoMode(const ShaderDeviceInfo_t& mode) override { require(SetMode(modeWindow,0,mode),"Source Vulkan mode change failed"); }
    void EndFrame() override {
        if (shaderUtil && !active) return;
        require(active && !pendingMesh && !queries.active(), "Source EndFrame has no frame or an unfinished draw/query");
        if (shaderUtil) {
            if (!detail::Access::allocation(backColor)->attachmentDefined) {
                SetRenderTarget(SHADER_RENDERTARGET_BACKBUFFER,SHADER_RENDERTARGET_DEPTHBUFFER);
                ensurePass();
            }
            closePass();presentBackbuffer();
        } else {
            closePass();
            if (!presentRecorded) { ensurePass(); closePass(); }
        }
        textures.finishFrame();
        active = false;
    }
    void OnPresent() override {
        if (shaderUtil) {
            // The startup graphic and loading plaques draw/SwapBuffers without
            // a material-system BeginFrame/EndFrame pair.
            if (!active && !device.frameActive() && !startFrame()) return;
            if (active) EndFrame();
        } else require(!active, "End the Source API frame before presenting");
    }
    void SetDefaultState() override {
        // MaterialSystem calls this before every material draw. It only resets
        // shader-local defaults; context culling, stencil, scissors and write
        // overrides must survive until their owner changes them.
        MatrixMode(MATERIAL_MODEL);vertexDynamic=pixelDynamic=0;
    }
    void ResetRenderState(bool full, bool panorama) override {
        SetDefaultState();frontFace=VK_FRONT_FACE_CLOCKWISE;
        noCulling=depthOverride=depthEquals=alphaOverride=colorOverride=false;stencilState=ShaderStencilState_t();
        pixelTextures = {}; vertexTextures = {}; boundVertex = {}; boundIndex = nullptr;
        scissorEnabled = false;
        if (full) selected = {};
    }
    StateSnapshot_t TakeSnapshot() override {
        auto value = shadow.snapshot();
        for (const auto& [id, previous] : snapshots) if (previous == value) return id;
        require(snapshots.size() < limits.maximumSnapshots && nextSnapshot < 32768, "Source short snapshot handle space exhausted");
        const auto id = StateSnapshot_t(nextSnapshot++);
        snapshots.emplace(id, std::move(value)); return id;
    }
    void ClearSnapshots() override {
        require(!pendingMesh, "Cannot clear snapshots inside a Source material pass");
        snapshots.clear(); selected = {}; shadow.clearSnapshots();
        // Do not recycle short handles: a stale material must fail after unload.
    }
    bool IsTranslucent(StateSnapshot_t id) const override { return snapshot(id).translucent(); }
    bool IsAlphaTested(StateSnapshot_t id) const override { return snapshot(id).state().alpha.enabled; }
    bool UsesVertexAndPixelShaders(StateSnapshot_t id) const override {
        const auto& value = snapshot(id).state(); return !value.vertex.name.empty() && !value.pixel.name.empty();
    }
    bool IsDepthWriteEnabled(StateSnapshot_t id) const override { return snapshot(id).state().depthWrite; }
    int CompareSnapshots(StateSnapshot_t a, StateSnapshot_t b) override { snapshot(a); snapshot(b); return a < b ? -1 : a > b ? 1 : 0; }
    VertexFormat_t ComputeVertexUsage(int count, StateSnapshot_t* ids) const override {
        require(count > 0 && ids, "Source vertex usage requires material snapshots");
        VertexFormat_t fields = 0;
        int weights = 0, userData = 0;
        std::array<int, VERTEX_MAX_TEXTURE_COORDINATES> uv {};
        for (int i = 0; i < count; ++i) {
            const auto format = snapshot(ids[i]).state().vertexUsage;
            fields |= format & ((VertexFormat_t(1) << VERTEX_BONE_WEIGHT_BIT) - 1);
            weights = std::max(weights, NumBoneWeights(format)); userData = std::max(userData, UserDataSize(format));
            for (int j = 0; j < VERTEX_MAX_TEXTURE_COORDINATES; ++j) uv[j] = std::max(uv[j], TexCoordSize(j, format));
        }
        fields |= VERTEX_BONEWEIGHT(weights) | VERTEX_USERDATA_SIZE(userData);
        for (int j = 0; j < VERTEX_MAX_TEXTURE_COORDINATES; ++j) fields |= VERTEX_TEXCOORD_SIZE(j, uv[j]);
        sourceVertexLayout(fields); return fields;
    }
    VertexFormat_t ComputeVertexFormat(int count, StateSnapshot_t* ids) const override { return ComputeVertexUsage(count, ids); }
    void BeginPass(StateSnapshot_t id) override { selected = snapshot(id); }
    void SetPixelShaderFogParams(int index) override {
        if(selected && selected.state().fogMode!=SHADER_FOGMODE_DISABLED)SourceStateAPI::SetPixelShaderFogParams(index);
        else {
            const float disabled[4]={0,-FLT_MAX,0,0};constants.floats(SourceShaderStage::Pixel,index,disabled,1);
        }
    }
    void RenderPass(const unsigned char* commands, int pass, int count) override {
        require(pendingMesh && pass >= 0 && pass < count, "Source RenderPass requires a material draw callback and valid pass index");
        executeInstanceCommands(commands,pendingMesh->modulation);
        renderMesh(*pendingMesh);
    }
    void Bind(IMaterial* value) override {
        require(!value || bool(materialPass), "Install the Source material pass callback before binding IMaterial"); material = value;
    }
    IMesh* GetDynamicMeshEx(IMaterial* value, VertexFormat_t format, int bones, bool, IMesh* vertices, IMesh* indices) override {
        SetNumBoneWeights(bones);
        if (!format) format=value?value->GetVertexFormat():material?material->GetVertexFormat():selected?selected.state().vertexUsage:0;
        format &= ~VERTEX_FORMAT_COMPRESSED;
        if (bones) format=(format & ~VERTEX_BONE_WEIGHT_MASK) | VERTEX_BONEWEIGHT(2) | VERTEX_BONE_INDEX;
        if (value) Bind(value);
        return device.dynamicMesh(format,vertices,indices);
    }
    IMesh* GetDynamicMesh(IMaterial* value, int bones, bool buffered, IMesh* vertices, IMesh* indices) override {
        require(value || selected, "Source dynamic mesh needs an explicit material vertex format");
        return GetDynamicMeshEx(value, value ? value->GetVertexFormat() : selected.state().vertexUsage, bones, buffered, vertices, indices);
    }
    void BindVertexBuffer(int stream, IVertexBuffer* buffer, int offset, int first, int count, VertexFormat_t format, int repeats) override {
        require(stream == 0 && buffer && offset >= 0 && first >= 0 && count > 0 && repeats == 1,
            "Source API vertex binding currently requires a single, non-instanced stream");
        sourceVertexLayout(format);
        const auto actual = device.vertexFormat(buffer);
        require(!actual || actual == format, "Source bound vertex format differs from its buffer");
        boundVertex = {buffer, uint32_t(offset), uint32_t(first), uint32_t(count), format};
    }
    void BindIndexBuffer(IIndexBuffer* buffer, int offset) override {
        require(buffer && offset >= 0, "Invalid Source index binding");
        device.indexType(buffer); boundIndex = buffer; boundIndexOffset = uint32_t(offset);
    }
    void Draw(MaterialPrimitiveType_t type, int first, int count) override {
        if (shaderUtil && !ensureEngineFrame()) return;
        require(active && boundVertex.buffer && boundIndex && first >= 0, "Source Draw requires active vertex/index bindings");
        const auto primitive = topology(type, count);
        if (!count) return;
        device.validateIndexedDraw(boundVertex.buffer, boundVertex.offset, boundVertex.first, boundVertex.count,
            boundIndex, boundIndexOffset, uint32_t(first), uint32_t(count));
        auto vertex = device.vertexSlice(boundVertex.buffer, currentFrame), index = device.indexSlice(boundIndex, currentFrame);
        vertex.offset += boundVertex.offset; vertex.size -= boundVertex.offset;
        index.offset += boundIndexOffset; index.size -= boundIndexOffset;
        encode(vertex, index, device.indexType(boundIndex), boundVertex.format, primitive, uint32_t(first), uint32_t(count),
            int32_t(boundVertex.first), {1,1,1,1});
    }
    void DrawInstances(int count,const MeshInstanceData_t* instances) override {
        require(count>=0 && (!count || instances) && !pendingMesh,"Invalid Source mesh instance list");
        if (!count || (shaderUtil && !ensureEngineFrame())) return;
        if (shaderUtil) shaderUtil->SyncMatrices();
        const auto previousModel=matrix(MATERIAL_MODEL);
        const auto previousStencil=stencilState;
        try {
            for (int i=0;i<count;++i) {
                const auto& instance=instances[i];
                require(instance.m_pVertexBuffer && instance.m_pIndexBuffer && instance.m_nVertexOffsetInBytes>=0 &&
                    instance.m_nColorVertexOffsetInBytes>=0 && instance.m_nIndexOffset>=0,"Invalid Source instance buffers/offsets");
                SourceMeshDraw draw;
                draw.vertices=instance.m_pVertexBuffer; draw.indices=instance.m_pIndexBuffer;
                draw.colors=instance.m_pColorBuffer; draw.vertexOffset=uint32_t(instance.m_nVertexOffsetInBytes);
                draw.colorOffset=uint32_t(instance.m_nColorVertexOffsetInBytes); draw.instance=&instance;
                draw.topology=topology(instance.m_nPrimType,instance.m_nIndexCount);
                draw.firstIndex=uint32_t(instance.m_nIndexOffset); draw.indexCount=uint32_t(instance.m_nIndexCount);
                if (!draw.indexCount) continue;
                std::copy_n(instance.m_DiffuseModulation.Base(),4,draw.modulation.begin());
                const auto stride=sourceVertexLayout(device.vertexFormat(draw.vertices)).stride;
                require(draw.vertexOffset%stride==0 && draw.vertexOffset/stride<uint32_t(draw.vertices->VertexCount()),"Invalid Source instance vertex range");
                device.validateIndexedDraw(draw.vertices,draw.vertexOffset,0,uint32_t(draw.vertices->VertexCount())-draw.vertexOffset/stride,
                    draw.indices,0,draw.firstIndex,draw.indexCount);
                if (instance.m_pPoseToWorld) SetSkinningMatrices(instance);
                else {
                    const float identityPose[12]={1,0,0,0,0,1,0,0,0,0,1,0};
                    LoadBoneMatrix(0,identityPose);
                }
                if (instance.m_pLightingState) SetLightingState(*instance.m_pLightingState);
                stencilState=instance.m_pStencilState?*instance.m_pStencilState:previousStencil;
                meshDraw(draw);
            }
        } catch (...) { matrices[MATERIAL_MODEL].back()=previousModel; stencilState=previousStencil; throw; }
        matrices[MATERIAL_MODEL].back()=previousModel; stencilState=previousStencil;
    }
    void ComputeVertexDescription(unsigned char* buffer, VertexFormat_t format, MeshDesc_t& desc) const override {
        require(buffer, "Null Source vertex description buffer"); sourceVertexLayout(format);
        desc = {}; ComputeVertexDesc<false>(buffer, format, desc);
    }
    int VertexFormatSize(VertexFormat_t format) const override { return int(sourceVertexLayout(format).stride); }
    int GetCurrentDynamicVBSize() override { return device.dynamicVertexBytes(); }
    int GetMaxVerticesToRender(IMaterial* value) override {
        const auto format=value?value->GetVertexFormat():selected?selected.state().vertexUsage:0;
        return device.maximumVertices(format?format:VERTEX_POSITION);
    }
    int GetMaxIndicesToRender() override { return device.maximumIndices(); }
    void GetMaxToRender(IMesh* mesh,bool,int* vertices,int* indices) override {
        require(vertices && indices,"Null Source mesh limits");
        const auto format=mesh?mesh->GetVertexFormat():material?material->GetVertexFormat():0;
        *vertices=device.maximumVertices(format?format:VERTEX_POSITION);
        *indices=device.maximumIndices();
    }
    void SetVertexShaderIndex(int index) override { require(index >= 0, "Negative Source vertex shader combo"); vertexDynamic = uint32_t(index); }
    void SetPixelShaderIndex(int index) override { require(index >= 0, "Negative Source pixel shader combo"); pixelDynamic = uint32_t(index); }
    void SetVertexShaderConstant(int first, const float* values, int count, bool) override { constants.floats(SourceShaderStage::Vertex, first, values, count); }
    void SetPixelShaderConstant(int first, const float* values, int count, bool) override { constants.floats(SourceShaderStage::Pixel, first, values, count); }
    void SetBooleanVertexShaderConstant(int first, const BOOL* values, int count, bool) override { constants.booleans(SourceShaderStage::Vertex, first, values, count); }
    void SetIntegerVertexShaderConstant(int first, const int* values, int count, bool) override { constants.integers(SourceShaderStage::Vertex, first, values, count); }
    void SetBooleanPixelShaderConstant(int first, const BOOL* values, int count, bool) override { constants.booleans(SourceShaderStage::Pixel, first, values, count); }
    void SetIntegerPixelShaderConstant(int first, const int* values, int count, bool) override { constants.integers(SourceShaderStage::Pixel, first, values, count); }
    void InvalidateDelayedShaderConstants() override {} // Every draw takes immutable register-bank snapshots.

    ShaderAPITextureHandle_t CreateTexture(int width, int height, int depth, ImageFormat format, int mips,
            int copies, int flags, const char* name, const char* group) override {
        try { return textures.create(width, height, depth, format, mips, copies, uint32_t(flags), name, group); }
        catch (const std::logic_error& error) {
            throw std::invalid_argument("Source texture '"+std::string(name?name:"")+"' "+
                std::to_string(width)+"x"+std::to_string(height)+"x"+std::to_string(depth)+
                " format="+std::to_string(int(format))+" mips="+std::to_string(mips)+" copies="+std::to_string(copies)+
                " flags="+std::to_string(flags)+": "+error.what());
        }
    }
    void CreateTextures(ShaderAPITextureHandle_t* handles, int count, int width, int height, int depth, ImageFormat format,
            int mips, int copies, int flags, const char* name, const char* group) override {
        require(handles && count > 0 && size_t(count) <= limits.textures.maximumTextures, "Invalid Source texture batch");
        std::vector<ShaderAPITextureHandle_t> pending;
        pending.reserve(size_t(count));
        try { for (int i = 0; i < count; ++i) pending.push_back(CreateTexture(width, height, depth, format, mips, copies, flags, name, group)); }
        catch (...) { for (auto handle : pending) textures.destroy(handle); throw; }
        std::copy(pending.begin(), pending.end(), handles);
    }
    void DeleteTexture(ShaderAPITextureHandle_t handle) override {
        if (shaderUtil) {
            closePass();
            if (colorTarget == handle || depthTarget == handle) SetRenderTarget(SHADER_RENDERTARGET_BACKBUFFER,SHADER_RENDERTARGET_DEPTHBUFFER);
            // Frame retainers keep recorded framebuffers/images alive.
            engineTargets.clear();
        }
        textures.destroy(handle);
        if (modifying == handle) modifying = 0;
        for (auto& state : pixelTextures) if (state.handle == handle) state = {};
        for (auto& state : vertexTextures) if (state.handle == handle) state = {};
        for (auto& standard : standardTextures) if (standard == handle) standard = 0;
    }
    bool IsTexture(ShaderAPITextureHandle_t handle) override { return textures.exists(handle); }
    bool IsTextureResident(ShaderAPITextureHandle_t handle) override { return textures.resident(handle); }
    void ModifyTexture(ShaderAPITextureHandle_t handle) override { textures.info(handle); modifying = handle; }
    void TexImage2D(int mip, int face, ImageFormat destination, int z, int width, int height, ImageFormat source, bool tiled, void* data) override {
        require(!tiled && width > 0 && height > 0, "Source texture uploads require untiled data");
        const auto bytes = data ? sourceTextureDataLayout(source, uint32_t(width), uint32_t(height)).sourceBytes : 0;
        textures.image(modifying, mip, face, destination, z, width, height, source, data, bytes);
    }
    void TexSubImage2D(int mip, int face, int x, int y, int z, int width, int height, ImageFormat source, int pitch, bool tiled, void* data) override {
        require(!tiled && width > 0 && height > 0 && pitch >= 0, "Invalid Source subimage upload");
        const auto layout = sourceTextureDataLayout(source, uint32_t(width), uint32_t(height), size_t(pitch));
        textures.subImage(modifying, mip, face, x, y, z, width, height, source, data, layout.sourceBytes, size_t(pitch));
    }
    void TexImageFromVTF(IVTFTexture* texture, int frame) override {
        require(texture && frame>=0 && frame<texture->FrameCount(),"Invalid VTF frame");
        const auto info=textures.info(modifying);
        for(uint32_t face=0;face<info.layers;++face) for(uint32_t mip=0;mip<info.mipLevels;++mip) {
            const int width=std::max(1,int(info.width>>mip)),height=std::max(1,int(info.height>>mip));
            TexImage2D(int(mip),int(face),info.sourceFormat,0,width,height,texture->Format(),false,texture->ImageData(frame,int(face),int(mip)));
        }
    }
    bool TexLock(int level,int face,int x,int y,int width,int height,CPixelWriter& writer) override {
        require(!textureLock.texture,"Source texture is already locked");
        const auto info=textures.info(modifying);
        require(!info.renderTarget && level>=0 && uint32_t(level)<info.mipLevels && width>0 && height>0,
            "Invalid Source texture lock");
        const auto layout=sourceTextureDataLayout(info.sourceFormat,uint32_t(width),uint32_t(height));
        textureLock={modifying,level,face,x,y,width,height,info.sourceFormat,layout.rowBytes,{}};
        textureLock.pixels.resize(layout.sourceBytes);
        writer.SetPixelMemory(info.sourceFormat,textureLock.pixels.data(),int(textureLock.pitch));
        if(!writer.GetPixelSize()) {textureLock={};return false;}
        return true;
    }
    void TexUnlock() override {
        require(textureLock.texture,"Source texture is not locked");
        auto lock=std::move(textureLock);textureLock={};
        const auto info=textures.info(lock.texture);
        if(!lock.x && !lock.y && lock.width==std::max(1,int(info.width>>lock.level)) && lock.height==std::max(1,int(info.height>>lock.level)))
            textures.image(lock.texture,lock.level,lock.face,info.sourceFormat,0,lock.width,lock.height,lock.format,lock.pixels.data(),lock.pixels.size());
        else textures.subImage(lock.texture,lock.level,lock.face,lock.x,lock.y,0,lock.width,lock.height,lock.format,lock.pixels.data(),lock.pixels.size(),lock.pitch);
    }
    ShaderAPITextureHandle_t CreateDepthTexture(ImageFormat,int width,int height,const char* name,bool,bool alias) override {
        require(!alias,"Console depth aliasing is not available");
        // The argument is the companion COLOR target's format, not the depth
        // format. Source expects its advertised 8 stencil bits on this surface.
        return CreateTexture(width,height,1,IMAGE_FORMAT_D24S8,1,1,TEXTURE_CREATE_DEPTHBUFFER,name,"RenderTargets");
    }
    ImageFormat GetNearestRenderTargetFormat(ImageFormat format) const override {
        switch(format) {
        case IMAGE_FORMAT_RGBA16161616F: case IMAGE_FORMAT_R32F: case IMAGE_FORMAT_RGBA32323232F:
            return format;
        default:return IMAGE_FORMAT_RGBA8888;
        }
    }
    void TexMinFilter(ShaderTexFilterMode_t mode) override { textures.minFilter(modifying, mode); }
    void TexMagFilter(ShaderTexFilterMode_t mode) override { textures.magFilter(modifying, mode); }
    void TexWrap(ShaderTexCoordComponent_t coordinate, ShaderTexWrapMode_t mode) override { textures.wrap(modifying, coordinate, mode); }
    void BindTexture(Sampler_t sampler, TextureBindFlags_t flags, ShaderAPITextureHandle_t handle) override {
        const auto bits = uint32_t(flags);
        require(int(sampler) >= 0 && int(sampler) < 16 && !(bits & ~(uint32_t(TEXTURE_BINDFLAGS_SRGBREAD) | uint32_t(TEXTURE_BINDFLAGS_NOMIP))),
            "Invalid Source sampler or unsupported shadow-depth binding");
        if(handle==INVALID_SHADERAPI_TEXTURE_HANDLE) {pixelTextures[size_t(sampler)]={};return;}
        textures.info(handle); pixelTextures[size_t(sampler)] = {handle, bits, false};
    }
    void BindVertexTexture(VertexTextureSampler_t sampler, ShaderAPITextureHandle_t handle) override {
        require(int(sampler) >= 0 && int(sampler) < 4, "Invalid Source vertex texture sampler");
        textures.info(handle); vertexTextures[size_t(sampler)] = {handle, 0, false};
    }
    void SetTextureFilterMode(Sampler_t sampler, TextureFilterMode_t mode) override {
        require(int(sampler) >= 0 && int(sampler) < 16 && mode == TFILTER_MODE_POINTSAMPLED, "Unsupported Source sampler override");
        pixelTextures[size_t(sampler)].point = true;
    }
    void SetStandardTextureHandle(StandardTextureId_t id, ShaderAPITextureHandle_t handle) override {
        require(int(id) >= 0 && size_t(id) < standardTextures.size(), "Invalid Source standard texture id");
        if (handle > 0) textures.info(handle);
        standardTextures[size_t(id)] = handle > 0 ? handle : 0;
    }
    void SetLinearToGammaConversionTextures(ShaderAPITextureHandle_t srgb,ShaderAPITextureHandle_t linear) override {
        // Native shaders use linear/sRGB image views, without a lookup sampler.
        if(srgb>0)textures.info(srgb);if(linear>0)textures.info(linear);
    }
    ShaderAPITextureHandle_t GetStandardTextureHandle(StandardTextureId_t id) override {
        require(int(id) >= 0 && size_t(id) < standardTextures.size(), "Invalid Source standard texture id"); return standardTextures[size_t(id)];
    }
    bool IsStandardTextureHandleValid(StandardTextureId_t id) override { return textures.exists(GetStandardTextureHandle(id)); }
    void BindStandardTexture(Sampler_t sampler, TextureBindFlags_t flags, StandardTextureId_t id) override {
        if (shaderUtil && id>=TEXTURE_WHITE && id<=TEXTURE_SSBUMP_FLAT) {
            auto handle=shaderUtil->GetStandardTexture(id);
            if (!textures.exists(handle)) handle=constantTexture(id);
            BindTexture(sampler,flags,handle);
        }
        else if (shaderUtil && currentInstance && id==TEXTURE_LIGHTMAP && currentInstance->m_nLightmapPageId!=MATERIAL_SYSTEM_LIGHTMAP_PAGE_INVALID)
            BindTexture(sampler,flags,shaderUtil->GetLightmapTexture(currentInstance->m_nLightmapPageId));
        else if (shaderUtil && currentInstance && id==TEXTURE_LOCAL_ENV_CUBEMAP && currentInstance->m_pEnvCubemap)
            BindTexture(sampler,flags,shaderUtil->GetShaderAPITextureBindHandle(const_cast<ITexture*>(currentInstance->m_pEnvCubemap),0,0));
        else if (shaderUtil) shaderUtil->BindStandardTexture(sampler,flags,id);
        else BindTexture(sampler, flags, GetStandardTextureHandle(id));
    }
    void BindStandardVertexTexture(VertexTextureSampler_t sampler, StandardTextureId_t id) override { BindVertexTexture(sampler, GetStandardTextureHandle(id)); }
    void GetStandardTextureDimensions(int* width, int* height, StandardTextureId_t id) override {
        require(width && height, "Null Source standard texture dimension output");
        const auto value = textures.info(GetStandardTextureHandle(id)); *width = int(value.width); *height = int(value.height);
    }
    ShaderAPITextureHandle_t FindTexture(const char* name) override { return textures.find(name); }
    void GetTextureDimensions(ShaderAPITextureHandle_t handle, int& width, int& height, int& depth) override {
        const auto value = textures.info(handle); width = int(value.width); height = int(value.height); depth = 1;
    }
    bool CanDownloadTextures() const override { return device.interface().IsUsingGraphics(); }
    ImageFormat GetNearestSupportedFormat(ImageFormat format, bool filtering) const override {
        SourceTextureDataLayout layout;
        try { layout=sourceTextureDataLayout(format,1,1); }
        catch(const std::invalid_argument&) { return IMAGE_FORMAT_RGBA8888; }
        require(context.supportsFormat(layout.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            (filtering ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0)), "Source texture format/filter is not supported");
        return format; // CPU conversion preserves this Source storage contract.
    }
    void EvictManagedResources() override {
        require(!device.frameActive(), "Evict Source textures between frames"); cachedSets.clear(); textures.evictManagedResources();
    }
    void FlushHardware() override { textures.flushUploads(device); }
    void ForceHardwareSync() override { require(!device.frameActive(), "Source hardware sync must occur between frames"); context.waitIdle(); }
    void TexSetPriority(int) override {} // VMA owns placement; Source handles keep resources resident.
    void GetGPUMemoryStats(GPUMemoryStats& stats) override {
        stats={};stats.nUnknown=uint32_t(std::min<VkDeviceSize>(context.allocationStatistics().allocationBytes,0xffffffffu));
    }
    void ClearVertexAndPixelShaderRefCounts() override {} // Shader modules are owned by the immutable variant library.
    void PurgeUnusedVertexAndPixelShaders() override {}
    void DestroyVertexBuffers(bool) override {} // Buffer handles are destroyed by their explicit Source owners.
    void MarkUnusedVertexFields(unsigned int,int,bool*) override {} // Pipeline attributes use only the declared shader inputs.
    void HandleDeviceLost() override {} // Context diagnoses Vulkan device loss and owns swapchain recovery.
    void EnableBuffer2FramesAhead(bool) override {} // Context always fences its two reusable frame slots.
    void AntiAliasingHint(int) override {}
    void EnableLinearColorSpaceFrameBuffer(bool) override {} // Linear/sRGB are selected through immutable attachment views.
    void EnableAlphaToCoverage() override {} // Single-sample targets have no sample coverage mask.
    void DisableAlphaToCoverage() override {}
    void BeginPIXEvent(unsigned long,const char*) override {}
    void EndPIXEvent() override {}
    void SetPIXMarker(unsigned long,const char*) override {}
    void SyncToken(const char*) override {}
    void DXSupportLevelChanged(int level) override { require(level==95,"Vulkan materials use the Source SM3 feature tier"); }
    void AddShaderComboInformation(const ShaderComboSemantics_t*) override {} // Offline manifest is the authoritative combo table.
    void SetStencilState(const ShaderStencilState_t& state) override { stencilState=state; }
    void OverrideDepthEnable(bool enabled,bool write,bool test) override {depthOverride=enabled;depthWriteOverride=write;depthTestOverride=test;}
    void OverrideAlphaWriteEnable(bool enabled,bool write) override {alphaOverride=enabled;alphaWriteOverride=write;}
    void OverrideColorWriteEnable(bool enabled,bool write) override {colorOverride=enabled;colorWriteOverride=write;}
    void ForceDepthFuncEquals(bool enabled) override {depthEquals=enabled;}
    void SetHeightClipZ(float) override {}
    void SetHeightClipMode(MaterialHeightClipMode_t mode) override {require(mode==MATERIAL_HEIGHTCLIPMODE_DISABLE,"Height clipping needs a material clip-plane shader");}
    void EnableClipPlane(int,bool enabled) override {require(!enabled,"User clip planes are not enabled");}
    void SetShadowDepthBiasFactors(float,float) override {}
    void SetFlexWeights(int,int,const MorphWeight_t*) override {} // Hardware morphing is not advertised.
    void PrintfVA(char* format,va_list args) override { std::vfprintf(stderr,format,args); }
    void Printf(char* format,...) override { va_list args;va_start(args,format);PrintfVA(format,args);va_end(args); }
    void readPixels(const Image& image,int x,int y,int width,int height,unsigned char* data,ImageFormat format,int pitch=0) {
        if (!(shaderUtil && image && x>=0 && y>=0 && width>0 && height>0 && data &&
            uint64_t(x)+width<=image.description().width && uint64_t(y)+height<=image.description().height))
            throw std::invalid_argument("Invalid Source readback region: "+std::to_string(x)+","+std::to_string(y)+" "+
                std::to_string(width)+"x"+std::to_string(height)+" image="+
                std::to_string(image?image.description().width:0)+"x"+std::to_string(image?image.description().height:0));
        require(image.description().format==VK_FORMAT_R8G8B8A8_UNORM || image.description().format==VK_FORMAT_R8G8B8A8_SRGB,
            "Source readback currently requires an RGBA8 target");
        closePass();
        auto storage=context.createBuffer(VkDeviceSize(width)*height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
        auto record=[&](VkCommandBuffer commands) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.oldLayout=detail::Access::allocation(image)->attachmentLayout;
            barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.image=image.handle();barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            context.vk().vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.imageOffset={x,y,0};copy.imageExtent={uint32_t(width),uint32_t(height),1};
            context.vk().vkCmdCopyImageToBuffer(commands,image.handle(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,storage.handle(),1,&copy);
            std::swap(barrier.oldLayout,barrier.newLayout);barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            context.vk().vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=storage.handle();host.size=VK_WHOLE_SIZE;
            context.vk().vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        };
        if(active) {record(currentFrame.commands);context.submitFramePrefix(currentFrame);} else context.submitAndWait(record);
        std::vector<uint8_t> pixels(size_t(width)*height*4);storage.read(0,pixels.data(),pixels.size());
        require(shaderUtil->ConvertImageFormat(pixels.data(),IMAGE_FORMAT_RGBA8888,data,format,width,height,width*4,pitch),"Source readback format conversion failed");
    }
    void ReadPixels(int x,int y,int width,int height,unsigned char* data,ImageFormat format,ITexture* renderTexture) override {
        Image image=backColor;
        if(renderTexture) image=textures.renderImage(shaderUtil->GetShaderAPITextureBindHandle(renderTexture,0,0));
        readPixels(image,x,y,width,height,data,format);
    }
    void ReadPixels(Rect_t* source,Rect_t* destination,unsigned char* data,ImageFormat format,int pitch) override {
        require(source && destination && source->width==destination->width && source->height==destination->height,
            "Scaled Source readback requires an explicit resample");
        readPixels(colorTarget==SHADER_RENDERTARGET_BACKBUFFER?backColor:textures.renderImage(colorTarget),
            source->x,source->y,source->width,source->height,data,format,pitch);
    }

    void SetViewports(int count, const ShaderViewport_t* values, bool) override {
        require(count == 1 && values && values->m_nVersion == SHADER_VIEWPORT_VERSION, "Source Vulkan supports one versioned viewport");
        const auto& value = *values;
        require(value.m_nTopLeftX >= 0 && value.m_nTopLeftY >= 0 && value.m_nWidth > 0 && value.m_nHeight > 0 &&
            uint32_t(value.m_nTopLeftX) < extent.width && uint32_t(value.m_nTopLeftY) < extent.height &&
            std::isfinite(value.m_flMinZ) && std::isfinite(value.m_flMaxZ) && value.m_flMinZ >= 0 && value.m_flMaxZ <= 1 &&
            value.m_flMinZ <= value.m_flMaxZ, "Source viewport is invalid or outside the render target");
        viewport = value;
        // Source permits a viewport request larger than a texture target (for
        // example when rendering reduced-resolution screen effects). Match
        // the legacy API's clamp before passing the viewport to Vulkan.
        viewport.m_nWidth=std::min(uint32_t(value.m_nWidth),extent.width-uint32_t(value.m_nTopLeftX));
        viewport.m_nHeight=std::min(uint32_t(value.m_nHeight),extent.height-uint32_t(value.m_nTopLeftY));
    }
    int GetViewports(ShaderViewport_t* values, int maximum) const override {
        require(maximum >= 0 && (!maximum || values), "Invalid Source viewport output"); if (maximum) *values = viewport; return 1;
    }
    void GetCurrentViewport(int& x, int& y, int& width, int& height) const override {
        x = viewport.m_nTopLeftX; y = viewport.m_nTopLeftY; width = viewport.m_nWidth; height = viewport.m_nHeight;
    }
    void GetCurrentRenderTargetDimensions(int& width, int& height) const override { width = int(extent.width); height = int(extent.height); }
    void GetBackBufferDimensions(int& width, int& height) const override { device.interface().GetBackBufferDimensions(width, height); }
    const AspectRatioInfo_t& GetAspectRatioInfo() const override { return device.interface().GetAspectRatioInfo(); }
    void SetScissorRect(int left, int top, int right, int bottom, bool enabled) override {
        require(!enabled || (left >= 0 && top >= 0 && right >= left && bottom >= top &&
            uint32_t(right) <= extent.width && uint32_t(bottom) <= extent.height), "Source scissor is outside its render target");
        scissorEnabled = enabled;
        if (enabled) scissor = {{left,top}, {uint32_t(right-left),uint32_t(bottom-top)}};
    }
    void ForceCommitScissorRect() override { if (encoder) encoder->scissor(scissorEnabled ? scissor : VkRect2D{{0,0},extent}); }
    void ClearColor3ub(unsigned char r, unsigned char g, unsigned char b) override { ClearColor4ub(r,g,b,255); }
    void ClearColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) override {
        clearColor = {r/255.0f,g/255.0f,b/255.0f,a/255.0f};
    }
    void ClearBuffers(bool clear, bool z, bool stencil, int width, int height) override {
        require(width > 0 && height > 0 && uint32_t(width) == extent.width && uint32_t(height) == extent.height,
            "Source clear requires the current render-target extent");
        if (!clear && !z && !stencil) return;
        if (shaderUtil && !active && colorTarget==SHADER_RENDERTARGET_BACKBUFFER && !ensureEngineFrame()) return;
        if (shaderUtil && !active) {
            // Loading and CTexture initialization can clear a full target or
            // just an atlas viewport before the first swap image is acquired.
            require(!device.frameActive(),"Source attachment clear has an inconsistent active frame");
            const VkRect2D rectangle {{viewport.m_nTopLeftX,viewport.m_nTopLeftY},
                {uint32_t(viewport.m_nWidth),uint32_t(viewport.m_nHeight)}};
            if (clear) clearAttachmentRegion(context, colorTarget==SHADER_RENDERTARGET_BACKBUFFER ? backColor : textures.renderImage(colorTarget), rectangle, clearColor);
            if (z || stencil) clearAttachmentRegion(context, depthTarget==SHADER_RENDERTARGET_DEPTHBUFFER ? backDepth : textures.renderImage(depthTarget), rectangle,
                clearColor,(z?VK_IMAGE_ASPECT_DEPTH_BIT:0)|(stencil?VK_IMAGE_ASPECT_STENCIL_BIT:0));
            return;
        }
        if (shaderUtil) ensurePass();
        require(!z || depth != VK_FORMAT_UNDEFINED, "Source clear requested an absent depth attachment");
        require(!stencil || depth==VK_FORMAT_D24_UNORM_S8_UINT || depth==VK_FORMAT_D32_SFLOAT_S8_UINT ||
            depth==VK_FORMAT_D16_UNORM_S8_UINT,"Source clear requested an absent stencil attachment");
        if (!clear && !z && !stencil) return;
        ensurePass();
        VkClearAttachment attachments[2] {};
        uint32_t count = 0;
        if (clear) {
            auto& attachment = attachments[count++]; attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            std::copy(clearColor.begin(), clearColor.end(), attachment.clearValue.color.float32);
        }
        if (z || stencil) {
            auto& attachment = attachments[count++];
            attachment.aspectMask = (z ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            attachment.clearValue.depthStencil.depth = 1;
        }
        VkClearRect rectangle {{{viewport.m_nTopLeftX,viewport.m_nTopLeftY}, {uint32_t(viewport.m_nWidth),uint32_t(viewport.m_nHeight)}}, 0, 1};
        context.vk().vkCmdClearAttachments(currentFrame.commands, count, attachments, 1, &rectangle);
    }
    void ClearBuffersObeyStencilEx(bool clear,bool alpha,bool z) override {
        if(shaderUtil)shaderUtil->DrawClearBufferQuad(uint8_t(clearColor[0]*255),uint8_t(clearColor[1]*255),
            uint8_t(clearColor[2]*255),uint8_t(clearColor[3]*255),clear,alpha,z);
        else {
            require(!stencilState.m_bEnable && clear==alpha,"Stencil-aware clears require the Source material callback");
            ClearBuffers(clear,z,false,int(extent.width),int(extent.height));
        }
    }
    void ClearBuffersObeyStencil(bool clear, bool z) override { ClearBuffersObeyStencilEx(clear,clear,z); }
    void SetRenderTarget(ShaderAPITextureHandle_t colorHandle, ShaderAPITextureHandle_t depthHandle) override {
        if (shaderUtil) {
            closePass();colorTarget=colorHandle;depthTarget=depthHandle;
            int width,height;
            if (colorHandle == SHADER_RENDERTARGET_BACKBUFFER)device.interface().GetBackBufferDimensions(width,height);
            else {auto image=textures.renderImage(colorHandle);width=int(image.description().width);height=int(image.description().height);}
            extent={uint32_t(width),uint32_t(height)};
            viewport.Init(0,0,width,height);scissor={{0,0},extent};scissorEnabled=false;
            return;
        }
        require(colorHandle == SHADER_RENDERTARGET_BACKBUFFER &&
            (depthHandle == SHADER_RENDERTARGET_DEPTHBUFFER || depthHandle == SHADER_RENDERTARGET_NONE),
            "Texture render targets are not yet connected to ShaderApi029");
        if (target) closePass();
        extent = currentFrame.extent; color = currentFrame.colorFormat; depth = VK_FORMAT_UNDEFINED;
        viewport.Init(0,0,int(extent.width),int(extent.height));
        scissor = {{0,0}, extent}; scissorEnabled = false;
    }
    void SetRenderTargetEx(int index, ShaderAPITextureHandle_t colorHandle, ShaderAPITextureHandle_t depthHandle) override {
        require(index >= 0 && index < 4, "Invalid Source render target slot");
        if (index) {
            // Source resets all four slots when restoring its target stack.
            // The backbuffer sentinel means unbind for slots other than zero.
            require(colorHandle == SHADER_RENDERTARGET_BACKBUFFER || colorHandle == SHADER_RENDERTARGET_NONE,
                "Source MRT attachments are not implemented");
            return;
        }
        SetRenderTarget(colorHandle,depthHandle);
    }
    void SetFullScreenTextureHandle(ShaderAPITextureHandle_t handle) override {
        if (handle!=INVALID_SHADERAPI_TEXTURE_HANDLE) textures.renderImage(handle);
        fullScreenTexture=handle;
    }
    void copyTarget(const Image& source,const Image& destination,const Rect_t* sourceRect,const Rect_t* destinationRect) {
        require(active,"Render-target copies require an active frame");
        closePass();
        auto rectangle=[](const Rect_t* rect,const Image& image) -> VkRect2D {
            if (!rect) return {{0,0},{image.description().width,image.description().height}};
            require(rect->width>0 && rect->height>0,"Invalid Source copy rectangle");
            return {{rect->x,rect->y},{uint32_t(rect->width),uint32_t(rect->height)}};
        };
        blitAttachmentImage(context,currentFrame,source,destination,rectangle(sourceRect,source),rectangle(destinationRect,destination));
    }
    void CopyRenderTargetToTexture(ShaderAPITextureHandle_t handle) override { CopyRenderTargetToTextureEx(handle,0,nullptr,nullptr); }
    void CopyRenderTargetToTextureEx(ShaderAPITextureHandle_t handle,int index,Rect_t* sourceRect,Rect_t* destinationRect) override {
        require(shaderUtil && index==0,"Only the primary Source color target can be copied");
        // Source can run its post-processing callbacks while the window is
        // minimized. Match draw availability, and acquire before resolving the
        // backbuffer image because resuming may replace it after a resize.
        if (!ensureEngineFrame()) return;
        copyTarget(colorTarget==SHADER_RENDERTARGET_BACKBUFFER?backColor:textures.renderImage(colorTarget),textures.renderImage(handle),sourceRect,destinationRect);
    }
    void CopyTextureToRenderTargetEx(int index,ShaderAPITextureHandle_t handle,Rect_t* sourceRect,Rect_t* destinationRect) override {
        require(shaderUtil && index==0,"Only the primary Source color target can receive a copy");
        if (!ensureEngineFrame()) return;
        copyTarget(textures.renderImage(handle),colorTarget==SHADER_RENDERTARGET_BACKBUFFER?backColor:textures.renderImage(colorTarget),sourceRect,destinationRect);
    }
    void CullMode(MaterialCullMode_t mode) override {
        require(mode == MATERIAL_CULLMODE_CCW || mode == MATERIAL_CULLMODE_CW || mode==MATERIAL_CULLMODE_NONE, "Invalid Source cull mode");
        noCulling=mode==MATERIAL_CULLMODE_NONE;
        frontFace = mode == MATERIAL_CULLMODE_CCW ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    void FlipCullMode() override { frontFace = frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE; }
    void ShadeMode(ShaderShadeMode_t mode) override { require(mode == SHADER_SMOOTH, "Source flat shading requires a separate shader variant"); }
    bool SupportsMSAAMode(int samples) override { return samples == 0 || samples == 1; }
    bool SupportsCSAAMode(int, int) override { return false; }
    bool DoRenderTargetsNeedSeparateDepthBuffer() const override { return false; }
    void SetAnisotropicLevel(int level) override {
        require(level>=0,"Negative Source anisotropy level");
        // Update the engine before it reapplies per-texture filtering modes.
        if (shaderUtil) shaderUtil->NoteAnisotropicLevel(level);
        textures.setAnisotropicLevel(level);
    }
    bool IsHWMorphingEnabled() const override { return false; }
    void EnableHWMorphing(bool enabled) override { require(!enabled, "Source hardware morphing is not implemented"); }
    bool InFlashlightMode() const override { return false; }
    bool SinglePassFlashlightModeEnabled() override { return singlePassFlashlight; }
    void EnableSinglePassFlashlightMode(bool enabled) override {
        // This is the engine's batching preference, also set when no flashlight
        // exists. Actual flashlight state/shaders are separate API operations.
        singlePassFlashlight=enabled;
    }
    bool IsCascadedShadowMapping() const override { return false; }
    bool InEditorMode() const override { return false; }
    bool IsRenderingPaint() const override { return false; }
    bool IsStereoActiveThisFrame() const override { return false; }
    bool IsStereoSupported() const override { return false; }
    ShaderAPIOcclusionQuery_t CreateOcclusionQueryObject() override {
        return reinterpret_cast<ShaderAPIOcclusionQuery_t>(queries.create());
    }
    void DestroyOcclusionQueryObject(ShaderAPIOcclusionQuery_t handle) override { queries.destroy(reinterpret_cast<uintptr_t>(handle)); }
    void BeginOcclusionQueryDrawing(ShaderAPIOcclusionQuery_t handle) override {
        require(!queries.active() && !skippedQuery,"Source occlusion queries cannot overlap");
        if(shaderUtil && !ensureEngineFrame()) {queries.invalidate(reinterpret_cast<uintptr_t>(handle));skippedQuery=handle;return;}
        require(active,"Source occlusion query requires a frame");
        auto* resume=shaderUtil?nullptr:target;
        const auto savedViewport=viewport;const auto savedScissor=scissor;const bool savedScissorEnabled=scissorEnabled;
        closePass();queries.begin(reinterpret_cast<uintptr_t>(handle),currentFrame);
        // Standalone users retain their explicit offscreen target. The engine
        // chooses its current attachment view when the next material is drawn.
        if(resume) {
            startTarget(*resume,clearColor,1);viewport=savedViewport;scissor=savedScissor;scissorEnabled=savedScissorEnabled;
        }
    }
    void EndOcclusionQueryDrawing(ShaderAPIOcclusionQuery_t handle) override {
        if(skippedQuery==handle && skippedQuery) {skippedQuery=nullptr;return;}
        queries.end(reinterpret_cast<uintptr_t>(handle),currentFrame);
    }
    int OcclusionQuery_GetNumPixelsRendered(ShaderAPIOcclusionQuery_t handle,bool flush) override {
        const auto id=reinterpret_cast<uintptr_t>(handle);
        int result=queries.result(id);
        if(result!=OCCLUSION_QUERY_RESULT_PENDING || !flush)return result;
        if(active && queries.frameSerial(id)==detail::Access::serial(context,currentFrame)) {
            require(!queries.active(),"Cannot flush an active Source occlusion query");
            auto* resume=shaderUtil?nullptr:target;
            const auto savedViewport=viewport;const auto savedScissor=scissor;const bool savedScissorEnabled=scissorEnabled;
            closePass();context.submitFramePrefix(currentFrame);queries.completedPrefix(currentFrame);
            if(resume) {
                startTarget(*resume,clearColor,1);viewport=savedViewport;scissor=savedScissor;scissorEnabled=savedScissorEnabled;
            }
            return queries.result(id);
        }
        require(!device.frameActive() || queries.frameSerial(id)!=detail::Access::serial(context,currentFrame),
            "Submit the completed present pass before flushing its occlusion query");
        return queries.result(id,true);
    }
    bool ShouldWriteDepthToDestAlpha() const override { return false; }
    int GetNumActiveDeformations() const override { return 0; }
    double CurrentTime() const override { return gameTimeSet ? gameTime : platform::time(); }
    void UpdateGameTime(float time) override { require(std::isfinite(time), "Non-finite Source game time"); gameTime = time; gameTimeSet = true; }
    float GetLightMapScaleFactor() const override { return shadow.interface().GetLightMapScaleFactor(); }
    float GammaToLinear_HardwareSpecific(float value) const override {
        require(std::isfinite(value) && value >= 0, "Invalid sRGB input"); return value <= 0.04045f ? value/12.92f : std::pow((value+0.055f)/1.055f,2.4f);
    }
    float LinearToGamma_HardwareSpecific(float value) const override {
        require(std::isfinite(value) && value >= 0, "Invalid linear color input"); return value <= 0.0031308f ? value*12.92f : 1.055f*std::pow(value,1/2.4f)-0.055f;
    }
};

SourceAPI::SourceAPI(Context& ctx, GraphicsDevice& gfx, FrameArena& arena, SourceDevice& device,
        SourceShadow& shadow, SourceShaderLibrary& shaders, SourceAPILimits limits)
    : impl_(std::make_unique<Impl>(ctx,gfx,arena,device,shadow,shaders,limits)) {}
SourceAPI::~SourceAPI() = default;
IShaderAPI& SourceAPI::interface() { return *impl_; }
void SourceAPI::registerShaderInputs(const std::string& name, std::vector<SourceShaderInput> inputs,uint32_t staticIndex,uint32_t dynamicIndex,bool dynamicTextureReads) {
    auto& api = *impl_;
    require(!api.device.frameActive() && !inputs.empty() && api.shaderInputs.size() < api.limits.maximumShaderInputs,
        "Register Source shader inputs between frames within the contract budget");
    std::set<uint32_t> locations;
    for (const auto& input : inputs) require(input.location < api.context.capabilities().properties.limits.maxVertexInputAttributes &&
        locations.insert(input.location).second, "Duplicate or out-of-range Source shader attribute location");
    auto key=shaderName(name);
    if(staticIndex!=UINT32_MAX || dynamicIndex!=UINT32_MAX)key+=":"+std::to_string(staticIndex)+":"+std::to_string(dynamicIndex);
    require(api.shaderInputs.emplace(key, std::move(inputs)).second, "Source shader inputs are already registered");
    if(dynamicTextureReads)api.dynamicTextureReadShaders.insert(shaderName(name));
}
bool SourceAPI::beginFrame() { return impl_->startFrame(); }
const Frame& SourceAPI::frame() const { require(impl_->active, "No active Source API frame"); return impl_->currentFrame; }
void SourceAPI::beginTarget(RenderTarget& target, const std::array<float,4>& clear, float depth) { impl_->startTarget(target,clear,depth); }
void SourceAPI::endTarget() {
    require(impl_->active && impl_->target, "No active Source API offscreen target");
    impl_->SetRenderTarget(SHADER_RENDERTARGET_BACKBUFFER, SHADER_RENDERTARGET_DEPTHBUFFER);
}
void SourceAPI::flushUploads() { impl_->textures.flushUploads(impl_->device); }
void SourceAPI::setMaterialPassCallback(std::function<void(IMaterial*, const SourceMeshDraw&)> callback) {
    require(!impl_->pendingMesh && !impl_->material, "Unbind the Source material before replacing its callback"); impl_->materialPass = std::move(callback);
}
void SourceAPI::setShaderUtil(IShaderUtil* util) { impl_->shaderUtil = util; }
SourceAPIStatistics SourceAPI::statistics() const {
    auto counts = impl_->counts;
    counts.snapshots = impl_->snapshots.size(); counts.constantUploads = impl_->constants.uploads();
    counts.textures = impl_->textures.statistics();counts.queries=impl_->queries.statistics(); return counts;
}
std::vector<SourceTextureDebugInfo> SourceAPI::debugTextures() const { return impl_->textures.debugInfo(); }
void SourceAPI::debugTextureRendering(bool enabled) { impl_->textures.debugRendering(enabled); }
} // namespace sourcevk
