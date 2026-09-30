#include "vulkan_graphics.h"
#include "vulkan_target.h"
#include "vulkan_upload.h"
#include "vulkan_source.h"
#include "vulkan_material.h"
#include "vulkan_device.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "shaderapi/ishadershadow.h"
#include "shaderapi/IShaderDevice.h"
#include "materialsystem/imesh.h"

namespace {
using namespace sourcevk;
constexpr uint32_t Width = 128, Height = 64, Frames = 90;
struct Vertex { float position[3]; uint8_t color[4]; float uv[2]; };
struct Instance { float offset[2], color[4]; };
struct Push { float transform[4]; float parameters[4]; SourceAlphaState alpha; };
static_assert(sizeof(Push) == 48);
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void rejected(Function function) {
    try { function(); } catch (const std::invalid_argument&) { return; } catch (const std::logic_error&) { return; }
    throw std::runtime_error("Invalid GPU binding was accepted");
}
void sourceDataChecks() {
    SourceVertexFormatDescription skin;
    skin.normal = true; skin.boneWeights = 2; skin.userData = 4; skin.texCoords[0] = 2;
    const auto layout = sourceVertexLayout(sourceVertexFormat(skin));
    const auto attributes = layout.attributes({{SourceSemantic::Position,0,0}, {SourceSemantic::BoneWeights,0,1},
        {SourceSemantic::BoneIndices,0,2}, {SourceSemantic::Normal,0,3}, {SourceSemantic::TexCoord,0,4}, {SourceSemantic::UserData,0,5}});
    const uint32_t offsets[] = {0,12,20,24,36,44};
    require(layout.stride == 64, "Source skinned vertex cache padding changed");
    for (size_t i = 0; i < attributes.size(); ++i) require(attributes[i].offset == offsets[i], "Source skinned vertex field offset changed");
    rejected([&] { sourceVertexLayout(sourceVertexFormat(skin) | (uint64_t(1) << 63)); });
    const uint8_t bgra[] = {7,23,101,59, 0,0,0,0, 5,6,7,8};
    const auto converted = prepareSourceTexture(IMAGE_FORMAT_BGRA8888,1,2,bgra,sizeof(bgra),8,true);
    require(converted.pixels == std::vector<uint8_t>({101,23,7,59,7,6,5,8}) && converted.description.format == VK_FORMAT_R8G8B8A8_SRGB,
        "Source BGRA conversion, row pitch or sRGB choice failed");
    const uint8_t intensity = 55;
    require(prepareSourceTexture(IMAGE_FORMAT_I8,1,1,&intensity,1).pixels == std::vector<uint8_t>({55,55,55,255}), "Source intensity conversion failed");
    require(prepareSourceTexture(IMAGE_FORMAT_A8,1,1,&intensity,1).pixels == std::vector<uint8_t>({255,255,255,55}), "Source alpha conversion failed");
    const uint8_t bc1[] = {0,0xf8,0xe0,7,0xe4,0,0,0};
    require(prepareSourceTexture(IMAGE_FORMAT_DXT1,4,1,bc1,sizeof(bc1)).pixels ==
        std::vector<uint8_t>({255,0,0,255,0,255,0,255,170,85,0,255,85,170,0,255}), "Source DXT1 block decoding failed");
    const uint8_t transparent[] = {0,0,255,255,3,0,0,0};
    require(prepareSourceTexture(IMAGE_FORMAT_DXT1_ONEBITALPHA,1,1,transparent,8).pixels[3] == 0, "Source BC1 one-bit alpha failed");
    uint8_t bc2[] = {0x10,0,0,0,0,0,0,0, 0,0xf8,0,0,0,0,0,0};
    const auto alpha4 = prepareSourceTexture(IMAGE_FORMAT_DXT3,2,1,bc2,sizeof(bc2));
    require(alpha4.pixels[3] == 0 && alpha4.pixels[7] == 17, "Source DXT3 alpha failed");
    uint8_t bc3[] = {0,255,62,0,0,0,0,0, 0,0xf8,0,0,0,0,0,0};
    const auto alpha8 = prepareSourceTexture(IMAGE_FORMAT_DXT5,2,1,bc3,sizeof(bc3));
    require(alpha8.pixels[3] == 0 && alpha8.pixels[7] == 255, "Source DXT5 six-alpha mode failed");
    bc3[0] = 255; bc3[1] = 0; bc3[2] = 2 | (3 << 3);
    const auto alphaInterpolated = prepareSourceTexture(IMAGE_FORMAT_DXT5,2,1,bc3,sizeof(bc3));
    require(alphaInterpolated.pixels[3] == 218 && alphaInterpolated.pixels[7] == 182, "Source DXT5 eight-alpha mode failed");
    rejected([&] { prepareSourceTexture(IMAGE_FORMAT_DXT5,5,4,bc3,sizeof(bc3)); });
    rejected([&] { prepareSourceTexture(IMAGE_FORMAT_RGBA8888,UINT32_MAX,UINT32_MAX,bgra,sizeof(bgra)); });
}
struct MaterialFixtures {
    SourceSnapshot opaque, blend, cutout, present, forcedOpaque;
    size_t count = 0;
};
MaterialFixtures sourceMaterialChecks() {
    SourceShadow shadow(1.5f, 8);
    IShaderShadow& api = shadow.interface(); // Exercise the actual Source vtable.
    require(api.GetLightMapScaleFactor() == 1.5f, "Source lightmap scale was lost");
    rejected([&] { shadow.snapshot(); }); // No fixed-function fallback.
    api.SetDefaultState();
    api.VertexShaderVertexFormat(VERTEX_POSITION | VERTEX_COLOR, 1, nullptr, 0);
    api.SetVertexShader("FRAMEWORK_VS", 7); api.SetPixelShader("framework_ps", 3);
    api.EnableTexture(SHADER_SAMPLER0, true); api.EnableCulling(false); api.EnableAlphaWrites(true);
    MaterialFixtures result;
    result.opaque = shadow.snapshot();
    // Inactive settings do not allocate new snapshots, but later enable calls
    // still use the raw settings and leave earlier captures unchanged.
    api.BlendFunc(SHADER_BLEND_SRC_ALPHA, SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
    api.AlphaFunc(SHADER_ALPHAFUNC_LESS, .2f);
    api.EnableSRGBRead(SHADER_SAMPLER1, true);
    require(shadow.snapshot() == result.opaque, "Inactive Source state defeated snapshot interning");
    api.EnableAlphaTest(true); api.AlphaFunc(SHADER_ALPHAFUNC_GEQUAL, .7f);
    result.cutout = shadow.snapshot();
    require(result.cutout.state().alpha.reference == 178.0f / 255.0f && !result.opaque.state().alpha.enabled,
        "Source alpha quantization or immutable snapshot was lost");
    api.EnableAlphaTest(false); api.EnableDepthWrites(false); api.EnableBlending(true);
    api.EnableBlendingSeparateAlpha(true); api.BlendFuncSeparateAlpha(SHADER_BLEND_ONE, SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
    result.blend = shadow.snapshot();
    api.EnableBlendingForceOpaque(true);
    result.forcedOpaque = shadow.snapshot();
    require(result.blend.translucent() && !result.forcedOpaque.translucent() && result.forcedOpaque.state().blend,
        "Source force-opaque blending changed draw classification or blend state");
    api.EnableBlending(false); api.EnableDepthTest(false);
    result.present = shadow.snapshot();
    rejected([&] { api.EnableTexture(SHADER_SAMPLER_INVALID, true); });
    rejected([&] { api.FogMode(static_cast<ShaderFogMode_t>(-1), false); });
    rejected([&] { api.EnablePolyOffset(static_cast<PolygonOffsetMode_t>(-1)); });
    require(shadow.snapshot() == result.present, "Rejected Source state partially modified a snapshot");
    result.count = shadow.snapshotCount();
    require(result.count == 5, "Unexpected Source snapshot growth");
    shadow.clearSnapshots();
    require(!shadow.snapshotCount() && result.opaque.state().depthWrite, "Clearing Source snapshots invalidated live materials");
    return result;
}
struct ShaderBytes : IShaderBuffer {
    std::vector<uint8_t> bytes;
    explicit ShaderBytes(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(file), {});
        require(!bytes.empty(), "Cannot read offline shader for IShaderDevice");
    }
    size_t GetSize() const override { return bytes.size(); }
    const void* GetBits() const override { return bytes.data(); }
    void Release() override { throw std::runtime_error("IShaderDevice incorrectly released its caller-owned input"); }
};
struct DeviceObserver : IShaderDeviceDependentObject {
    int resizes = 0;
    void DeviceLost() override { throw std::runtime_error("Vulkan resource suspension issued a D3D device-loss event"); }
    void DeviceReset(void*, void*, void*) override { throw std::runtime_error("Vulkan resource suspension issued a D3D reset event"); }
    void ScreenSizeChanged(int width, int height) override { require(width > 0 && height > 0, "Invalid Source screen size"); ++resizes; }
};
void exercise(Context& context, SDL_Window* window, const std::filesystem::path& shaders, const std::filesystem::path& output, const LogSink& log) {
    sourceDataChecks();
    const auto materials = sourceMaterialChecks();
    GraphicsDevice graphics(context, (output / "pipeline-cache.bin").string());
    SourceShaderLibrary library(graphics, shaders.string());
    FrameArena dynamic(context, 4096, 16384);
    SourceShadow deviceShadow;
    SourceDevice sourceDevice(context, graphics, dynamic, deviceShadow, window);
    auto& device = sourceDevice.interface();
    auto& manager = sourceDevice.manager();
    require(manager.Connect([](const char*, int*) -> void* { return nullptr; }) && manager.Init() == INIT_OK, "Source device manager did not initialize");
    MaterialAdapterInfo_t adapter;
    manager.GetAdapterInfo(0, adapter);
    require(manager.GetAdapterCount() == 1 && adapter.m_VendorID == context.capabilities().properties.vendorID, "Source adapter info is not the attached Vulkan device");
    ShaderDeviceInfo_t mode;
    mode.m_bWindowed = mode.m_bResizing = mode.m_bWaitForVSync = true;
    device.GetWindowSize(mode.m_DisplayMode.m_nWidth, mode.m_DisplayMode.m_nHeight);
    const auto factory = manager.SetMode(window, 0, mode);
    int interfaceStatus = -1;
    require(factory && factory(SHADER_DEVICE_INTERFACE_VERSION, &interfaceStatus) == &device && interfaceStatus == IFACE_OK,
            "Source SetMode factory did not expose ShaderDevice001");
    require(factory(SHADERSHADOW_INTERFACE_VERSION, nullptr) == &deviceShadow.interface(), "Source device factory lost ShaderShadow010");
    require(!factory("ShaderApi029", &interfaceStatus) && interfaceStatus == IFACE_FAILED, "Incomplete Source shader API was advertised");
    DeviceObserver observer;
    manager.AddDeviceDependentObject(&observer);
    {
        SourceDeviceLimits limits;
        limits.maximumBufferBytes = limits.maximumShadowBytes = 64;
        limits.dynamicVertexBytes = 32; limits.dynamicIndexBytes = 16;
        SourceDevice limited(context, graphics, dynamic, deviceShadow, window, limits);
        auto& limitedAPI = limited.interface();
        auto* buffer = limitedAPI.CreateVertexBuffer(SHADER_BUFFER_TYPE_STATIC, VERTEX_POSITION, 2, "BudgetProbe");
        VertexDesc_t lock;
        require(buffer->Lock(2, false, lock), "Budgeted Source lock failed");
        require(limited.statistics().shadowBytes == 48, "Source CPU budget omitted active lock storage");
        rejected([&] { limitedAPI.CreateIndexBuffer(SHADER_BUFFER_TYPE_STATIC, MATERIAL_INDEX_FORMAT_32BIT, 6, "BudgetProbe"); });
        buffer->Unlock(2, lock);
        require(limited.statistics().shadowBytes == 24, "Static lock staging was retained after unlock");
        auto* freedCapacity=limitedAPI.CreateIndexBuffer(SHADER_BUFFER_TYPE_STATIC,MATERIAL_INDEX_FORMAT_32BIT,6,"BudgetProbe");
        limitedAPI.DestroyIndexBuffer(freedCapacity);
        limitedAPI.DestroyVertexBuffer(buffer);
        require(limited.statistics().shadowBytes == 0, "Source CPU budget was not released");
    }
    {
        ShaderBytes vs(shaders / "framework_vs.spv"), ps(shaders / "framework_ps.spv");
        const auto vh = device.CreateVertexShader(&vs);
        const auto ph = device.CreatePixelShader(&ps);
        require(sourceDevice.shader(vh, SourceShaderStage::Vertex).stage() == VK_SHADER_STAGE_VERTEX_BIT, "Source shader buffer was not imported");
        rejected([&] { device.CreateVertexShader(&ps); });
        device.DestroyVertexShader(vh); device.DestroyPixelShader(ph);
        rejected([&] { sourceDevice.shader(vh, SourceShaderStage::Vertex); });
    }
    SourceConstants registers(context, dynamic);
    const float floatRegisters[4] = {1,1,1,1};
    const int32_t integerRegisters[4] = {1,0,0,0}, booleanRegister = -7;
    for (auto stage : {SourceShaderStage::Vertex, SourceShaderStage::Pixel}) {
        registers.floats(stage, stage == SourceShaderStage::Vertex ? 255 : 223, floatRegisters);
        registers.integers(stage,15,integerRegisters); registers.booleans(stage,15,&booleanRegister);
        const auto version = registers.revision(stage);
        registers.integers(stage,15,integerRegisters);
        require(registers.revision(stage) == version, "Identical Source register values dirtied the bank");
    }
    rejected([&] { registers.floats(SourceShaderStage::Pixel,224,floatRegisters); });
    DescriptorArena descriptors(context, 1, 2); // Force growth, then recycle four pools across two frame slots.
    auto vertex = library.shader(SourceShaderStage::Vertex, materials.opaque.state().vertex, 2);
    auto fragment = library.shader(SourceShaderStage::Pixel, materials.opaque.state().pixel, 5);
    require(fragment.bindings().size() == 3, "Shader reflection did not find the uniform, texture and sampler bindings");
    auto setLayout = graphics.descriptorLayout({
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT},
        {3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT}});
    auto layout = graphics.pipelineLayout({setLayout}, sizeof(Push));
    SamplerDescription samplerDescription;
    samplerDescription.minFilter = samplerDescription.magFilter = VK_FILTER_NEAREST;
    samplerDescription.mipFilter = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerDescription.addressU = samplerDescription.addressV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerDescription.maxLod = 0;
    auto sampler = graphics.sampler(samplerDescription);
    const Vertex vertices[] = {{{-.3f,-.6f,0},{128,255,255,255},{0,0}}, {{.3f,-.6f,0},{128,255,255,255},{1,0}},
                               {{.3f,.6f,0},{128,255,255,255},{1,1}}, {{-.3f,.6f,0},{128,255,255,255},{0,1}}};
    const uint16_t indices[] = {0,1,2,2,3,0};
    const uint8_t texels[] = {255,255,255,255, 128,128,128,255, 64,64,64,255, 192,192,192,255};
    SourceVertexFormatDescription sourceDescription;
    sourceDescription.color = sourceDescription.exact = true; sourceDescription.texCoords[0] = 2;
    const auto sourceFormat = sourceVertexFormat(sourceDescription);
    IMesh* mesh = device.CreateStaticMesh(sourceFormat, "VulkanProbe");
    MeshDesc_t meshLock;
    mesh->LockMesh(4, 6, meshLock);
    rejected([&] { sourceDevice.flushUploads(); });
    for (size_t i = 0; i < 4; ++i) {
        std::memcpy(reinterpret_cast<uint8_t*>(meshLock.m_pPosition) + i * meshLock.m_VertexSize_Position, vertices[i].position, sizeof(vertices[i].position));
        std::memcpy(meshLock.m_pColor + i * meshLock.m_VertexSize_Color, vertices[i].color, sizeof(vertices[i].color));
        std::memcpy(reinterpret_cast<uint8_t*>(meshLock.m_pTexCoord[0]) + i * meshLock.m_VertexSize_TexCoord[0], vertices[i].uv, sizeof(vertices[i].uv));
    }
    std::memcpy(meshLock.m_pIndices, indices, sizeof(indices));
    mesh->UnlockMesh(4, 6, meshLock);
    mesh->ModifyBeginEx(true, 0, 1, 0, 0, meshLock);
    meshLock.m_pColor[0] = 1; // Read-only edits must never dirty or overwrite the committed mesh.
    mesh->ModifyEnd(meshLock);
    mesh->ModifyBeginEx(true, 0, 1, 0, 0, meshLock);
    require(meshLock.m_pColor[0] == 128, "Read-only mesh access overwrote committed data");
    mesh->ModifyEnd(meshLock);
    SourceVertexFormatDescription instanceDescription;
    instanceDescription.position = false; instanceDescription.exact = true;
    instanceDescription.texCoords[0] = 2; instanceDescription.texCoords[1] = 4;
    auto* instanceBuffer = device.CreateVertexBuffer(SHADER_BUFFER_TYPE_DYNAMIC, sourceVertexFormat(instanceDescription), 8, "VulkanProbe");
    auto* index32 = device.CreateIndexBuffer(SHADER_BUFFER_TYPE_DYNAMIC, MATERIAL_INDEX_FORMAT_32BIT, 6, "VulkanProbe");
    IndexDesc_t indexLock;
    require(index32->Lock(6, false, indexLock) && indexLock.m_nIndexSize == 2, "Source 32-bit index lock failed");
    for (size_t i = 0; i < 6; ++i) { const uint32_t value = indices[i]; std::memcpy(reinterpret_cast<uint8_t*>(indexLock.m_pIndices) + i * 4, &value, 4); }
    index32->Unlock(6, indexLock);
    const auto preparedTexture = prepareSourceTexture(IMAGE_FORMAT_RGBA8888,2,2,texels,sizeof(texels));
    auto texture = context.createImage(preparedTexture.description);
    materials.opaque.validateTexture(SourceShaderStage::Pixel, 0, texture);
    rejected([&] { materials.opaque.validateTexture(SourceShaderStage::Pixel, 1, texture); });
    const auto submissions = context.statistics().queueSubmissions;
    const auto allocations = context.allocationStatistics().allocations;
    rejected([&] {
        sourceDevice.flushUploads([](UploadBatch&) { throw std::logic_error("Injected upload recording failure"); });
    });
    require(context.statistics().queueSubmissions==submissions && context.allocationStatistics().allocations==allocations &&
        sourceDevice.statistics().bufferUploads==0,"Failed upload recording committed or leaked pending resources");
    sourceDevice.flushUploads([&](UploadBatch& batch) {
        ImageUse sampled;
        sampled.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sampled.access = VK_ACCESS_SHADER_READ_BIT;
        sampled.stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        batch.image(texture, 0, 0, preparedTexture.pixels.data(), preparedTexture.pixels.size(), {}, sampled);
    });
    require(context.statistics().queueSubmissions == submissions + 1, "Three uploads did not share one queue submission");
    const auto inspections=sourceDevice.statistics().uploadInspections;
    sourceDevice.flushUploads();
    require(sourceDevice.statistics().uploadInspections==inspections && context.statistics().queueSubmissions==submissions+1,
        "Unchanged Source buffers were inspected or submitted again");
    RenderTargetDescription targetDescription;
    targetDescription.width = Width; targetDescription.height = Height;
    targetDescription.depthFormat = VK_FORMAT_D32_SFLOAT;
    targetDescription.colorFinalLayout = targetDescription.depthFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    RenderTarget target(context, targetDescription);
    auto pixels = context.createBuffer(Width * Height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryAccess::Readback);
    auto depths = context.createBuffer(Width * Height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryAccess::Readback);
    GraphicsPipelineDescription description;
    description.vertex = vertex; description.fragment = fragment; description.layout = layout;
    description.renderPass = target.renderPass(); description.colorFormat = targetDescription.colorFormat;
    description.depthFormat = targetDescription.depthFormat; description.depthTest = description.depthWrite = true;
    const auto sourceLayout = sourceVertexLayout(sourceFormat);
    materials.opaque.validateVertexFormat(sourceFormat);
    auto missingColor = sourceDescription;
    missingColor.color = false;
    rejected([&] { materials.opaque.validateVertexFormat(sourceVertexFormat(missingColor)); });
    require(sourceLayout.stride == sizeof(Vertex), "Source unlit vertex packing does not match the mesh");
    description.vertexBindings = {{0, sourceLayout.stride, VK_VERTEX_INPUT_RATE_VERTEX}, {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE}};
    description.attributes = sourceLayout.attributes({{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1},{SourceSemantic::Color,0,4}});
    description.attributes.push_back({2,1,VK_FORMAT_R32G32_SFLOAT,0});
    description.attributes.push_back({3,1,VK_FORMAT_R32G32B32A32_SFLOAT,8});
    auto opaque = library.pipeline(materials.opaque, description, 2, 5);
    require(library.pipeline(materials.opaque, description, 2, 5).handle() == opaque.handle(), "Equivalent pipeline description was not cached");
    const auto cutout = library.pipeline(materials.cutout, description, 2, 5);
    require(cutout.handle() == opaque.handle(), "Shader alpha state unnecessarily created another Vulkan pipeline");
    auto blend = library.pipeline(materials.blend, description, 2, 5);
    require(library.pipeline(materials.forcedOpaque, description, 2, 5).handle() == blend.handle(), "Force-opaque sorting duplicated a blend pipeline");
    rejected([&] { library.pipeline(materials.opaque, description); }); // Missing dynamic combo does not fall back.
    rejected([&] { library.shader(SourceShaderStage::Pixel, materials.opaque.state().pixel, 0x80000000u); });
    rejected([&] { library.pipeline(materials.cutout, description, 2, 6); }); // This variant does not implement alpha test.
    auto wrongColorSpace = description;
    wrongColorSpace.colorFormat = VK_FORMAT_R8G8B8A8_SRGB;
    rejected([&] { library.pipeline(materials.opaque, wrongColorSpace, 2, 5); });
    const auto badLayout = graphics.descriptorLayout({{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT}});
    auto bad = description;
    bad.layout = graphics.pipelineLayout({badLayout}, sizeof(Push));
    rejected([&] { graphics.pipeline(bad); });
    rejected([&] { UploadBatch invalid(context, 0); });
    DescriptorSet retired;
    uint64_t drawCalls = 0, ordinaryFrames = 0, stableDynamicBytes = 0;
    bool recycledSetRejected = false, resized = false, refreshed = false;
    Buffer originalGeometry;
    const auto started = SDL_GetTicks();
    uint32_t frameNumber = 0;
    while (frameNumber < Frames) {
        require(SDL_GetTicks() - started < 20000, "Graphics integration test timed out");
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            require(event.type != SDL_EVENT_QUIT && event.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED, "Graphics integration window was closed");
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) context.requestResize();
        }
        // One resize and one true Surface replacement, with the same shader and
        // material resources. No pipeline rebuild is needed for compatible passes.
        if (frameNumber == 30 && !resized) {
            mesh->ModifyBegin(0, 4, 0, 0, meshLock);
            for (size_t i = 0; i < 4; ++i) meshLock.m_pColor[i * meshLock.m_VertexSize_Color] = 64;
            mesh->ModifyEnd(meshLock);
            require(SDL_SetWindowSize(window, 800, 450), SDL_GetError());
            context.requestResize();
            resized = true;
        }
        if (frameNumber == 60 && !refreshed) {
            device.ReleaseResources(true);
            Frame paused;
            require(!sourceDevice.beginFrame(paused), "Released Source device continued presenting");
            device.ReacquireResources();
            context.refreshSurface(); refreshed = true;
        }
        Frame frame;
        const auto before = context.statistics();
        if (!sourceDevice.beginFrame(frame)) { SDL_Delay(2); continue; }
        const auto geometry = sourceDevice.vertexSlice(mesh, frame);
        if (frameNumber == 0) originalGeometry = geometry.buffer;
        if (frameNumber >= 30) require(geometry.buffer.handle() != originalGeometry.handle(), "Source mesh update overwrote an earlier GPU allocation");
        DrawEncoder* currentDraw = nullptr;
        uint32_t instanceCount = 2;
        bool useIndex32 = false;
        sourceDevice.setDrawSink([&](const SourceMeshDraw& request) {
            require(currentDraw && request.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, "Source mesh drew without a matching pass/pipeline");
            currentDraw->vertexBuffer(0, sourceDevice.vertexSlice(request.mesh, frame));
            const IIndexBuffer* elements = useIndex32 ? index32 : static_cast<IIndexBuffer*>(request.mesh);
            currentDraw->indexBuffer(sourceDevice.indexSlice(elements, frame), sourceDevice.indexType(elements));
            currentDraw->drawIndexed(request.indexCount, instanceCount, request.firstIndex);
        });
        const Instance instances[] = {{{-.5f,0},{1,0,0,1}}, {{.5f,0},{0,1,0,1}}};
        const Instance hidden[] = {{{-.5f,0},{0,0,1,1}}, {{.5f,0},{0,0,1,1}}};
        const Instance overlay[] = {{{0,0},{0,0,1,.5f}}};
        const Instance alphaTested[] = {{{-.5f,0},{0,0,1,.5f}}, {{.5f,0},{0,0,1,1}}};
        const std::array<float, 4> zero {};
        dynamic.write(frame, zero.data(), sizeof(zero));
        const auto vsConstants = registers.snapshot(SourceShaderStage::Vertex,frame);
        const auto psConstants = registers.snapshot(SourceShaderStage::Pixel,frame);
        const auto repeated = registers.snapshot(SourceShaderStage::Pixel,frame);
        require(psConstants.buffer.handle() == repeated.buffer.handle() && psConstants.offset == repeated.offset, "Source constant bank was reuploaded without changes");
        require(psConstants.offset && psConstants.offset <= UINT32_MAX && vsConstants.offset <= UINT32_MAX, "Dynamic Source register offset was not exercised");
        auto writeInstances = [&](const Instance* values, int count) {
            VertexDesc_t lock;
            require(instanceBuffer->Lock(count, false, lock), "Dynamic Source instance lock failed");
            for (int i = 0; i < count; ++i) {
                std::memcpy(reinterpret_cast<uint8_t*>(lock.m_pTexCoord[0]) + i * lock.m_VertexSize_TexCoord[0], values[i].offset, sizeof(values[i].offset));
                std::memcpy(reinterpret_cast<uint8_t*>(lock.m_pTexCoord[1]) + i * lock.m_VertexSize_TexCoord[1], values[i].color, sizeof(values[i].color));
            }
            instanceBuffer->Unlock(count, lock);
            return sourceDevice.vertexSlice(instanceBuffer, frame);
        };
        const auto objects = writeInstances(instances, 2);
        const auto unchanged = sourceDevice.vertexSlice(instanceBuffer, frame);
        require(objects.buffer.handle() == unchanged.buffer.handle() && objects.offset == unchanged.offset, "Unchanged dynamic Source data was copied twice");
        const auto occluded = writeInstances(hidden, 2);
        const auto translucent = writeInstances(overlay, 1);
        const auto masked = writeInstances(alphaTested, 2);
        sourceDevice.indexSlice(index32, frame);
        DescriptorWrite uniformVS;
        uniformVS.binding = 0; uniformVS.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        uniformVS.buffer = {vsConstants.buffer,0,vsConstants.size};
        DescriptorWrite uniformPS;
        uniformPS.binding = 1; uniformPS.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        uniformPS.buffer = {psConstants.buffer,0,psConstants.size};
        DescriptorWrite image;
        image.binding = 2; image.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; image.image = texture;
        DescriptorWrite sampling;
        sampling.binding = 3; sampling.type = VK_DESCRIPTOR_TYPE_SAMPLER; sampling.sampler = sampler;
        const auto set = descriptors.allocate(frame, setLayout, {uniformVS, uniformPS, image, sampling});
        const auto secondSet = descriptors.allocate(frame, setLayout, {uniformVS, uniformPS, image, sampling});
        if (frameNumber == 0) {
            retired = set;
            rejected([&] { descriptors.allocate(frame, setLayout, {uniformVS, uniformPS, image, sampling}); });
        }
        if (frameNumber == 2) { rejected([&] { retired.handle(); }); retired = {}; recycledSetRejected = true; }
        const std::vector<uint32_t> offsets {uint32_t(vsConstants.offset),uint32_t(psConstants.offset)};
        if (frameNumber == 3) stableDynamicBytes = dynamic.allocatedBytes();
        target.begin(frame, {0,0,0,1});
        {
            DrawEncoder draw(context, frame, target.extent(), targetDescription.colorFormat, targetDescription.depthFormat);
            currentDraw = &draw;
            draw.pipeline(opaque);
            draw.vertexBuffer(1, objects);
            draw.descriptors(0, set, offsets);
            Push push {{1,1,0,0}, {.25f,0,0,0}};
            draw.pushConstants(&push, sizeof(push));
            mesh->Draw(); // Source IMesh emits two actual instances in one command.
            draw.vertexBuffer(1, occluded);
            push.parameters[0] = .75f;
            draw.pushConstants(&push, sizeof(push));
            mesh->Draw(); // Rejected by depth, must not replace red/green.
            draw.pipeline(cutout);
            draw.vertexBuffer(1, masked);
            push = {{.4f,.1f,0,.86f}, {.2f,0,0,0}, materials.cutout.state().alpha};
            draw.pushConstants(&push, sizeof(push));
            useIndex32 = true;
            mesh->Draw(); // Source 32-bit indices: one instance is discarded, the other writes color/depth.
            draw.pipeline(blend);
            draw.vertexBuffer(1, translucent);
            draw.descriptors(0, secondSet, offsets);
            push = {{.4f,.2f,0,0}, {.1f,0,0,0}};
            draw.pushConstants(&push, sizeof(push));
            draw.scissor({{64,0},{64,Height}});
            useIndex32 = false; instanceCount = 1;
            mesh->Draw();
            drawCalls += draw.drawCalls();
        }
        target.end(frame);
        if (frameNumber + 1 == Frames) {
            context.retain(frame, pixels); context.retain(frame, depths);
            VkBufferImageCopy copy {};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.imageExtent = {Width,Height,1};
            context.vk().vkCmdCopyImageToBuffer(frame.commands, target.color().handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels.handle(), 1, &copy);
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            context.vk().vkCmdCopyImageToBuffer(frame.commands, target.depth().handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depths.handle(), 1, &copy);
            VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            context.vk().vkCmdPipelineBarrier(frame.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        }
        context.beginPresentPass(frame, {.025f,.025f,.025f,1});
        {
            auto presentDescription = description;
            presentDescription.renderPass = frame.renderPass; presentDescription.colorFormat = frame.colorFormat;
            presentDescription.depthFormat = VK_FORMAT_UNDEFINED;
            presentDescription.depthTest = presentDescription.depthWrite = presentDescription.blend = false;
            const auto present = library.pipeline(materials.present, presentDescription, 2, 5);
            DrawEncoder draw(context, frame);
            currentDraw = &draw; instanceCount = 2;
            draw.pipeline(present);
            draw.vertexBuffer(1, objects);
            draw.descriptors(0, set, offsets);
            Push push {{1,1,0,0},{.25f,0,0,0}};
            draw.pushConstants(&push,sizeof(push)); mesh->Draw();
            drawCalls += draw.drawCalls();
        }
        context.endPresentPass(frame);
        if (frameNumber + 1 == Frames) {
            // Destruction after recording but before submission must leave the
            // frame's retained GPU buffers valid through its completion fence.
            device.DestroyStaticMesh(mesh);
            device.DestroyVertexBuffer(instanceBuffer); device.DestroyIndexBuffer(index32);
            rejected([&] { device.DestroyStaticMesh(mesh); });
        }
        device.Present();
        sourceDevice.setDrawSink({});
        if (before.swapchainGeneration == context.statistics().swapchainGeneration) {
            require(before.idleWaits == context.statistics().idleWaits, "Graphics frame performed a device-wide wait");
            ++ordinaryFrames;
        }
        ++frameNumber;
    }
    manager.RemoveDeviceDependentObject(&observer);
    manager.Shutdown(); manager.Disconnect(); // Explicit shutdown wait before diagnostic readback.
    std::vector<uint8_t> rgba(Width * Height * 4);
    std::vector<float> depth(Width * Height);
    pixels.read(0, rgba.data(), rgba.size()); depths.read(0, depth.data(), depth.size() * sizeof(float));
    std::ofstream ppm(output / "graphics-readback.ppm", std::ios::binary);
    ppm << "P6\n" << Width << ' ' << Height << "\n255\n";
    for (size_t i = 0; i < rgba.size(); i += 4) ppm.write(reinterpret_cast<const char*>(rgba.data() + i), 3);
    ppm.close(); require(bool(ppm), "Cannot save graphics readback");
    auto pixel = [&](uint32_t x, uint32_t y, std::array<int,4> expected) {
        for (size_t c = 0; c < 4; ++c) {
            if (std::abs(int(rgba[(y * Width + x) * 4 + c]) - expected[c]) > 1)
                throw std::runtime_error("Graphics pixel mismatch at " + std::to_string(x) + "," + std::to_string(y) +
                    " channel=" + std::to_string(c) + " actual=" + std::to_string(rgba[(y * Width + x) * 4 + c]) + " expected=" + std::to_string(expected[c]));
        }
    };
    pixel(24,20,{64,0,0,255}); pixel(40,20,{192,0,0,255}); pixel(24,44,{255,0,0,255}); pixel(40,44,{128,0,0,255});
    pixel(88,20,{0,64,0,255}); pixel(104,20,{0,192,0,255}); pixel(88,44,{0,255,0,255}); pixel(104,44,{0,128,0,255});
    pixel(60,32,{0,0,0,255}); pixel(66,30,{0,0,24,255});
    pixel(28,3,{0,0,0,255}); pixel(92,3,{0,0,16,255});
    require(std::abs(depth[20 * Width + 24] - .25f) < .0001f && std::abs(depth[32 * Width + 66] - 1) < .0001f,
            "Depth rejection or translucent depth-write state failed");
    require(std::abs(depth[3 * Width + 28] - 1) < .0001f && std::abs(depth[3 * Width + 92] - .2f) < .0001f,
            "Alpha-discarded fragments wrote depth or passing alpha-test fragments did not");
    require(recycledSetRejected && descriptors.poolCount() == 4 && dynamic.allocatedBytes() == stableDynamicBytes && stableDynamicBytes <= 32768,
            "Frame resource pools did not recycle within their budgets");
    require(registers.uploads() == Frames * 2, "Unchanged Source constant banks were uploaded more than once per frame");
    require(graphics.statistics().pipelineCreations == 3 && graphics.statistics().pipelineHits >= Frames,
            "Compatible render passes did not reuse cached pipelines");
    require(library.variantCount() >= 3 && library.loadedShaders() == 2, "Source combo lookup did not reuse its shader modules");
    const auto resources = sourceDevice.statistics();
    require(resources.uploadSubmissions == 3 && resources.bufferUploads == 5 && resources.dynamicCopies == Frames * 5 &&
            resources.meshDraws == drawCalls && !resources.shadowBytes && !resources.vertexBuffers && !resources.indexBuffers &&
            !resources.meshes && !resources.shaders && observer.resizes >= 2, "Source device resources did not update/recycle/notify as expected");
    graphics.savePipelineCache((output / "pipeline-cache.bin").string());
    GraphicsDevice reloaded(context, (output / "pipeline-cache.bin").string());
    require(reloaded.statistics().loadedCacheBytes > 0, "Saved driver pipeline cache was not accepted");
    log("VK_GRAPHICS_PASS: frames=" + std::to_string(frameNumber) + " draws=" + std::to_string(drawCalls) +
        " instances_per_geometry_draw=2 upload_submissions=" + std::to_string(resources.uploadSubmissions) + " pipelines=" + std::to_string(graphics.statistics().pipelineCreations) +
        " pipeline_hits=" + std::to_string(graphics.statistics().pipelineHits) + " descriptor_pools=" + std::to_string(descriptors.poolCount()) +
        " dynamic_bytes=" + std::to_string(dynamic.allocatedBytes()) + " steady_frames=" + std::to_string(ordinaryFrames) +
        " cache_bytes=" + std::to_string(reloaded.statistics().loadedCacheBytes));
    log("VK_SOURCE_BRIDGE_PASS: legacy_vertex_stride=" + std::to_string(sourceLayout.stride) +
        " constant_uploads=" + std::to_string(registers.uploads()) + " vs_bytes=" + std::to_string(SourceVertexRegisterBytes) +
        " ps_bytes=" + std::to_string(SourcePixelRegisterBytes) + " formats=BGRA/I8/A8/DXT1/DXT3/DXT5 shader_registers=c255/c223/i15/b15");
    log("VK_SOURCE_MATERIAL_PASS: snapshots=" + std::to_string(materials.count) + " variants=" + std::to_string(library.variantCount()) +
        " loaded_shaders=" + std::to_string(library.loadedShaders()) + " interface=ShaderShadow010 vs_combo=7/2 ps_combo=3/5 alpha_test=color/depth");
    log("VK_SOURCE_DEVICE_PASS: interfaces=ShaderDeviceMgr001/ShaderDevice001 mesh_draws=" + std::to_string(resources.meshDraws) +
        " buffer_uploads=" + std::to_string(resources.bufferUploads) + " dynamic_copies=" + std::to_string(resources.dynamicCopies) +
        " resize_callbacks=" + std::to_string(observer.resizes) + " indices=16/32 shadow_bytes=0 destroy_before_submit=pass");
}
} // namespace

int main(int argc, char** argv) {
    SDL_Window* window = nullptr;
    std::ofstream report;
    int result = 0;
    std::atomic<uint32_t> errors {0};
    LogSink log = [&](const std::string& message) {
        if (message.rfind("VK_VALIDATION_ERROR:",0) == 0) ++errors;
        std::fprintf(stderr,"%s\n",message.c_str());
        if (report.is_open()) { report << message << '\n'; report.flush(); }
    };
    try {
        require(argc == 3, "Usage: csgo-vulkan-graphics-probe SHADER_DIRECTORY OUTPUT_DIRECTORY");
        std::filesystem::create_directories(argv[2]);
        report.open(std::filesystem::path(argv[2]) / "graphics-probe.log");
        require(bool(report), "Cannot open graphics diagnostic log");
        require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        window = SDL_CreateWindow("Source Vulkan framework: instancing and textures", 720, 400,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        require(window, SDL_GetError());
        {
            Context context(window, {true, log});
            exercise(context, window, argv[1], argv[2], log);
            require(context.allocationStatistics().allocations == 0, "Graphics framework leaked VMA allocations");
        }
        require(errors == 0, "Vulkan validation reported errors during graphics recording or cleanup");
        log("VK_GRAPHICS_CLEANUP_PASS: validation_errors=0 live_allocations=0");
    } catch (const std::exception& error) { log("VK_GRAPHICS_FAIL: " + std::string(error.what())); result = 1; }
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
