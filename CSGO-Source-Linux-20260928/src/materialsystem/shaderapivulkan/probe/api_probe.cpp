#include "vulkan_api.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

#include "materialsystem/imesh.h"
#include "mathlib/vmatrix.h"
#include "shaderapi/ishaderapi.h"
#include "shaderapi/ishadershadow.h"

namespace {
using namespace sourcevk;
constexpr uint32_t Width = 128, Height = 96, Frames = 60, DrawsPerFrame = 13;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class Function> void rejected(Function function, const char* name = "invalid operation") {
    try { function(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error(std::string("Source API accepted ") + name);
}
struct Vertex { float position[3]; uint8_t color[4]; float uv[2]; };
constexpr VertexFormat_t VertexFormat = VERTEX_POSITION | VERTEX_COLOR | VERTEX_FORMAT_USE_EXACT_FORMAT |
    (VertexFormat_t(2) << TEX_COORD_SIZE_BIT);
void fillMesh(IMesh* mesh, std::array<uint8_t,4> bgra = {255,255,255,255}) {
    Vertex vertices[] = {{{-1,-1,0},{},{0,1}}, {{1,-1,0},{},{1,1}}, {{1,1,0},{},{1,0}}, {{-1,1,0},{},{0,0}}};
    for (auto& vertex : vertices) std::copy(bgra.begin(),bgra.end(),vertex.color);
    const uint16_t indices[] = {0,1,2,0,2,3};
    MeshDesc_t lock;
    mesh->LockMesh(4,6,lock);
    require(lock.m_ActualVertexSize == int(sizeof(Vertex)), "Source API fixture vertex ABI changed");
    std::memcpy(lock.m_pPosition,vertices,sizeof(vertices));
    std::memcpy(lock.m_pIndices,indices,sizeof(indices));
    mesh->UnlockMesh(4,6,lock);
}
void rectangle(IShaderAPI& api, int column, int row, float depth, std::array<float,4> tint = {1,1,1,1}, float mip = 0) {
    const float sx = 32.0f/Width, sy = 32.0f/Height;
    const float matrix[16] = {sx,0,0,(column*64.0f+32)/Width-1, 0,sy,0,1-(row*64.0f+32)/Height,
        0,0,1,depth, 0,0,0,1};
    const float lod[4] = {mip,0,0,0};
    api.SetVertexShaderConstant(0,matrix,4);
    api.SetPixelShaderConstant(0,tint.data()); api.SetPixelShaderConstant(1,lod);
}
void readback(Context& context, const Frame& frame, RenderTarget& target, const Buffer& color, const Buffer& depth = {}) {
    context.retain(frame,color);
    VkBufferImageCopy copy {};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent = {Width,Height,1};
    context.vk().vkCmdCopyImageToBuffer(frame.commands,target.color().handle(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,color.handle(),1,&copy);
    if (depth) {
        context.retain(frame,depth); copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        context.vk().vkCmdCopyImageToBuffer(frame.commands,target.depth().handle(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,depth.handle(),1,&copy);
    }
    VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    context.vk().vkCmdPipelineBarrier(frame.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
}
void savePPM(const std::filesystem::path& path, const std::vector<uint8_t>& rgba) {
    std::ofstream output(path,std::ios::binary);
    output << "P6\n" << Width << ' ' << Height << "\n255\n";
    for (size_t i=0;i<rgba.size();i+=4) output.write(reinterpret_cast<const char*>(rgba.data()+i),3);
    output.close(); require(bool(output),"Cannot save Source API GPU readback");
}

void exercisePreFrameClears(Context& context,const LogSink& log) {
    ImageDescription description;description.width=description.height=4;
    description.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto color=context.createImage(description);
    description.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
    description.aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    description.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto depth=context.createImage(description);
    const VkRect2D whole {{0,0},{4,4}},middle {{1,1},{2,2}},corner {{0,0},{1,1}};
    // The first partial clear defines untouched pixels without overwriting
    // neighboring atlas tiles in later clears. These images have no transfer-dst usage.
    clearAttachmentRegion(context,color,middle,{0,1,0,.5f});
    clearAttachmentRegion(context,color,corner,{1,0,0,1});
    clearAttachmentRegion(context,depth,whole,{},description.aspect,.25f,7);
    clearAttachmentRegion(context,depth,middle,{},VK_IMAGE_ASPECT_DEPTH_BIT,.75f);
    clearAttachmentRegion(context,depth,corner,{},VK_IMAGE_ASPECT_STENCIL_BIT,1,9);
    rejected([&] { clearAttachmentRegion(context,color,{{3,0},{2,1}},{1,1,1,1}); },"an out-of-bounds pre-frame clear");
    const auto submissions=context.statistics().queueSubmissions;
    clearAttachmentRegion(context,color,{{4,4},{0,0}},{1,1,1,1});
    require(context.statistics().queueSubmissions==submissions,"Empty pre-frame clear submitted GPU work");
    auto readback=context.createBuffer(144,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    context.submitAndWait([&](VkCommandBuffer commands) {
        auto read=[&](const Image& image,VkImageLayout layout) {
            VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout=layout;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.image=image.handle();barrier.subresourceRange={image.description().aspect,0,1,0,1};
            context.vk().vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            VkBufferImageCopy copies[2] {};copies[0].imageExtent=copies[1].imageExtent={4,4,1};
            const bool isColor=image.description().aspect==VK_IMAGE_ASPECT_COLOR_BIT;
            copies[0].bufferOffset=isColor?0:64;
            copies[0].imageSubresource={isColor?VK_IMAGE_ASPECT_COLOR_BIT:VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
            copies[1].bufferOffset=128;copies[1].imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1};
            context.vk().vkCmdCopyImageToBuffer(commands,image.handle(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle(),isColor?1:2,copies);
        };
        read(color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);read(depth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkMemoryBarrier host {VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        context.vk().vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    });
    std::array<uint8_t,144> pixels {};readback.read(0,pixels.data(),pixels.size());
    for(int y=0;y<4;++y)for(int x=0;x<4;++x) {
        const int index=y*4+x;const bool inside=x>=1 && x<=2 && y>=1 && y<=2;
        const std::array<int,4> expected=index==0?std::array<int,4>{255,0,0,255}:
            inside?std::array<int,4>{0,255,0,128}:std::array<int,4>{0,0,0,0};
        for(int channel=0;channel<4;++channel)
            require(std::abs(int(pixels[index*4+channel])-expected[channel])<=1,"Pre-frame partial color clear lost neighboring pixels");
        float value;std::memcpy(&value,pixels.data()+64+index*4,sizeof(value));
        require(std::abs(value-(inside?.75f:.25f))<1e-6f,"Pre-frame clear corrupted independent depth values");
        require(pixels[128+index]==(index==0?9:7),"Pre-frame depth clear corrupted stencil or neighboring tiles");
    }
    log("VK_SOURCE_PREFRAME_CLEAR_PASS: partial_color=pass depth_stencil_preserved=pass undefined_pixels=zero attachment_only_usage=pass");
}

void exercise(Context& context, SDL_Window* window, const std::string& shaderDirectory, const std::filesystem::path& output, const LogSink& log) {
    GraphicsDevice graphics(context,(output/"pipeline-cache.bin").string());
    FrameArena arena(context,64*1024,512*1024);
    SourceShadow shadow;
    SourceDevice device(context,graphics,arena,shadow,window);
    SourceShaderLibrary shaders(graphics,shaderDirectory);
    SourceAPILimits limits;
    limits.maximumSnapshots = 16; limits.textures.maximumTextures = 3;
    SourceAPI sourceAPI(context,graphics,arena,device,shadow,shaders,limits);
    sourceAPI.registerShaderInputs("api_fixture_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1},{SourceSemantic::Color,0,2}});
    auto& manager = device.manager();
    require(manager.Connect([](const char*,int*) -> void* { return nullptr; }) && manager.Init() == INIT_OK,"Source API manager initialization failed");
    ShaderDeviceInfo_t mode;
    mode.m_bWindowed = mode.m_bResizing = mode.m_bWaitForVSync = true;
    device.interface().GetWindowSize(mode.m_DisplayMode.m_nWidth,mode.m_DisplayMode.m_nHeight);
    auto factory = manager.SetMode(window,0,mode);
    require(factory,"Source API device factory is unavailable");
    int status = -1;
    auto* apiPointer = static_cast<IShaderAPI*>(factory(SHADERAPI_INTERFACE_VERSION,&status));
    require(apiPointer == &sourceAPI.interface() && status == IFACE_OK,"Source factory lost ShaderApi029");
    require(factory(SHADERDYNAMIC_INTERFACE_VERSION,nullptr) == static_cast<IShaderDynamicAPI*>(apiPointer),"Source factory lost ShaderDynamic001");
    IShaderAPI& api = *apiPointer;
    auto& states = shadow.interface();
    states.SetDefaultState();
    states.VertexShaderVertexFormat(VERTEX_POSITION|VERTEX_COLOR,1,nullptr,0);
    states.SetVertexShader("api_fixture_vs",0); states.SetPixelShader("api_fixture_ps",0);
    states.EnableTexture(SHADER_SAMPLER0,true); states.EnableAlphaWrites(true); states.EnableCulling(false);
    auto staleSnapshot = api.TakeSnapshot();
    api.ClearSnapshots();
    rejected([&] { api.BeginPass(staleSnapshot); },"a cleared snapshot");
    const auto opaque = api.TakeSnapshot();
    require(opaque != staleSnapshot && api.TakeSnapshot() == opaque,"Source snapshots were recycled or not interned");
    states.EnableSRGBRead(SHADER_SAMPLER0,true); const auto srgb = api.TakeSnapshot();
    states.EnableSRGBRead(SHADER_SAMPLER0,false);
    states.EnableAlphaTest(true); states.AlphaFunc(SHADER_ALPHAFUNC_GEQUAL,.7f); const auto cutout = api.TakeSnapshot();
    states.EnableAlphaTest(false); states.EnableDepthWrites(false); states.EnableBlending(true);
    states.BlendFunc(SHADER_BLEND_SRC_ALPHA,SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
    states.EnableBlendingSeparateAlpha(true); states.BlendFuncSeparateAlpha(SHADER_BLEND_ONE,SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
    const auto blend = api.TakeSnapshot();
    require(api.IsTranslucent(blend) && !api.IsTranslucent(opaque) && api.IsAlphaTested(cutout) &&
        api.IsDepthWriteEnabled(opaque) && !api.IsDepthWriteEnabled(blend) && api.UsesVertexAndPixelShaders(opaque),"Source snapshot classification failed");
    StateSnapshot_t snapshotList[] = {opaque,srgb,cutout,blend};
    require(TexCoordSize(0,api.ComputeVertexUsage(4,snapshotList)) == 2 && api.CompareSnapshots(opaque,opaque) == 0,
        "Source snapshot vertex usage or comparison failed");
    {
        // Source matrices use row vectors. Local scale must leave an existing
        // world translation intact, and each stack must restore independently.
        float matrix[16];
        const float translated[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 3,4,5,1};
        const float scaled[16] = {2,0,0,0, 0,3,0,0, 0,0,4,0, 3,4,5,1};
        api.MatrixMode(MATERIAL_MODEL); api.LoadIdentity(); api.Translate(3,4,5);
        api.PushMatrix(); api.Scale(2,3,4); api.GetMatrix(MATERIAL_MODEL,matrix);
        require(std::equal(matrix,matrix+16,scaled),"Source local scale/translation order changed");
        api.PopMatrix(); api.GetMatrix(MATERIAL_MODEL,matrix);
        require(std::equal(matrix,matrix+16,translated),"Source matrix stack did not restore translation");
        rejected([&] { api.PopMatrix(); },"matrix stack underflow");
        api.LoadIdentity();
        api.MatrixMode(MATERIAL_VIEW); api.Translate(-10,2,-3);
        float camera[3]; api.GetWorldSpaceCameraPosition(camera);
        require(camera[0]==10 && camera[1]==-2 && camera[2]==3,"Source view inverse lost the camera position");
        api.LoadIdentity();
        api.MatrixMode(MATERIAL_PROJECTION); api.PerspectiveX(90,2,1,101);
        api.GetActualProjectionMatrix(matrix);
        const auto projectedZ = [&](float z) { return (z*matrix[10]+matrix[14])/(z*matrix[11]+matrix[15]); };
        require(std::abs(matrix[0]-1)<1e-5f && std::abs(matrix[5]-2)<1e-5f &&
            std::abs(projectedZ(-1))<1e-5f && std::abs(projectedZ(-101)-1)<1e-5f,
            "Source perspective FOV, aspect or zero-to-one depth changed");
        api.LoadIdentity(); api.MatrixMode(MATERIAL_MODEL);
        rejected([&] { api.MatrixMode(MaterialMatrixMode_t(NUM_MATRIX_MODES)); },"invalid matrix mode");
    }
    api.SetAnisotropicLevel(8); // Saved settings are clamped to current hardware capabilities.
    rejected([&] { api.SetAnisotropicLevel(-1); });
    api.UpdateGameTime(12.5f); require(api.CurrentTime() == 12.5,"Source game time was not retained");

    const float ones[4] = {1,1,1,1};
    const int integers[4] = {1,0,0,0}; const BOOL boolean = -1;
    api.SetVertexShaderConstant(255,ones); api.SetPixelShaderConstant(223,ones);
    api.SetIntegerVertexShaderConstant(15,integers); api.SetIntegerPixelShaderConstant(15,integers);
    api.SetBooleanVertexShaderConstant(15,&boolean); api.SetBooleanPixelShaderConstant(15,&boolean);
    rejected([&] { api.SetVertexShaderConstant(255,ones,2); });
    rejected([&] { api.SetIntegerPixelShaderConstant(15,integers,2); });

    auto base = api.CreateTexture(2,2,1,IMAGE_FORMAT_BGRA8888,2,1,TEXTURE_CREATE_MANAGED|TEXTURE_CREATE_SRGB,"API Base","Probe");
    auto white = api.CreateTexture(1,1,1,IMAGE_FORMAT_RGBA8888,1,1,TEXTURE_CREATE_MANAGED,"API White","Probe");
    uint8_t basePixels[] = {32,64,128,255, 32,64,128,255, 32,64,128,255, 32,64,128,255};
    uint8_t green[] = {0,255,0,255}, whitePixel[] = {255,255,255,255};
    api.ModifyTexture(base);
    api.TexImage2D(0,0,IMAGE_FORMAT_BGRA8888,0,2,2,IMAGE_FORMAT_BGRA8888,false,basePixels);
    require(!api.IsTextureResident(base),"Incomplete Source mip chain was resident");
    api.TexImage2D(1,0,IMAGE_FORMAT_BGRA8888,0,1,1,IMAGE_FORMAT_RGBA8888,false,green);
    api.TexMinFilter(SHADER_TEXFILTERMODE_NEAREST_MIPMAP_NEAREST); api.TexMagFilter(SHADER_TEXFILTERMODE_NEAREST);
    api.TexWrap(SHADER_TEXCOORD_S,SHADER_TEXWRAPMODE_CLAMP); api.TexWrap(SHADER_TEXCOORD_T,SHADER_TEXWRAPMODE_CLAMP);
    rejected([&] { api.TexSubImage2D(0,0,2,0,0,1,1,IMAGE_FORMAT_RGBA8888,0,false,green); });
    rejected([&] { api.TexImage2D(0,1,IMAGE_FORMAT_BGRA8888,0,2,2,IMAGE_FORMAT_BGRA8888,false,basePixels); });
    api.ModifyTexture(white); api.TexImage2D(0,0,IMAGE_FORMAT_RGBA8888,0,1,1,IMAGE_FORMAT_RGBA8888,false,whitePixel);
    api.SetStandardTextureHandle(TEXTURE_WHITE,white);
    require(api.IsStandardTextureHandleValid(TEXTURE_WHITE) && api.FindTexture("API Base") == base,"Source standard/name texture lookup failed");
    int width=0,height=0,depth=0;
    api.GetTextureDimensions(base,width,height,depth); require(width==2 && height==2 && depth==1,"Source texture dimensions changed");
    ShaderAPITextureHandle_t rollback[] = {-7,-9};
    rejected([&] { api.CreateTextures(rollback,2,1,1,1,IMAGE_FORMAT_RGBA8888,1,1,0,"Rollback","Probe"); },"a texture batch over budget");
    require(rollback[0] == -7 && rollback[1] == -9 && sourceAPI.statistics().textures.textures == 2 &&
        sourceAPI.statistics().textures.shadowBytes == 24,"Source texture batch did not roll back atomically");
    auto transient = api.CreateTexture(1,1,1,IMAGE_FORMAT_RGBA8888,1,1,0,"Transient","Probe");
    api.DeleteTexture(transient);
    auto replacement = api.CreateTexture(1,1,1,IMAGE_FORMAT_RGBA8888,1,1,0,"Replacement","Probe");
    require(replacement != transient && !api.IsTexture(transient),"Source texture handles were recycled");
    rejected([&] { api.ModifyTexture(transient); }); api.DeleteTexture(replacement);
    {
        SourceTextures bounded(context,graphics,{2,64,64});
        rejected([&] { bounded.create(4,4,1,IMAGE_FORMAT_RGBA8888,0,1,0,"MipBudget"); });
        auto handle = bounded.create(2,2,1,IMAGE_FORMAT_RGBA8888,0,1,0,"Fits");
        require(bounded.statistics().shadowBytes==20,"Source texture budget omitted a mip");
        rejected([&] { bounded.create(4,4,1,IMAGE_FORMAT_RGBA8888,1,1,0,"TotalBudget"); });
        bounded.destroy(handle); require(!bounded.statistics().shadowBytes,"Source texture shadow budget leaked");
    }
    {
        ImageDescription description;
        description.srgbViews = true; description.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        auto image = context.createImage(description);
        auto sampled = image.samplingView(true);
        require(sampled.handle() == image.handle() && sampled.description().format == VK_FORMAT_R8G8B8A8_SRGB &&
            sampled.description().usage == VK_IMAGE_USAGE_SAMPLED_BIT,"sRGB sampling view inherited incompatible storage usage");
        auto* vertex = device.interface().CreateVertexBuffer(SHADER_BUFFER_TYPE_DYNAMIC,VertexFormat,4,"Cast check");
        vertex->EndCastBuffer();
        rejected([&] { api.BindVertexBuffer(0,vertex,0,0,4,VERTEX_POSITION); },"a wrong ended-cast vertex format");
        device.interface().DestroyVertexBuffer(vertex);
        auto* index = device.interface().CreateIndexBuffer(SHADER_BUFFER_TYPE_DYNAMIC,MATERIAL_INDEX_FORMAT_UNKNOWN,24,"Cast check");
        index->BeginCastBuffer(MATERIAL_INDEX_FORMAT_32BIT);
        require(index->IndexFormat()==MATERIAL_INDEX_FORMAT_32BIT && index->IndexCount()==6,"First dynamic index cast did not select 32 bits");
        index->EndCastBuffer(); device.interface().DestroyIndexBuffer(index);
    }

    IMesh* mesh = device.interface().CreateStaticMesh(VertexFormat,"API Probe",nullptr,nullptr);
    fillMesh(mesh);
    auto* index32 = device.interface().CreateIndexBuffer(SHADER_BUFFER_TYPE_STATIC,MATERIAL_INDEX_FORMAT_32BIT,6,"API Probe");
    IndexDesc_t indexLock;
    require(index32->Lock(6,false,indexLock),"Source 32-bit index lock failed");
    const uint32_t indices32[] = {0,1,2,0,2,3};
    std::memcpy(indexLock.m_pIndices,indices32,sizeof(indices32)); index32->Unlock(6,indexLock);
    RenderTargetDescription targetDescription;
    targetDescription.width = Width; targetDescription.height = Height; targetDescription.depthFormat = VK_FORMAT_D32_SFLOAT;
    targetDescription.colorFinalLayout = targetDescription.depthFinalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    RenderTarget target(context,targetDescription);
    auto beforePixels = context.createBuffer(Width*Height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    auto transitionPixels = context.createBuffer(Width*Height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    auto afterPixels = context.createBuffer(Width*Height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    auto afterDepth = context.createBuffer(Width*Height*sizeof(float),VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    // An opaque identity only: this callback deliberately does not dereference
    // IMaterial. It validates the engine hook, not an actual migrated material.
    uintptr_t materialIdentity = 0;
    auto* materialToken = reinterpret_cast<IMaterial*>(&materialIdentity);
    uint32_t materialCallbacks = 0;
    sourceAPI.setMaterialPassCallback([&](IMaterial* material, const SourceMeshDraw& draw) {
        require(material == materialToken && draw.mesh == mesh,"Source material callback lost mesh/material identity");
        api.BeginPass(opaque); api.BindStandardTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,TEXTURE_WHITE);
        rectangle(api,3,2,.3f);
        ShaderViewport_t viewport;
        viewport.Init(96,64,32,32); api.SetViewports(1,&viewport);
        const float localTransform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,.3f, 0,0,0,1};
        api.SetVertexShaderConstant(0,localTransform,4);
        api.RenderPass(nullptr,0,1); ++materialCallbacks;
    });
    VkDeviceSize stableArena = 0;
    const auto deadline = SDL_GetTicksNS() + 30'000'000'000ull;
    for (uint32_t number=0;number<Frames;++number) {
        require(SDL_GetTicksNS()<deadline,"Source API diagnostic timed out");
        SDL_Event event;
        while (SDL_PollEvent(&event)) require(event.type!=SDL_EVENT_QUIT,"Source API diagnostic window was closed");
        if (number==30) {
            device.interface().ReleaseResources(true);
            require(!api.IsTextureResident(base) && !api.IsTextureResident(white),"Source resource release retained resident textures");
            device.interface().ReacquireResources();
        }
        const auto waits = context.statistics().idleWaits;
        while (!sourceAPI.beginFrame()) {
            require(SDL_GetTicksNS()<deadline,"Source API surface stayed unavailable"); SDL_Delay(5);
        }
        const auto frame = sourceAPI.frame();
        require(api.IsTextureResident(base) && api.IsTextureResident(white),"Source frame did not upload complete texture chains");
        api.SetDefaultState();
        sourceAPI.beginTarget(target,{.2f,.3f,.4f,.5f},.05f);
        api.ClearColor4ub(0,0,0,255); api.ClearBuffers(true,true,false,Width,Height);
        if (!number) {
            api.BeginPass(opaque); api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,base); rectangle(api,0,0,.4f);
            api.SetPixelShaderIndex(99); rejected([&] { mesh->Draw(); },"a missing shader combo"); api.SetPixelShaderIndex(0);
            api.BeginPass(srgb); rejected([&] { mesh->Draw(); },"an sRGB binding mismatch");
        }
        api.BeginPass(opaque); api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,base);
        rectangle(api,0,0,.4f); mesh->Draw();
        if (number==20) {
            // A draw already references the old image in this render pass.
            // Pitched uploads must replace it, preserving that draw and the mip.
            uint8_t blueColumn[] = {255,0,0,255, 99,99,99,99, 255,0,0,255};
            api.ModifyTexture(base);
            api.TexSubImage2D(0,0,0,0,0,1,2,IMAGE_FORMAT_BGRA8888,8,false,blueColumn);
            require(!api.IsTextureResident(base),"Dirty texture incorrectly remained resident");
        }
        rectangle(api,1,0,.4f,{.5f,1,1,1}); mesh->Draw();
        api.BeginPass(srgb); api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,base);
        rectangle(api,2,0,.4f); mesh->Draw();
        api.BeginPass(opaque); api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,base);
        rectangle(api,3,0,.4f,{1,1,1,1},1); mesh->Draw();
        api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NOMIP,base);
        rectangle(api,0,1,.4f,{1,1,1,1},1); mesh->Draw();
        api.BeginPass(cutout); api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,base);
        rectangle(api,1,1,.2f,{1,1,1,.25f}); mesh->Draw();
        rectangle(api,2,1,.2f); mesh->Draw();
        api.BeginPass(opaque); api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,white);
        rectangle(api,2,1,.8f,{1,0,0,1}); mesh->Draw();
        api.BeginPass(blend); api.SetScissorRect(112,32,128,64,true);
        rectangle(api,3,1,.1f,{0,0,1,.5f}); mesh->Draw();
        api.SetScissorRect(0,0,0,0,false); api.BeginPass(opaque);
        auto* dynamic = api.GetDynamicMeshEx(nullptr,VertexFormat,0);
        require(static_cast<IVertexBuffer*>(dynamic)->IsDynamic() && static_cast<IIndexBuffer*>(dynamic)->IsDynamic(),
            "Source API dynamic mesh is static");
        fillMesh(dynamic,{0,0,255,255}); rectangle(api,0,2,.3f); dynamic->Draw();
        fillMesh(dynamic,{0,255,0,255}); rectangle(api,1,2,.3f); dynamic->Draw();
        // Mutating the same CPU mesh again must not overwrite either draw.
        fillMesh(dynamic,{255,0,0,255});
        api.BindVertexBuffer(0,mesh,0,0,4,VertexFormat);
        api.BindIndexBuffer(index32,0); rectangle(api,2,2,.3f,{1,1,0,1});
        if (!number) rejected([&] { api.Draw(MATERIAL_TRIANGLES,1,6); },"an out-of-range index draw");
        api.Draw(MATERIAL_TRIANGLES,0,6);
        api.Bind(materialToken);
        const Vector4D modulation(1,0,1,1);
        mesh->DrawModulated(modulation);
        api.Bind(nullptr);
        sourceAPI.endTarget();
        if (number==19) readback(context,frame,target,beforePixels);
        if (number==20) readback(context,frame,target,transitionPixels);
        if (number+1==Frames) {
            readback(context,frame,target,afterPixels,afterDepth);
            api.DeleteTexture(base); api.DeleteTexture(white);
            device.interface().DestroyStaticMesh(mesh); device.interface().DestroyIndexBuffer(index32);
            require(!api.IsTexture(base) && !api.IsStandardTextureHandleValid(TEXTURE_WHITE),"Deleted Source texture remained bound");
            rejected([&] { api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,base); });
            api.ClearSnapshots(); rejected([&] { api.BeginPass(opaque); });
        }
        api.EndFrame(); device.interface().Present();
        if (number>1) require(waits==context.statistics().idleWaits,"Source API frame performed a device-wide wait");
        if (number==4) stableArena=arena.allocatedBytes();
        if (number>4) require(arena.allocatedBytes()==stableArena,"Source API frame arena grew during steady drawing");
    }
    manager.Shutdown(); manager.Disconnect();
    std::vector<uint8_t> before(Width*Height*4),transition(Width*Height*4),after(Width*Height*4);
    std::vector<float> depths(Width*Height);
    beforePixels.read(0,before.data(),before.size()); afterPixels.read(0,after.data(),after.size());
    transitionPixels.read(0,transition.data(),transition.size());
    afterDepth.read(0,depths.data(),depths.size()*sizeof(float));
    savePPM(output/"api-before-update.ppm",before); savePPM(output/"api-readback.ppm",after);
    auto pixel = [&](const std::vector<uint8_t>& data, uint32_t x, uint32_t y, std::array<int,4> expected) {
        for (size_t c=0;c<4;++c) if (std::abs(int(data[(y*Width+x)*4+c])-expected[c])>1)
            throw std::runtime_error("Source API pixel mismatch at "+std::to_string(x)+","+std::to_string(y)+
                " channel="+std::to_string(c)+" actual="+std::to_string(data[(y*Width+x)*4+c])+" expected="+std::to_string(expected[c]));
    };
    pixel(before,8,8,{128,64,32,255}); pixel(after,8,8,{0,0,255,255});
    pixel(transition,8,8,{128,64,32,255}); pixel(transition,40,8,{0,0,255,255});
    pixel(transition,56,8,{64,64,32,255}); pixel(transition,112,8,{0,255,0,255});
    pixel(after,24,8,{128,64,32,255}); pixel(after,56,8,{64,64,32,255});
    pixel(after,88,8,{55,13,4,255}); pixel(after,112,8,{0,255,0,255});
    pixel(after,24,40,{128,64,32,255}); pixel(after,48,48,{0,0,0,255});
    pixel(after,88,40,{128,64,32,255}); pixel(after,104,48,{0,0,0,255}); pixel(after,120,48,{0,0,128,255});
    pixel(after,16,80,{255,0,0,255}); pixel(after,48,80,{0,255,0,255});
    pixel(after,80,80,{255,255,0,255}); pixel(after,112,80,{255,0,255,255});
    require(std::abs(depths[48*Width+48]-1)<.0001f && std::abs(depths[40*Width+88]-.2f)<.0001f &&
        std::abs(depths[48*Width+120]-1)<.0001f,"Source API alpha-test, occlusion or transparent depth-write behavior failed");
    const auto counts = sourceAPI.statistics();
    const auto resources = device.statistics();
    require(counts.draws==Frames*DrawsPerFrame && materialCallbacks==Frames && resources.meshDraws==Frames*(DrawsPerFrame-1),
        "Source API draw path bypassed the mesh/material interface");
    require(!counts.textures.textures && !counts.textures.shadowBytes && counts.textures.imageUploads==5 && counts.textures.mipUploads==8,
        "Source texture update/reacquire/delete lifecycle failed");
    require(resources.uploadSubmissions==3 && resources.bufferUploads==6 && resources.dynamicCopies==Frames*3,
        "Source API buffer/texture uploads were not batched or dynamic mesh data was not reused");
    require(counts.textures.uploadInspections==counts.textures.imageUploads && resources.uploadInspections==resources.bufferUploads,
        "Unchanged Source textures or buffers were inspected during repeated draws");
    require(counts.descriptorHits>Frames*5 && counts.descriptorSets<Frames*8,"Source API descriptors were not reused across draws");
    graphics.savePipelineCache((output/"pipeline-cache.bin").string());
    log("VK_SOURCE_API_PASS: interfaces=ShaderApi029/ShaderDynamic001 frames="+std::to_string(Frames)+
        " draws="+std::to_string(counts.draws)+" material_callbacks="+std::to_string(materialCallbacks)+
        " descriptor_sets="+std::to_string(counts.descriptorSets)+" descriptor_hits="+std::to_string(counts.descriptorHits)+
        " constant_uploads="+std::to_string(counts.constantUploads)+" dynamic_bytes="+std::to_string(stableArena));
    log("VK_SOURCE_TEXTURE_PASS: image_uploads="+std::to_string(counts.textures.imageUploads)+
        " mip_uploads="+std::to_string(counts.textures.mipUploads)+" combined_submissions="+std::to_string(resources.uploadSubmissions)+
        " mip_sampling=pass srgb_views=pass pitched_update=pass mid_frame_copy_on_write=pass release_reacquire=pass delete_before_submit=pass shadow_bytes=0");
    log("VK_SOURCE_API_DRAW_PASS: static_mesh=pass dynamic_mesh=immutable indices=16/32 registers=c255/c223/i15/b15 alpha_test=color/depth blend/scissor=pass clear/viewport=pass");
    log("VK_SOURCE_UPLOAD_PASS: texture_inspections="+std::to_string(counts.textures.uploadInspections)+
        " buffer_inspections="+std::to_string(resources.uploadInspections)+" unchanged_resource_scans=0");
}
void exerciseFiltering(Context& context, SDL_Window* window, const std::string& directory,
        const std::filesystem::path& output, const LogSink& log) {
    GraphicsDevice graphics(context);
    const auto& caps=context.capabilities();
    const float maximum=caps.enabledFeatures.samplerAnisotropy?caps.properties.limits.maxSamplerAnisotropy:1;
    SamplerDescription description;description.maxAnisotropy=maximum;
    auto sampler=graphics.sampler(description);
    require(graphics.sampler(description).handle()==sampler.handle(),"Identical sampling state was not shared");
    description.maxAnisotropy=maximum+1;
    rejected([&] {graphics.sampler(description);},"anisotropy above the enabled hardware limit");
    description.maxAnisotropy=std::numeric_limits<float>::quiet_NaN();
    rejected([&] {graphics.sampler(description);},"non-finite anisotropy");
    {
        SourceTextures textures(context,graphics);
        auto handle=textures.create(16,16,1,IMAGE_FORMAT_RGBA8888,1,1,TEXTURE_CREATE_RENDERTARGET,"Sampler lifetime");
        textures.minFilter(handle,SHADER_TEXFILTERMODE_ANISOTROPIC);
        textures.magFilter(handle,SHADER_TEXFILTERMODE_ANISOTROPIC);
        textures.setAnisotropicLevel(4096);
        const auto before=textures.binding(handle,false);
        require(before.sampler.description().maxAnisotropy==maximum,"Source anisotropy was not clamped to device capabilities");
        require(textures.binding(handle,true).sampler.handle()==before.sampler.handle(),"sRGB view unnecessarily changed the sampler");
        for(int i=0;i<100;++i)textures.binding(handle,(i&1)!=0);
        require(textures.statistics().filterFormatChecks==2,"Unchanged bindings re-queried format capabilities or missed the sRGB view");
        const auto point=textures.binding(handle,false,true,true);
        require(point.sampler.description().maxAnisotropy==1 && point.sampler.description().maxLod==0 &&
            point.sampler.description().minFilter==VK_FILTER_NEAREST,"Point/NOMIP override retained anisotropy");
        textures.setAnisotropicLevel(0);
        const auto after=textures.binding(handle,false);
        const float hinted=std::min(maximum,float(std::max(2,int(maximum)/2)));
        require(after.sampler.description().maxAnisotropy==hinted && before.sampler.description().maxAnisotropy==maximum,
            "Changing Source filtering mutated a retained sampler or lost per-texture hints");
        textures.destroy(handle);
    }
    FrameArena arena(context,64*1024,512*1024);
    SourceShadow shadow;
    SourceDevice device(context,graphics,arena,shadow,window);
    SourceShaderLibrary shaders(graphics,directory);
    SourceAPI source(context,graphics,arena,device,shadow,shaders);
    source.registerShaderInputs("native_unlit_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}},0,0);
    auto& api=source.interface();auto& manager=device.manager();auto& states=shadow.interface();
    require(manager.Connect([](const char*,int*)->void* {return nullptr;}) && manager.Init()==INIT_OK,"Filtering device initialization failed");
    ShaderDeviceInfo_t mode;mode.m_bWindowed=true;mode.m_DisplayMode.m_nWidth=640;mode.m_DisplayMode.m_nHeight=480;
    require(manager.SetMode(window,0,mode),"Filtering device mode failed");
    states.SetDefaultState();states.VertexShaderVertexFormat(VERTEX_POSITION,1,nullptr,0);
    states.SetVertexShader("native_unlit_vs",0);states.SetPixelShader("native_unlit_ps",0);
    states.EnableTexture(SHADER_SAMPLER0,true);states.EnableDepthTest(false);states.EnableDepthWrites(false);
    states.EnableCulling(false);states.EnableAlphaWrites(true);
    const auto snapshot=api.TakeSnapshot();
    const auto texture=api.CreateTexture(256,256,1,IMAGE_FORMAT_RGBA8888,9,1,0,"Anisotropic stripe mip chain","Probe");
    api.ModifyTexture(texture);
    for(int level=0;level<9;++level) {
        const int size=256>>level;
        std::vector<uint8_t> pixels(size_t(size)*size*4);
        for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
            // A box-filtered mip chain of four-texel black/white stripes.
            const uint8_t value=level>=3?128:((x<<level)&4)?255:0;
            const size_t offset=(size_t(y)*size+x)*4;
            pixels[offset]=pixels[offset+1]=pixels[offset+2]=value;pixels[offset+3]=255;
        }
        api.TexImage2D(level,0,IMAGE_FORMAT_RGBA8888,0,size,size,IMAGE_FORMAT_RGBA8888,false,pixels.data());
    }
    auto* mesh=device.interface().CreateStaticMesh(VertexFormat,"Anisotropic sampling",nullptr,nullptr);fillMesh(mesh);
    const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},white[4]={1,1,1,1};
    // One texel per pixel in X, sixteen in Y: isotropic mip selection erases
    // the stripes, while anisotropic filtering can preserve their X detail.
    const float uv[8]={Width/256.f,0,0,0,0,Height*16/256.f,0,0};
    api.SetVertexShaderConstant(4,identity,4);api.SetVertexShaderConstant(48,uv,2);api.SetPixelShaderConstant(0,white);
    RenderTargetDescription targetDescription;targetDescription.width=Width;targetDescription.height=Height;
    targetDescription.colorFinalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    RenderTarget target(context,targetDescription);
    auto buffer=context.createBuffer(Width*Height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    require(source.beginFrame(),"Filtering frame unavailable");const auto frame=source.frame();
    source.beginTarget(target);api.BeginPass(snapshot);api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,texture);
    api.TexMinFilter(SHADER_TEXFILTERMODE_LINEAR_MIPMAP_LINEAR);api.TexMagFilter(SHADER_TEXFILTERMODE_LINEAR);
    api.SetScissorRect(0,0,Width,Height/2,true);mesh->Draw();
    api.SetAnisotropicLevel(4096);api.TexMinFilter(SHADER_TEXFILTERMODE_ANISOTROPIC);api.TexMagFilter(SHADER_TEXFILTERMODE_ANISOTROPIC);
    api.SetScissorRect(0,Height/2,Width,Height,true);mesh->Draw();
    // Change the setting again before submission to exercise immutable state.
    api.SetAnisotropicLevel(0);
    source.endTarget();readback(context,frame,target,buffer);api.EndFrame();device.interface().Present();context.waitIdle();
    std::vector<uint8_t> pixels(Width*Height*4);buffer.read(0,pixels.data(),pixels.size());
    int low=255,high=0;
    for(uint32_t x=8;x<Width-8;++x) {
        require(std::abs(int(pixels[(Height/4*Width+x)*4])-128)<=2,"Later filtering state changed an earlier trilinear draw");
        const int value=pixels[((3*Height/4)*Width+x)*4];low=std::min(low,value);high=std::max(high,value);
    }
    if(maximum>=8)require(high-low>=128,"Anisotropic filtering lost resolvable texture detail");
    if(maximum==1)require(high-low<=2,"Disabled anisotropy did not use the core linear fallback");
    require(source.statistics().textures.imageUploads==1,"A filter change unnecessarily reuploaded texture pixels");
    savePPM(output/"anisotropic-readback.ppm",pixels);
    api.DeleteTexture(texture);device.interface().DestroyStaticMesh(mesh);context.waitIdle();
    log("VK_SOURCE_FILTER_PASS: anisotropy="+std::to_string(maximum)+" resolved_contrast="+std::to_string(high-low)+
        " immutable_samplers=pass point_nomip=pass shared_samplers=pass image_uploads=1 sampler_creations="+
        std::to_string(graphics.statistics().samplerCreations)+" sampler_hits="+std::to_string(graphics.statistics().samplerHits));
}

void exerciseShadows(Context& context, SDL_Window* window, const std::string& directory,
        const std::filesystem::path& output, const LogSink& log) {
    GraphicsDevice graphics(context);
    FrameArena arena(context,64*1024,512*1024);
    SourceShadow shadow;
    SourceDevice device(context,graphics,arena,shadow,window);
    SourceShaderLibrary shaders(graphics,directory);
    SourceAPI source(context,graphics,arena,device,shadow,shaders);
    source.registerShaderInputs("native_shadow_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1},{SourceSemantic::TexCoord,1,2}});
    source.registerShaderInputs("native_shadowbuild_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}},0,0);
    source.registerShaderInputs("native_shadowbuild_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1},
        {SourceSemantic::BoneWeights,0,2},{SourceSemantic::BoneIndices,0,3}},0,1);
    auto& api=source.interface();auto& manager=device.manager();auto& states=shadow.interface();
    require(manager.Connect([](const char*,int*)->void* {return nullptr;}) && manager.Init()==INIT_OK,"Shadow device initialization failed");
    ShaderDeviceInfo_t mode;mode.m_bWindowed=true;mode.m_DisplayMode.m_nWidth=640;mode.m_DisplayMode.m_nHeight=480;
    require(manager.SetMode(window,0,mode),"Shadow device mode failed");
    states.SetDefaultState();states.VertexShaderVertexFormat(VERTEX_POSITION,1,nullptr,0);
    states.SetVertexShader("native_shadowbuild_vs",0);states.SetPixelShader("native_shadowbuild_ps",0);
    states.EnableTexture(SHADER_SAMPLER0,true);states.EnableSRGBRead(SHADER_SAMPLER0,true);states.EnableSRGBWrite(true);
    states.EnableCulling(false);states.EnableDepthTest(false);states.EnableDepthWrites(false);states.EnableAlphaWrites(true);
    states.EnableBlending(true);states.BlendFunc(SHADER_BLEND_ONE,SHADER_BLEND_ONE);
    const auto build=api.TakeSnapshot();
    int dimensions[2]={3,3};states.VertexShaderVertexFormat(VERTEX_POSITION|VERTEX_COLOR,2,dimensions,0);
    states.SetVertexShader("native_shadow_vs",0);states.SetPixelShader("native_shadow_ps",0);
    states.EnableAlphaWrites(false);states.BlendFunc(SHADER_BLEND_ZERO,SHADER_BLEND_SRC_COLOR);states.FogMode(SHADER_FOGMODE_WHITE,false);
    const auto projected=api.TakeSnapshot();
    states.SetPixelShader("native_shadow_ps",1);const auto blobby=api.TakeSnapshot();
    states.FogMode(SHADER_FOGMODE_DISABLED,false);const auto noFog=api.TakeSnapshot();
    auto halfAlpha=api.CreateTexture(1,1,1,IMAGE_FORMAT_RGBA8888,1,1,0,"Shadow alpha","Probe");
    uint8_t alphaPixel[4]={32,96,160,128};api.ModifyTexture(halfAlpha);
    api.TexImage2D(0,0,IMAGE_FORMAT_RGBA8888,0,1,1,IMAGE_FORMAT_RGBA8888,false,alphaPixel);
    api.TexMinFilter(SHADER_TEXFILTERMODE_NEAREST);api.TexMagFilter(SHADER_TEXFILTERMODE_NEAREST);
    auto coverage=api.CreateTexture(8,8,1,IMAGE_FORMAT_RGBA8888,1,1,0,"Five-tap shadow coverage","Probe");
    uint8_t coveragePixels[8*8*4]={};coveragePixels[(4*8+4)*4+3]=255;
    api.ModifyTexture(coverage);api.TexImage2D(0,0,IMAGE_FORMAT_RGBA8888,0,8,8,IMAGE_FORMAT_RGBA8888,false,coveragePixels);
    api.TexMinFilter(SHADER_TEXFILTERMODE_NEAREST);api.TexMagFilter(SHADER_TEXFILTERMODE_NEAREST);
    auto* quad=device.interface().CreateStaticMesh(VertexFormat,"Shadow atlas caster",nullptr,nullptr);fillMesh(quad);
    auto receiver=[&](float distance,std::array<float,3> falloff) {
        SourceVertexFormatDescription format;format.color=format.exact=true;format.texCoords[0]=format.texCoords[1]=3;
        auto* mesh=device.interface().CreateStaticMesh(sourceVertexFormat(format),"Shadow receiver",nullptr,nullptr);
        MeshDesc_t lock;mesh->LockMesh(4,6,lock);
        const float positions[4][3]={{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}},uv[3]={4.5f/8,4.5f/8,distance};
        for(int i=0;i<4;++i) {
            std::memcpy(reinterpret_cast<uint8_t*>(lock.m_pPosition)+i*lock.m_VertexSize_Position,positions[i],12);
            std::memset(lock.m_pColor+i*lock.m_VertexSize_Color,255,4);
            std::memcpy(reinterpret_cast<uint8_t*>(lock.m_pTexCoord[0])+i*lock.m_VertexSize_TexCoord[0],uv,12);
            std::memcpy(reinterpret_cast<uint8_t*>(lock.m_pTexCoord[1])+i*lock.m_VertexSize_TexCoord[1],falloff.data(),12);
        }
        const uint16_t indices[6]={0,1,2,0,2,3};std::memcpy(lock.m_pIndices,indices,sizeof(indices));mesh->UnlockMesh(4,6,lock);
        return mesh;
    };
    auto* full=receiver(0,{0,0,0});auto* faded=receiver(1,{.1f,.25f,.05f});auto* gone=receiver(5,{0,1,.1f});
    SourceVertexFormatDescription skinFormat;skinFormat.exact=true;skinFormat.boneWeights=2;skinFormat.texCoords[0]=2;
    auto* skin=device.interface().CreateStaticMesh(sourceVertexFormat(skinFormat),"Skinned shadow caster",nullptr,nullptr);
    MeshDesc_t skinLock;skin->LockMesh(4,6,skinLock);
    const float skinPositions[4][3]={{-1,-1,0},{0,-1,0},{0,1,0},{-1,1,0}},weights[2]={.25f,.75f},uv0[2]={0,0};
    const uint8_t bones[4]={0,1,2,0};
    for(int i=0;i<4;++i) {
        std::memcpy(reinterpret_cast<uint8_t*>(skinLock.m_pPosition)+i*skinLock.m_VertexSize_Position,skinPositions[i],12);
        std::memcpy(reinterpret_cast<uint8_t*>(skinLock.m_pBoneWeight)+i*skinLock.m_VertexSize_BoneWeight,weights,8);
        std::memcpy(skinLock.m_pBoneMatrixIndex+i*skinLock.m_VertexSize_BoneMatrixIndex,bones,4);
        std::memcpy(reinterpret_cast<uint8_t*>(skinLock.m_pTexCoord[0])+i*skinLock.m_VertexSize_TexCoord[0],uv0,8);
    }
    const uint16_t indices[6]={0,1,2,0,2,3};std::memcpy(skinLock.m_pIndices,indices,sizeof(indices));skin->UnlockMesh(4,6,skinLock);
    RenderTargetDescription targetDescription;targetDescription.width=Width;targetDescription.height=Height;
    targetDescription.colorFormat=VK_FORMAT_R8G8B8A8_SRGB;targetDescription.colorFinalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    RenderTarget target(context,targetDescription);
    auto buffer=context.createBuffer(Width*Height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1},transform[8]={1,0,0,0,0,1,0,0},modulation[4]={1,1,1,.5f};
    api.SetVertexShaderConstant(8,identity,4);api.SetVertexShaderConstant(48,transform,2);api.LoadBoneMatrix(0,identity);
    // Bone matrices are 3x4, and the first twelve floats of this identity match.
    api.SetPixelShaderConstant(0,modulation);
    require(source.beginFrame(),"Shadow build frame unavailable");auto frame=source.frame();
    source.beginTarget(target,{0,0,0,0});api.BeginPass(build);api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,halfAlpha);
    quad->Draw();api.SetScissorRect(32,24,96,72,true);quad->Draw();
    source.endTarget();readback(context,frame,target,buffer);api.EndFrame();device.interface().Present();context.waitIdle();
    std::vector<uint8_t> pixels(Width*Height*4);buffer.read(0,pixels.data(),pixels.size());
    require(pixels[(12*Width+16)*4+3]==64 && pixels[(48*Width+64)*4+3]==128,"Shadow atlas lost translucent alpha or additive coverage");
    savePPM(output/"shadow-build-readback.ppm",pixels);
    float translated[12]={1,0,0,1,0,1,0,0,0,0,1,0};api.LoadBoneMatrix(1,translated);api.SetNumBoneWeights(2);
    const float opaque[4]={1,1,1,1};api.SetPixelShaderConstant(0,opaque);
    require(source.beginFrame(),"Skinned shadow frame unavailable");frame=source.frame();
    source.beginTarget(target,{0,0,0,0});api.BeginPass(build);api.SetVertexShaderIndex(1);skin->Draw();
    source.endTarget();readback(context,frame,target,buffer);api.EndFrame();device.interface().Present();context.waitIdle();
    buffer.read(0,pixels.data(),pixels.size());
    require(pixels[(48*Width+16)*4+3]==0 && pixels[(48*Width+64)*4+3]==128 && pixels[(48*Width+120)*4+3]==0,
        "Shadow silhouette did not follow the weighted bone transforms");
    savePPM(output/"shadow-skin-readback.ppm",pixels);
    api.SetNumBoneWeights(0);api.SetVertexShaderIndex(0);api.LoadBoneMatrix(0,identity);
    const float shadowColor[4]={.25f,.5f,.75f,1},jitter[4]={1.f/8,1.f/8,0,0},falloff[4]={240.f/255,0,0,0};
    api.SetVertexShaderConstant(50,jitter);api.SetPixelShaderConstant(1,shadowColor);api.SetPixelShaderConstant(4,falloff);
    const float eye[4]={0,0,10,1};api.SetPixelShaderConstant(2,eye);
    require(source.beginFrame(),"Projected shadow frame unavailable");frame=source.frame();
    source.beginTarget(target,{1,1,1,.375f});api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,coverage);
    auto tile=[&](int column,int row,StateSnapshot_t snapshot,IMesh* mesh) {
        const float matrix[16]={32.f/Width,0,0,(column*64.f+32)/Width-1,0,32.f/Height,0,1-(row*64.f+32)/Height,
            0,0,1,0,0,0,0,1};
        api.SetVertexShaderConstant(8,matrix,4);api.BeginPass(snapshot);api.SetPixelShaderFogParams(3);
        api.SetPixelShaderIndex(api.GetPixelFogCombo());mesh->Draw();
    };
    api.SceneFogMode(MATERIAL_FOG_NONE);require(api.GetPixelFogCombo()==0,"No-fog combo changed");
    tile(0,0,projected,full);tile(1,0,blobby,full);tile(2,0,blobby,faded);tile(3,0,blobby,gone);
    api.SceneFogMode(MATERIAL_FOG_LINEAR);api.FogStart(1);api.FogEnd(3);api.FogMaxDensity(.5f);
    require(api.GetPixelFogCombo()==0,"Range fog selected the height-fog shader");tile(0,1,blobby,full);
    api.SceneFogMode(MATERIAL_FOG_LINEAR_BELOW_FOG_Z);api.SetFogZ(3);
    require(api.GetPixelFogCombo()==1,"Height fog combo changed");tile(1,1,blobby,full);
    api.SceneFogMode(MATERIAL_FOG_LINEAR);tile(2,1,noFog,full);
    source.endTarget();readback(context,frame,target,buffer);api.EndFrame();device.interface().Present();context.waitIdle();
    buffer.read(0,pixels.data(),pixels.size());
    auto check=[&](int column,int row,float coverage,float fog) {
        const size_t offset=((row*32+16)*Width+column*32+16)*4;
        for(int c=0;c<3;++c) {
            const float linear=1-coverage*(1-shadowColor[c])*std::pow(1-fog,4);
            const int expected=int(std::lround(255*(linear<=.0031308f?12.92f*linear:1.055f*std::pow(linear,1/2.4f)-.055f)));
            require(std::abs(int(pixels[offset+c])-expected)<=2,"Projected shadow filter/falloff/fog or sRGB blend differs from PC semantics");
        }
        require(pixels[offset+3]==96,"Projected shadow overwrote destination alpha");
    };
    check(0,0,.2f,0);check(1,0,1,0);check(2,0,1-(.05f+.35f*240/255),0);check(3,0,0,0);
    check(0,1,1,.5f);check(1,1,1,.5f);check(2,1,1,0);
    savePPM(output/"shadow-projection-readback.ppm",pixels);
    for(auto* mesh:{quad,full,faded,gone,skin})device.interface().DestroyStaticMesh(mesh);
    api.DeleteTexture(halfAlpha);api.DeleteTexture(coverage);context.waitIdle();
    log("VK_SOURCE_SHADOW_PASS: alpha_atlas=pass additive_coverage=pass skinned_silhouette=pass five_tap_filter=pass blobby=pass distance_falloff=pass range_height_fog=pass material_fog_disable=pass linear_srgb_blend=pass destination_alpha=preserved");
}

void exerciseEnginePaths(Context& context, SDL_Window* window, const std::string& directory,
        const std::filesystem::path& output, const LogSink& log) {
    GraphicsDevice graphics(context);
    FrameArena arena(context,64*1024,512*1024);
    SourceShadow shadow;
    SourceDevice device(context,graphics,arena,shadow,window);
    SourceShaderLibrary shaders(graphics,directory);
    SourceAPI source(context,graphics,arena,device,shadow,shaders);
    source.registerShaderInputs("api_fixture_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1},{SourceSemantic::Color,0,2}});
    source.registerShaderInputs("native_model_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1},
        {SourceSemantic::Normal,0,4},{SourceSemantic::BoneWeights,0,5},{SourceSemantic::BoneIndices,0,6},
        {SourceSemantic::Color,1,7},{SourceSemantic::Color,2,8},{SourceSemantic::Color,3,9}},0,3);
    source.registerShaderInputs("native_rope_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::Color,0,1},
        {SourceSemantic::TexCoord,0,2},{SourceSemantic::TexCoord,1,3},{SourceSemantic::TexCoord,2,4},{SourceSemantic::TexCoord,3,5}});
    source.registerShaderInputs("native_screen_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}});
    source.registerShaderInputs("native_clear_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::Color,0,1}});
    source.registerShaderInputs("native_unlit_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}},0,0);
    source.registerShaderInputs("panoramafancy_vs30",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1},
        {SourceSemantic::TexCoord,1,2},{SourceSemantic::TexCoord,2,3},{SourceSemantic::TexCoord,3,4},{SourceSemantic::TexCoord,4,5}},0,0);
    source.registerShaderInputs("native_twotexture_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1}},0,0);
    source.registerShaderInputs("native_lightmap_vs",{{SourceSemantic::Position,0,0},{SourceSemantic::TexCoord,0,1},{SourceSemantic::TexCoord,1,3}},0,0);
    auto& api=source.interface(); auto& manager=device.manager();
    require(manager.Connect([](const char*,int*)->void* {return nullptr;}) && manager.Init()==INIT_OK,"Engine-path device initialization failed");
    ShaderDeviceInfo_t mode; mode.m_bWindowed=true; mode.m_DisplayMode.m_nWidth=640; mode.m_DisplayMode.m_nHeight=480;
    require(manager.SetMode(window,0,mode),"Engine-path device mode failed");
    auto& states=shadow.interface(); states.SetDefaultState();
    states.VertexShaderVertexFormat(VERTEX_POSITION|VERTEX_COLOR,1,nullptr,0);
    states.SetVertexShader("api_fixture_vs",0); states.SetPixelShader("api_fixture_ps",0);
    states.EnableTexture(SHADER_SAMPLER0,true); states.EnableAlphaWrites(true); states.EnableCulling(false);
    const auto snapshot=api.TakeSnapshot();
    states.VertexShaderVertexFormat(VERTEX_POSITION|VERTEX_NORMAL,1,nullptr,0);
    states.SetVertexShader("native_model_vs",0); states.SetPixelShader("native_model_ps",0);
    const auto modelSnapshot=api.TakeSnapshot();
    int ropeDimensions[4]={4,4,4,4};
    states.VertexShaderVertexFormat(VERTEX_POSITION|VERTEX_COLOR,4,ropeDimensions,0);
    states.SetVertexShader("native_rope_vs",0); states.SetPixelShader("native_rope_ps",0);
    states.EnableTexture(SHADER_SAMPLER1,true);states.EnableSRGBRead(SHADER_SAMPLER0,true);
    auto ropeSnapshot=api.TakeSnapshot();
    states.EnableSRGBRead(SHADER_SAMPLER0,false);
    states.VertexShaderVertexFormat(VERTEX_POSITION,1,nullptr,0);
    states.SetVertexShader("native_screen_vs",0);states.SetPixelShader("native_screen_ps",3);
    states.EnableTexture(SHADER_SAMPLER2,true);states.EnableDepthWrites(false);states.EnableDepthTest(false);
    const auto postSnapshot=api.TakeSnapshot();
    states.SetDefaultState();states.VertexShaderVertexFormat(VERTEX_POSITION|VERTEX_COLOR,1,nullptr,0);
    states.SetVertexShader("native_clear_vs",0);states.SetPixelShader("native_clear_ps",0);
    states.EnableCulling(false);states.EnableAlphaWrites(true);states.EnableDepthWrites(false);states.DepthFunc(SHADER_DEPTHFUNC_ALWAYS);
    const auto clearSnapshot=api.TakeSnapshot();
    states.VertexShaderVertexFormat(VERTEX_POSITION,1,nullptr,0);
    states.SetVertexShader("native_unlit_vs",0);states.SetPixelShader("native_unlit_ps",0);
    states.EnableTexture(SHADER_SAMPLER0,true);states.EnableDepthTest(false);
    const auto clipSnapshot=api.TakeSnapshot();
    states.SetDefaultState();int panelDimensions[5]={4,4,4,4,4};
    states.VertexShaderVertexFormat(VERTEX_POSITION,5,panelDimensions,0);
    states.SetVertexShader("panoramafancy_vs30",0);states.SetPixelShader("panoramafancy_ps30",0);
    states.EnableCulling(true);states.DepthFunc(SHADER_DEPTHFUNC_NEVER);
    states.EnableColorWrites(false);states.EnableAlphaWrites(false);
    states.EnableBlending(true);states.BlendFunc(SHADER_BLEND_ONE,SHADER_BLEND_ONE_MINUS_SRC_ALPHA);
    auto panelSnapshot=api.TakeSnapshot();
    states.SetDefaultState();int lightmapDimensions[2]={2,2};
    states.VertexShaderVertexFormat(VERTEX_POSITION,2,lightmapDimensions,0);
    states.SetVertexShader("native_lightmap_vs",0);states.SetPixelShader("native_lightmap_ps",0);
    states.EnableTexture(SHADER_SAMPLER0,true);states.EnableTexture(SHADER_SAMPLER1,true);
    states.EnableSRGBRead(SHADER_SAMPLER0,true);states.EnableSRGBWrite(true);
    states.EnableDepthTest(false);states.EnableDepthWrites(false);states.EnableCulling(false);states.EnableAlphaWrites(true);
    auto hdrSnapshot=api.TakeSnapshot();
    states.VertexShaderVertexFormat(VERTEX_POSITION,1,nullptr,0);
    states.SetVertexShader("native_twotexture_vs",0);states.SetPixelShader("native_twotexture_ps",0);
    states.EnableSRGBRead(SHADER_SAMPLER1,true);auto dualSnapshot=api.TakeSnapshot();
    const float ones[4]={1,1,1,1}; const int integer[4]={1,0,0,0}; const BOOL boolean=1;
    api.SetVertexShaderConstant(255,ones); api.SetPixelShaderConstant(223,ones);
    api.SetIntegerVertexShaderConstant(15,integer); api.SetIntegerPixelShaderConstant(15,integer);
    api.SetBooleanVertexShaderConstant(15,&boolean); api.SetBooleanPixelShaderConstant(15,&boolean);
    const auto white=api.CreateTexture(2,2,1,IMAGE_FORMAT_RGBA8888,1,1,TEXTURE_CREATE_DYNAMIC,"Engine fixture","Probe");
    uint8_t pixelWhite[4]={255,255,255,255};
    api.ModifyTexture(white); api.TexSubImage2D(0,0,0,0,0,1,1,IMAGE_FORMAT_RGBA8888,0,false,pixelWhite);
    api.TexMinFilter(SHADER_TEXFILTERMODE_NEAREST); api.TexMagFilter(SHADER_TEXFILTERMODE_NEAREST);
    const auto cableTexture=api.CreateTexture(7,5,1,IMAGE_FORMAT_DXT1,3,1,0,"Cable BC1 fixture","Probe");
    api.ModifyTexture(cableTexture);
    const uint8_t whiteBlock[8]={255,255,0,0,0,0,0,0};
    for(int mip=0;mip<3;++mip) {
        const int width=std::max(1,7>>mip),height=std::max(1,5>>mip);
        std::vector<uint8_t> blocks(size_t((width+3)/4)*((height+3)/4)*8);
        for(size_t offset=0;offset<blocks.size();offset+=8)std::memcpy(blocks.data()+offset,whiteBlock,8);
        api.TexImage2D(mip,0,IMAGE_FORMAT_DXT1,0,width,height,IMAGE_FORMAT_DXT1,false,blocks.data());
    }
    const auto bloomTexture=api.CreateTexture(4,4,1,IMAGE_FORMAT_DXT3,1,1,0,"Post BC2 fixture","Probe");
    uint8_t redBlock[16]={255,255,255,255,255,255,255,255,0,248,0,0,0,0,0,0};
    api.ModifyTexture(bloomTexture);api.TexImage2D(0,0,IMAGE_FORMAT_DXT3,0,4,4,IMAGE_FORMAT_DXT3,false,redBlock);
    const auto frameTexture=api.CreateTexture(4,4,1,IMAGE_FORMAT_DXT5,1,1,0,"Post BC3 fixture","Probe");
    uint8_t blueBlock[16]={128,128,0,0,0,0,0,0,31,0,0,0,0,0,0,0};
    api.ModifyTexture(frameTexture);api.TexImage2D(0,0,IMAGE_FORMAT_DXT5,0,4,4,IMAGE_FORMAT_DXT5,false,blueBlock);
    const bool nativeBC=context.capabilities().enabledFeatures.textureCompressionBC;
    require(source.statistics().textures.shadowBytes==(nativeBC?96u:312u),"BC mip storage did not match the selected capability tier");
    auto* mesh=device.interface().CreateStaticMesh(VertexFormat,"Engine fixture",nullptr,nullptr); fillMesh(mesh);
    SourceVertexFormatDescription skinFormat; skinFormat.normal=true; skinFormat.boneWeights=2; skinFormat.texCoords[0]=2; skinFormat.exact=true;
    auto* skinned=device.interface().CreateStaticMesh(sourceVertexFormat(skinFormat)|VERTEX_COLOR_STREAM_1,"Skinning fixture",nullptr,nullptr);
    MeshDesc_t skinLock; skinned->LockMesh(4,6,skinLock);
    const float positions[4][3]={{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
    for (int v=0;v<4;++v) {
        std::memcpy(reinterpret_cast<uint8_t*>(skinLock.m_pPosition)+v*skinLock.m_VertexSize_Position,positions[v],12);
        const float normal[3]={0,0,1}, weights[2]={.25f,.75f}, uv[2]={0,0};
        const uint8_t bones[4]={0,1,2,0};
        std::memcpy(reinterpret_cast<uint8_t*>(skinLock.m_pNormal)+v*skinLock.m_VertexSize_Normal,normal,12);
        std::memcpy(reinterpret_cast<uint8_t*>(skinLock.m_pBoneWeight)+v*skinLock.m_VertexSize_BoneWeight,weights,8);
        std::memcpy(skinLock.m_pBoneMatrixIndex+v*skinLock.m_VertexSize_BoneMatrixIndex,bones,4);
        std::memcpy(reinterpret_cast<uint8_t*>(skinLock.m_pTexCoord[0])+v*skinLock.m_VertexSize_TexCoord[0],uv,8);
    }
    const uint16_t skinIndices[6]={0,1,2,0,2,3}; std::memcpy(skinLock.m_pIndices,skinIndices,sizeof(skinIndices)); skinned->UnlockMesh(4,6,skinLock);
    auto* baked=device.interface().CreateStaticMesh(VERTEX_SPECULAR,"Baked lighting fixture",nullptr,nullptr);
    baked->SetPrimitiveType(MATERIAL_HETEROGENOUS);
    MeshDesc_t colorLock; baked->LockMesh(6,0,colorLock);
    for (int v=0;v<6;++v) {
        const uint8_t light[4]={uint8_t(v<2?255:0),0,uint8_t(v<2?0:64),255};
        std::memcpy(colorLock.m_pSpecular+v*colorLock.m_VertexSize_Specular,light,4);
    }
    baked->UnlockMesh(6,0,colorLock);
    auto* index32=device.interface().CreateIndexBuffer(SHADER_BUFFER_TYPE_STATIC,MATERIAL_INDEX_FORMAT_32BIT,6,"Engine fixture");
    IndexDesc_t indexLock; require(index32->Lock(6,false,indexLock),"Engine fixture index lock failed");
    const uint32_t indices32[6]={0,1,2,0,2,3}; std::memcpy(indexLock.m_pIndices,indices32,sizeof(indices32)); index32->Unlock(6,indexLock);
    ImageDescription color; color.width=Width; color.height=Height; color.srgbViews=true;
    color.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    auto first=context.createImage(color);
    color.width=Width/2; color.height=Height/2; auto second=context.createImage(color);
    ImageDescription depth; depth.width=Width; depth.height=Height; depth.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
    depth.aspect=VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    depth.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    auto sharedDepth=context.createImage(depth);
    clearAttachmentImage(context,first,{.1f,.2f,.3f,1});
    {
        SourceTextures resources(context,graphics);
        const auto handle=resources.create(2,2,1,IMAGE_FORMAT_RGBA8888,1,1,TEXTURE_CREATE_RENDERTARGET,"Persistent panel");
        const auto image=resources.renderImage(handle);
        clearAttachmentImage(context,image,{1,0,0,1});
        resources.evictManagedResources();
        require(resources.resident(handle) && resources.renderImage(handle).handle()==image.handle(),"Managed eviction discarded a UI render target");
        resources.destroy(handle);
    }
    clearAttachmentImage(context,sharedDepth,{},depth.aspect,1,0);
    RenderTarget firstTarget(context,first,sharedDepth), secondTarget(context,second,sharedDepth);
    auto firstRead=context.createBuffer(Width*Height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    auto secondRead=context.createBuffer(Width*Height,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    auto depthRead=context.createBuffer(Width*Height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    auto stencilRead=context.createBuffer(Width*Height,VK_BUFFER_USAGE_TRANSFER_DST_BIT,MemoryAccess::Readback);
    require(source.beginFrame(),"Engine-path frame was unavailable");
    auto frame=source.frame();
    source.beginTarget(firstTarget); api.BeginPass(snapshot); api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,white);
    // Overlays build an index-only batch before choosing the static vertex
    // stream. Binding the batch can change the material's primary format.
    auto* dynamic=api.GetDynamicMeshEx(nullptr,VERTEX_POSITION,0,false,nullptr,nullptr);
    MeshDesc_t lock; dynamic->LockMesh(0,api.GetMaxIndicesToRender(),lock);
    const uint16_t indices16[6]={0,1,2,0,2,3}; std::memcpy(lock.m_pIndices,indices16,sizeof(indices16)); dynamic->UnlockMesh(0,6,lock);
    dynamic=api.GetDynamicMeshEx(nullptr,VertexFormat,0,false,mesh,dynamic);
    rectangle(api,0,0,.4f,{1,0,0,1}); dynamic->Draw();
    uintptr_t identity=0; auto* material=reinterpret_cast<IMaterial*>(&identity);
    int callbacks=0;
    source.setMaterialPassCallback([&](IMaterial* received,const SourceMeshDraw& draw) {
        require(received==material && draw.instance && draw.vertices==mesh && draw.indices==index32,
            "Instance material callback lost its independent geometry");
        api.BeginPass(snapshot); rectangle(api,++callbacks,0,.4f); api.RenderPass(nullptr,0,1);
    });
    MeshInstanceData_t instances[2] {};
    for (auto& instance:instances) {
        instance.m_pVertexBuffer=mesh; instance.m_pIndexBuffer=index32;
        instance.m_nIndexCount=6; instance.m_nPrimType=MATERIAL_TRIANGLES;
        instance.m_nLightmapPageId=MATERIAL_SYSTEM_LIGHTMAP_PAGE_INVALID;
    }
    instances[0].m_DiffuseModulation.Init(0,1,0,1); instances[1].m_DiffuseModulation.Init(0,0,1,1);
    api.Bind(material); api.DrawInstances(2,instances); api.Bind(nullptr);
    require(callbacks==2,"Source instance list lost a material pass");
    source.setMaterialPassCallback([&](IMaterial*,const SourceMeshDraw& draw) {
        require(draw.vertices==skinned && draw.colors==baked,"Model instance lost its baked-lighting stream");
        api.BeginPass(modelSnapshot); api.SetVertexShaderIndex(3); api.SetPixelShaderIndex(0);
        api.SetPixelShaderConstant(0,ones);
        const float uvTransform[8]={1,0,0,0,0,1,0,0}; api.SetVertexShaderConstant(48,uvTransform,2);
        api.SetVertexShaderViewProj(); api.SetVertexShaderModelViewProjAndModelView();
        api.RenderPass(nullptr,0,1);
    });
    matrix3x4_t poses[2];
    const float pose0[12]={.125f,0,0,-.75f,0,1.f/3,0,0,0,0,1,.3f};
    const float pose1[12]={.125f,0,0,-.25f,0,1.f/3,0,0,0,0,1,.3f};
    std::memcpy(poses[0].Base(),pose0,sizeof(pose0)); std::memcpy(poses[1].Base(),pose1,sizeof(pose1));
    MeshBoneRemap_t remap[2]={{0,1},{1,0}};
    MeshInstanceData_t model {};
    model.m_pVertexBuffer=skinned; model.m_pIndexBuffer=skinned; model.m_pColorBuffer=baked;
    model.m_nColorVertexOffsetInBytes=8; model.m_nIndexCount=6; model.m_nPrimType=MATERIAL_TRIANGLES;
    model.m_nBoneCount=2; model.m_pBoneRemap=remap; model.m_pPoseToWorld=poses;
    model.m_DiffuseModulation.Init(1,1,1,1); model.m_nLightmapPageId=MATERIAL_SYSTEM_LIGHTMAP_PAGE_INVALID;
    api.SetNumBoneWeights(2); api.Bind(material); api.DrawInstances(1,&model); api.Bind(nullptr); api.SetNumBoneWeights(0);
    // This curve bows away from its chord. Sampling both locations catches a
    // shader that treats the rope's (t, v, side) parameters as XYZ positions.
    api.BeginPass(ropeSnapshot);api.SetVertexShaderIndex(0);api.SetPixelShaderIndex(0);
    api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,cableTexture);
    api.BindTexture(SHADER_SAMPLER1,TEXTURE_BINDFLAGS_NONE,cableTexture);
    auto* cable=api.GetDynamicMeshEx(nullptr,api.ComputeVertexFormat(1,&ropeSnapshot),0,false,nullptr,nullptr);
    MeshDesc_t cableLock;cable->LockMesh(18,48,cableLock);
    const float controlPoints[4][4]={{-.25f,1.5f,.2f,.1f},{.25f,-.5f,.2f,.1f},
        {.75f,-.5f,.2f,.1f},{1.25f,1.5f,.2f,.1f}};
    for(int v=0;v<18;++v) {
        const float parameters[3]={float(v/2)/8,float(v/2)/8,float(v%2)};
        const uint8_t cyan[4]={255,255,0,255};
        std::memcpy(reinterpret_cast<uint8_t*>(cableLock.m_pPosition)+v*cableLock.m_VertexSize_Position,parameters,sizeof(parameters));
        std::memcpy(cableLock.m_pColor+v*cableLock.m_VertexSize_Color,cyan,sizeof(cyan));
        for(int point=0;point<4;++point)
            std::memcpy(reinterpret_cast<uint8_t*>(cableLock.m_pTexCoord[point])+v*cableLock.m_VertexSize_TexCoord[point],controlPoints[point],16);
    }
    for(int segment=0;segment<8;++segment) {
        const uint16_t base=uint16_t(segment*2);
        const uint16_t indices[6]={base,uint16_t(base+1),uint16_t(base+2),uint16_t(base+1),uint16_t(base+3),uint16_t(base+2)};
        std::memcpy(cableLock.m_pIndices+segment*6,indices,sizeof(indices));
    }
    cable->UnlockMesh(18,48,cableLock);
    const float ropeConstants[8]={0,0,-2,0,float(Width),float(Height),1,0};
    api.SetVertexShaderConstant(48,ropeConstants,2);api.SetPixelShaderConstant(0,ones);api.SetVertexShaderViewProj();
    cable->Draw();
    api.BeginPass(postSnapshot);
    api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,bloomTexture);
    api.BindTexture(SHADER_SAMPLER1,TEXTURE_BINDFLAGS_NONE,frameTexture);
    api.BindTexture(SHADER_SAMPLER2,TEXTURE_BINDFLAGS_NONE,cableTexture);
    const float postConstants[16]={.25f,.25f,0,0,.5f,0,.2f,1,0,1,0,.25f,0,0,0,0};
    const float postUV[4]={0,0,1,1};api.SetVertexShaderConstant(48,postUV,1);api.SetPixelShaderConstant(0,postConstants,4);
    ShaderViewport_t postViewport;postViewport.Init(96,0,32,32);api.SetViewports(1,&postViewport);mesh->Draw();
    source.endTarget();
    blitAttachmentImage(context,frame,first,second,{{0,0},{Width,Height}},{{0,0},{Width/2,Height/2}});
    source.beginTarget(secondTarget);
    ShaderViewport_t viewport; viewport.Init(0,32,Width,Height); api.SetViewports(1,&viewport);
    api.GetViewports(&viewport,1);
    require(viewport.m_nWidth==64 && viewport.m_nHeight==16,"Source viewport was not clamped to the smaller target");
    api.ClearColor4ub(255,255,0,255); api.ClearBuffers(true,false,false,64,48);
    VkClearAttachment clear {}; clear.aspectMask=depth.aspect; clear.clearValue.depthStencil={.625f,7};
    const VkClearRect clearRect {{{0,32},{64,16}},0,1};
    context.vk().vkCmdClearAttachments(frame.commands,1,&clear,1,&clearRect);
    source.endTarget();source.beginTarget(firstTarget);
    ShaderStencilState_t stencilClear;stencilClear.m_bEnable=true;stencilClear.m_CompareFunc=SHADER_STENCILFUNC_EQUAL;
    stencilClear.m_nReferenceValue=7;stencilClear.m_nWriteMask=0;api.SetStencilState(stencilClear);
    api.BeginPass(clearSnapshot);fillMesh(mesh,{255,0,255,255});mesh->Draw();
    api.SetStencilState(ShaderStencilState_t());source.endTarget();
    auto read=[&](const Image& image,const Buffer& buffer,VkImageAspectFlags aspect,VkImageLayout oldLayout) {
        context.retain(frame,image); context.retain(frame,buffer);
        VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout=oldLayout; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT; barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=image.handle(); barrier.subresourceRange={image.description().aspect,0,1,0,1};
        context.vk().vkCmdPipelineBarrier(frame.commands,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy {}; copy.imageSubresource={aspect,0,0,1}; copy.imageExtent={image.description().width,image.description().height,1};
        context.vk().vkCmdCopyImageToBuffer(frame.commands,image.handle(),barrier.newLayout,buffer.handle(),1,&copy);
        std::swap(barrier.oldLayout,barrier.newLayout); barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT;
        context.vk().vkCmdPipelineBarrier(frame.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    };
    read(first,firstRead,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    read(second,secondRead,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    read(sharedDepth,depthRead,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    read(sharedDepth,stencilRead,VK_IMAGE_ASPECT_STENCIL_BIT,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkMemoryBarrier host {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    context.vk().vkCmdPipelineBarrier(frame.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    api.EndFrame(); device.interface().Present(); context.waitIdle();
    std::vector<uint8_t> a(Width*Height*4), b(Width*Height), stencil(Width*Height);
    std::vector<float> z(Width*Height);
    firstRead.read(0,a.data(),a.size()); secondRead.read(0,b.data(),b.size());
    depthRead.read(0,z.data(),z.size()*sizeof(float)); stencilRead.read(0,stencil.data(),stencil.size());
    auto pixel=[&](const auto& data,uint32_t width,uint32_t x,uint32_t y,std::array<int,4> expected) {
        for (size_t c=0;c<4;++c) require(std::abs(int(data[(y*width+x)*4+c])-expected[c])<=1,"Preserving target/copy/instance pixel mismatch");
    };
    pixel(a,Width,8,8,{255,0,0,255}); pixel(a,Width,40,8,{0,255,0,255}); pixel(a,Width,72,8,{0,0,255,255});
    pixel(a,Width,24,8,{0,0,0,0}); // Unwritten texels of the initially partial atlas are transparent.
    pixel(a,Width,112,80,{26,51,77,255});
    pixel(a,Width,112,8,{38,51,153,128});pixel(a,Width,8,40,{255,0,255,255});
    pixel(a,Width,96,84,{0,255,255,255});pixel(a,Width,96,72,{26,51,77,255});
    const int bakedRed=int(std::lround(std::pow(64.f/255*2,2.2f)*255));
    pixel(a,Width,24,48,{bakedRed,0,0,255}); pixel(a,Width,8,48,{26,51,77,255}); pixel(a,Width,40,48,{26,51,77,255});
    pixel(b,64,4,4,{255,0,0,255}); pixel(b,64,20,4,{0,255,0,255}); pixel(b,64,36,4,{0,0,255,255});
    pixel(b,64,8,40,{255,255,0,255});pixel(b,64,60,4,{38,51,153,128});
    require(std::abs(z[8*Width+8]-.4f)<1e-5f && std::abs(z[40*Width+8]-.625f)<1e-5f &&
        stencil[40*Width+8]==7 && stencil[8*Width+8]==0,"Shared target depth/stencil was not preserved");
    savePPM(output/"engine-path-readback.ppm",a);
    // Exercise the actual material transform, including the camera-space
    // particle override, a changed view, and restoration after disabling clip.
    require(source.beginFrame(),"Clip fixture frame was unavailable");frame=source.frame();
    source.beginTarget(firstTarget);api.ClearColor4ub(0,0,0,255);api.ClearBuffers(true,true,false,Width,Height);
    api.BeginPass(clipSnapshot);api.SetVertexShaderIndex(0);api.SetPixelShaderIndex(0);
    api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_NONE,cableTexture);
    const float green[4]={0,1,0,1},clipUV[8]={1,0,0,0,0,1,0,0},plane[4]={1,0,0,0};
    api.SetPixelShaderConstant(0,green);api.SetVertexShaderConstant(48,clipUV,2);
    api.MatrixMode(MATERIAL_MODEL);api.LoadIdentity();api.Translate(0,0,-2);api.Scale(2,2,1);
    api.MatrixMode(MATERIAL_VIEW);api.LoadIdentity();
    api.MatrixMode(MATERIAL_PROJECTION);api.LoadIdentity();api.PerspectiveX(90,1,1,101);
    api.SetFastClipPlane(plane);api.EnableFastClip(true);
    auto clipDraw=[&](int x,int y) {
        ShaderViewport_t view;view.Init(x,y,Width/2,Height/2);api.SetViewports(1,&view);
        api.SetVertexShaderModelViewProjAndModelView();mesh->Draw();
    };
    clipDraw(0,0);
    VMatrix oldView;
    for(int row=0;row<4;++row)for(int col=0;col<4;++col)oldView[row][col]=float(row==col);
    oldView[0][3]=-.5f;
    api.EnableUserClipTransformOverride(true);api.UserClipTransform(oldView);clipDraw(Width/2,0);
    api.EnableUserClipTransformOverride(false);api.MatrixMode(MATERIAL_VIEW);api.Translate(.5f,0,0);
    clipDraw(0,Height/2);
    api.EnableFastClip(false);api.LoadIdentity();clipDraw(Width/2,Height/2);
    float rawProjection[16],actualProjection[16];
    api.GetMatrix(MATERIAL_PROJECTION,rawProjection);api.GetActualProjectionMatrix(actualProjection);
    require(std::equal(rawProjection,rawProjection+16,actualProjection),"Disabling fast clip left an oblique projection");
    source.endTarget();read(first,firstRead,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    context.vk().vkCmdPipelineBarrier(frame.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    api.EndFrame();device.interface().Present();context.waitIdle();firstRead.read(0,a.data(),a.size());
    pixel(a,Width,28,24,{0,0,0,255});pixel(a,Width,36,24,{0,255,0,255});
    pixel(a,Width,84,24,{0,0,0,255});pixel(a,Width,92,24,{0,255,0,255});
    pixel(a,Width,36,72,{0,0,0,255});pixel(a,Width,44,72,{0,255,0,255});
    pixel(a,Width,72,72,{0,255,0,255});pixel(a,Width,120,72,{0,255,0,255});
    savePPM(output/"clip-readback.ppm",a);
    // Match MaterialSystem::DrawElements: SetDefaultState runs AFTER the
    // Panorama wrapper has set context overrides. Both triangle windings must
    // survive, clipped by the intersection of scissor and stencil.
    require(source.beginFrame(),"Panorama fixture frame was unavailable");frame=source.frame();
    source.beginTarget(firstTarget);api.ClearColor4ub(0,0,51,51);api.ClearBuffers(true,true,true,Width,Height);
    VkClearAttachment panelStencil {};panelStencil.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;panelStencil.clearValue.depthStencil={1,7};
    const VkClearRect panelRect {{{16,16},{64,64}},0,1};
    context.vk().vkCmdClearAttachments(frame.commands,1,&panelStencil,1,&panelRect);
    auto* panel=api.GetDynamicMeshEx(nullptr,api.ComputeVertexFormat(1,&panelSnapshot),0,false,nullptr,nullptr);
    MeshDesc_t panelLock;panel->LockMesh(6,6,panelLock);
    const float panelPositions[4][3]={{-1,1,0},{1,1,0},{1,-1,0},{-1,-1,0}};
    const int panelOrder[6]={0,2,1,0,2,3};
    const float panelUV[5][4]={{0,0,0,0},{0,.5f,0,.5f},{0,.5f,0,.5f},{0,0,0,0},{0,0,128,96}};
    for(int v=0;v<6;++v) {
        std::memcpy(reinterpret_cast<uint8_t*>(panelLock.m_pPosition)+v*panelLock.m_VertexSize_Position,panelPositions[panelOrder[v]],12);
        for(int uv=0;uv<5;++uv)std::memcpy(reinterpret_cast<uint8_t*>(panelLock.m_pTexCoord[uv])+v*panelLock.m_VertexSize_TexCoord[uv],panelUV[uv],16);
        panelLock.m_pIndices[v]=uint16_t(v);
    }
    panel->UnlockMesh(6,6,panelLock);
    source.setMaterialPassCallback([&](IMaterial*,const SourceMeshDraw&) {
        api.SetDefaultState();api.BeginPass(panelSnapshot);api.RenderPass(nullptr,0,1);
    });
    api.CullMode(MATERIAL_CULLMODE_NONE);api.SetScissorRect(32,24,96,72,true);api.SetStencilState(stencilClear);
    api.OverrideDepthEnable(true,false,false);api.OverrideColorWriteEnable(true,true);api.OverrideAlphaWriteEnable(true,true);
    const auto query=api.CreateOcclusionQueryObject();
    require(bool(query)==bool(context.capabilities().enabledFeatures.occlusionQueryPrecise),"Precise Source query capability mismatch");
    if(query)api.BeginOcclusionQueryDrawing(query);
    api.Bind(material);panel->Draw();
    // A logical query may cross attachment views/passes. Count both draws,
    // retaining the first draw's color for the independent Panorama checks.
    source.endTarget();source.beginTarget(firstTarget);api.SetScissorRect(32,24,96,72,true);
    api.OverrideColorWriteEnable(true,false);api.OverrideAlphaWriteEnable(true,false);panel->Draw();api.Bind(nullptr);
    if(query) {
        api.EndOcclusionQueryDrawing(query);
        require(api.OcclusionQuery_GetNumPixelsRendered(query,false)==OCCLUSION_QUERY_RESULT_PENDING,
            "Unsubmitted Source query exposed stale results");
    }
    api.OverrideDepthEnable(false,false,false);api.OverrideColorWriteEnable(false,false);api.OverrideAlphaWriteEnable(false,false);
    api.SetStencilState(ShaderStencilState_t());api.SetScissorRect(0,0,0,0,false);
    source.endTarget();read(first,firstRead,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    context.vk().vkCmdPipelineBarrier(frame.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    api.EndFrame();device.interface().Present();context.waitIdle();firstRead.read(0,a.data(),a.size());
    pixel(a,Width,40,56,{0,128,26,153});pixel(a,Width,72,32,{0,128,26,153});
    pixel(a,Width,24,40,{0,0,51,51});pixel(a,Width,88,40,{0,0,51,51});pixel(a,Width,40,80,{0,0,51,51});
    savePPM(output/"panorama-state-readback.ppm",a);
    if(query) {
        require(api.OcclusionQuery_GetNumPixelsRendered(query,false)==2*48*48,"Source query lost precise samples across render passes");
        require(source.beginFrame(),"Query reuse frame was unavailable");frame=source.frame();source.beginTarget(firstTarget);
        api.CullMode(MATERIAL_CULLMODE_NONE);api.SetScissorRect(0,0,16,8,true);api.OverrideDepthEnable(true,false,false);
        api.OverrideColorWriteEnable(true,false);api.OverrideAlphaWriteEnable(true,false);
        api.BeginOcclusionQueryDrawing(query);api.Bind(material);panel->Draw();api.Bind(nullptr);api.EndOcclusionQueryDrawing(query);
        require(api.OcclusionQuery_GetNumPixelsRendered(query,false)==OCCLUSION_QUERY_RESULT_PENDING,
            "Reused Source query returned the previous frame's count");
        require(api.OcclusionQuery_GetNumPixelsRendered(query,true)==16*8,"Explicit Source query flush did not submit its frame prefix");
        api.DestroyOcclusionQueryObject(query);
        require(api.OcclusionQuery_GetNumPixelsRendered(query,false)==OCCLUSION_QUERY_RESULT_ERROR,"Destroyed Source query remained valid");
        api.EndFrame();device.interface().Present();context.waitIdle();
        const auto counts=source.statistics().queries;
        require(counts.liveQueries==0 && counts.begins==2 && counts.segments==3 && counts.prefixFlushes==1 && counts.forcedWaits==0,
            "Source queries performed unexpected waits or leaked logical handles");
    }
    // Source's integer HDR lightmap stores light / 16, with exposure applied
    // in the material before the sRGB write. Test that path alongside two
    // independently transformed, sRGB-decoded textures used by sky clouds.
    const auto hdrLight=api.CreateTexture(1,1,1,IMAGE_FORMAT_RGBA16161616,1,1,0,"HDR lightmap fixture","Probe");
    uint16_t hdrPixel[4]={4096,2048,1024,65535};api.ModifyTexture(hdrLight);
    api.TexImage2D(0,0,IMAGE_FORMAT_RGBA16161616,0,1,1,IMAGE_FORMAT_RGBA16161616,false,hdrPixel);
    const auto dualFirst=api.CreateTexture(1,1,1,IMAGE_FORMAT_RGBA8888,1,1,TEXTURE_CREATE_SRGB,"Dual base fixture","Probe");
    const auto dualSecond=api.CreateTexture(2,1,1,IMAGE_FORMAT_RGBA8888,1,1,TEXTURE_CREATE_SRGB,"Dual second fixture","Probe");
    uint8_t dualBasePixel[4]={128,64,255,255},dualSecondPixels[8]={255,128,128,255,128,255,255,128};
    api.ModifyTexture(dualFirst);api.TexImage2D(0,0,IMAGE_FORMAT_RGBA8888,0,1,1,IMAGE_FORMAT_RGBA8888,false,dualBasePixel);
    api.ModifyTexture(dualSecond);api.TexImage2D(0,0,IMAGE_FORMAT_RGBA8888,0,2,1,IMAGE_FORMAT_RGBA8888,false,dualSecondPixels);
    auto* lightmapMesh=device.interface().CreateStaticMesh(api.ComputeVertexFormat(1,&hdrSnapshot),"HDR fixture",nullptr,nullptr);
    MeshDesc_t lightmapLock;lightmapMesh->LockMesh(4,6,lightmapLock);
    for(int v=0;v<4;++v) {
        std::memcpy(reinterpret_cast<uint8_t*>(lightmapLock.m_pPosition)+v*lightmapLock.m_VertexSize_Position,positions[v],12);
        const float uv[2]={0,0};
        for(int stage=0;stage<2;++stage)std::memcpy(reinterpret_cast<uint8_t*>(lightmapLock.m_pTexCoord[stage])+v*lightmapLock.m_VertexSize_TexCoord[stage],uv,8);
    }
    std::memcpy(lightmapLock.m_pIndices,skinIndices,sizeof(skinIndices));lightmapMesh->UnlockMesh(4,6,lightmapLock);
    RenderTarget hdrTarget(context,first.samplingView(true),sharedDepth);
    require(source.beginFrame(),"HDR material frame was unavailable");frame=source.frame();source.beginTarget(hdrTarget);
    api.ResetRenderState(true,false);
    for(auto mode:{MATERIAL_MODEL,MATERIAL_VIEW,MATERIAL_PROJECTION}) {api.MatrixMode(mode);api.LoadIdentity();}
    api.SetVertexShaderModelViewProjAndModelView();
    const float identityUV[8]={1,0,0,0,0,1,0,0},halfExposure[4]={8,8,8,1};
    api.SetVertexShaderConstant(48,identityUV,2);api.SetPixelShaderConstant(0,halfExposure);
    api.BeginPass(hdrSnapshot);api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,cableTexture);
    api.BindTexture(SHADER_SAMPLER1,TEXTURE_BINDFLAGS_NONE,hdrLight);
    ShaderViewport_t hdrViewport;hdrViewport.Init(0,0,Width/2,Height);api.SetViewports(1,&hdrViewport);lightmapMesh->Draw();
    api.BeginPass(dualSnapshot);api.BindTexture(SHADER_SAMPLER0,TEXTURE_BINDFLAGS_SRGBREAD,dualFirst);
    api.BindTexture(SHADER_SAMPLER1,TEXTURE_BINDFLAGS_SRGBREAD,dualSecond);api.SetPixelShaderConstant(0,ones);
    const float secondUV[8]={0,0,0,.75f,0,0,0,.5f};api.SetVertexShaderConstant(50,secondUV,2);
    hdrViewport.Init(Width/2,0,Width/2,Height);api.SetViewports(1,&hdrViewport);mesh->Draw();
    source.endTarget();read(first,firstRead,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    context.vk().vkCmdPipelineBarrier(frame.commands,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    api.EndFrame();device.interface().Present();context.waitIdle();firstRead.read(0,a.data(),a.size());
    pixel(a,Width,32,48,{188,137,99,255});pixel(a,Width,96,48,{61,64,255,255});
    savePPM(output/"hdr-material-readback.ppm",a);
    api.DeleteTexture(hdrLight);api.DeleteTexture(dualFirst);api.DeleteTexture(dualSecond);device.interface().DestroyStaticMesh(lightmapMesh);
    api.DeleteTexture(white);api.DeleteTexture(cableTexture);device.interface().DestroyStaticMesh(mesh);device.interface().DestroyIndexBuffer(index32);
    api.DeleteTexture(bloomTexture);api.DeleteTexture(frameTexture);
    device.interface().DestroyStaticMesh(skinned); device.interface().DestroyStaticMesh(baked);
    manager.Shutdown(); manager.Disconnect();
    log("VK_SOURCE_ENGINE_PATH_PASS: pre_frame_clear=pass preserving_targets=pass scaled_copy=pass shared_depth_stencil=pass initial_partial_atlas=pass static_vertex_override=pass index_only_batch=pass instance_materials=2 skinning_remap=pass baked_color_stream=pass spline_rope=pass post_bloom_fade=pass stencil_clear=pass fast_clip=pass clip_transform_override=pass panorama_context_overrides=pass integer_HDR=pass dual_texture_transform=pass precise_occlusion="+std::string(query?"pass":"unavailable")+" BC1_BC2_BC3="+std::string(nativeBC?"native":"CPU_fallback"));
}
} // namespace

int main(int argc,char** argv) {
    SDL_Window* window=nullptr;
    std::ofstream report;
    int result=0;
    std::atomic<uint32_t> errors {0};
    LogSink log = [&](const std::string& message) {
        if (message.rfind("VK_VALIDATION_ERROR:",0)==0) ++errors;
        std::fprintf(stderr,"%s\n",message.c_str());
        if (report.is_open()) { report<<message<<'\n'; report.flush(); }
    };
    try {
        require(argc>=3,"Usage: csgo-vulkan-api-probe SHADER_DIRECTORY OUTPUT_DIRECTORY [--no-bc] [--no-anisotropy]");
        bool noBC=false,noAnisotropy=false;
        for(int i=3;i<argc;++i) {
            if(std::string(argv[i])=="--no-bc")noBC=true;
            else if(std::string(argv[i])=="--no-anisotropy")noAnisotropy=true;
            else throw std::invalid_argument("Unknown Source API probe option");
        }
        std::filesystem::create_directories(argv[2]);
        report.open(std::filesystem::path(argv[2])/"api-probe.log"); require(bool(report),"Cannot open Source API log");
        require(SDL_Init(SDL_INIT_VIDEO),SDL_GetError());
        window=SDL_CreateWindow("Source Vulkan ShaderApi029 acceptance",640,480,SDL_WINDOW_VULKAN|SDL_WINDOW_RESIZABLE);
        require(window,SDL_GetError());
        {
            Context context(window,{true,log,!noBC,!noAnisotropy});
            exercise(context,window,argv[1],argv[2],log);
            exerciseFiltering(context,window,argv[1],argv[2],log);
            exerciseShadows(context,window,argv[1],argv[2],log);
            exercisePreFrameClears(context,log);
            exerciseEnginePaths(context,window,argv[1],argv[2],log);
            require(context.allocationStatistics().allocations==0,"Source API leaked VMA allocations");
        }
        require(errors==0,"Source API validation reported an error during drawing or cleanup");
        log("VK_SOURCE_API_CLEANUP_PASS: validation_errors=0 live_allocations=0");
    } catch (const std::exception& error) { log("VK_SOURCE_API_FAIL: "+std::string(error.what())); result=1; }
    if (window) SDL_DestroyWindow(window);
    SDL_Quit(); return result;
}
