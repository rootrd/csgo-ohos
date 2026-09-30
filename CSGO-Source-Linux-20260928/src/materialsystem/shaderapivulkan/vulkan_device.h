#pragma once

#include "vulkan_material.h"

class IShaderDevice;
class IShaderDeviceMgr;
class IShaderAPI;
class IVertexBuffer;
class IIndexBuffer;
class IMesh;
struct MeshInstanceData_t;

namespace sourcevk {
struct SourceDeviceLimits {
    size_t maximumBufferBytes = 64 * 1024 * 1024;
    size_t maximumShadowBytes = 256 * 1024 * 1024;
    size_t dynamicVertexBytes = 256 * 1024, dynamicIndexBytes = 128 * 1024;
};
struct SourceDeviceStatistics {
    uint64_t uploadSubmissions = 0, bufferUploads = 0, dynamicCopies = 0, meshDraws = 0;
    uint64_t uploadInspections = 0;
    size_t shadowBytes = 0, vertexBuffers = 0, indexBuffers = 0, meshes = 0, shaders = 0;
};
struct SourceMeshDraw {
    IMesh* mesh = nullptr;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    uint32_t firstIndex = 0, indexCount = 0;
    std::array<float, 4> modulation {1,1,1,1};
    const IVertexBuffer* vertices = nullptr;
    const IIndexBuffer* indices = nullptr;
    const IVertexBuffer* colors = nullptr;
    uint32_t vertexOffset = 0, colorOffset = 0;
    const MeshInstanceData_t* instance = nullptr;
};

// Source device interfaces for an already selected Context/SDL window. These
// objects do not own that Context, GraphicsDevice, FrameArena or SourceShadow.
// The manager exposes the attached physical device as logical adapter 0; full
// pre-window discovery and the game module factory live in vulkan_module.cpp.
class SourceDevice {
public:
    SourceDevice(Context& context, GraphicsDevice& graphics, FrameArena& dynamic,
                 SourceShadow& shadow, SDL_Window* window, SourceDeviceLimits limits = {});
    ~SourceDevice();
    SourceDevice(const SourceDevice&) = delete;
    SourceDevice& operator=(const SourceDevice&) = delete;
    IShaderDevice& interface();
    IShaderDeviceMgr& manager();
    void* queryInterface(const char* name, int* status = nullptr);
    // Static Lock/Modify commits become visible together in this one upload
    // batch. Updates replace GPU allocations, preserving earlier recorded uses.
    // Copy-on-write uploads may also precede a still-recording frame submission;
    // beginFrame flushes any pending static uploads as well.
    void flushUploads(const std::function<void(UploadBatch&)>& additional = {});
    bool beginFrame(Frame& frame);
    bool frameActive() const;
    // One device-owned dynamic mesh, valid until the next request. Its GPU
    // slices are immutable within a frame, just like dynamic buffer slices.
    IMesh* dynamicMesh(uint64_t format, IMesh* vertices = nullptr, IMesh* indices = nullptr);
    void attachAPI(IShaderAPI* api, std::function<void(bool)> releaseResources = {});
    void detachAPI(IShaderAPI* api);
    // Present on IShaderDevice submits the frame; close all render passes first.
    BufferSlice vertexSlice(const IVertexBuffer* buffer, const Frame& frame);
    uint64_t vertexFormat(const IVertexBuffer* buffer) const;
    void traceVertexData(const IVertexBuffer* buffer, uint32_t byteOffset) const;
    BufferSlice indexSlice(const IIndexBuffer* buffer, const Frame& frame);
    VkIndexType indexType(const IIndexBuffer* buffer) const;
    void validateIndexedDraw(const IVertexBuffer* vertices, size_t vertexByteOffset, uint32_t firstVertex,
        uint32_t vertexCount, const IIndexBuffer* indices, size_t indexByteOffset,
        uint32_t firstIndex, uint32_t indexCount) const;
    Shader shader(const void* handle, SourceShaderStage stage) const;
    void setDrawSink(std::function<void(const SourceMeshDraw&)> sink);
    SourceDeviceStatistics statistics() const;
    int maximumVertices(uint64_t format) const;
    int maximumIndices() const;
    int dynamicVertexBytes() const;
    // Exponent, output scale and bias, applied to gamma-encoded presentation.
    std::array<float,4> outputGamma() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sourcevk
