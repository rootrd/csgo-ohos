#include "vulkan_device.h"
#include "vulkan_internal.h"

#include "vulkan_platform.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <stdexcept>

#include "shaderapi/IShaderDevice.h"
#include "shaderapi/ishaderapi.h"
#include "shaderapi/ishadershadow.h"
#include "../shaderapidx9/meshbase.h"

namespace sourcevk {
namespace {
void require(bool value, const char* message) { if (!value) throw std::invalid_argument(message); }
[[noreturn]] void unsupported(const char* operation) { throw std::logic_error(std::string("Source Vulkan does not implement ") + operation); }
struct Storage;
struct Budget {
    SourceDeviceLimits limits;
    size_t bytes = 0, lockedBuffers = 0;
    std::set<Storage*> pendingUploads {};
};
std::atomic<uintptr_t> nextSourceShader {1};
struct Storage {
    std::shared_ptr<Budget> budget;
    std::vector<uint8_t> data, scratch;
    bool dynamic, vertex, locked = false, modifying = false, readOnly = false;
    size_t stride, used = 0, cursor = 0, lockStart = 0, lockCount = 0;
    uint64_t revision = 1, gpuRevision = 0, cachedRevision = 0, cachedFrame = 0;
    Buffer gpu;
    BufferSlice cached;
    Storage(std::shared_ptr<Budget> owner, bool isDynamic, bool isVertex, size_t element, size_t capacity)
        : budget(std::move(owner)), dynamic(isDynamic), vertex(isVertex), stride(element) { resize(capacity); }
    ~Storage() {
        budget->pendingUploads.erase(this);
        if(locked)--budget->lockedBuffers;
        budget->bytes -= data.capacity() + scratch.capacity();
    }
    void resizeManaged(std::vector<uint8_t>& target, size_t bytes) {
        require(bytes <= budget->limits.maximumBufferBytes, "Source buffer exceeds its capacity limit");
        if (bytes <= target.capacity()) { target.resize(bytes); return; }
        const auto previous = target.capacity();
        if (bytes - previous > budget->limits.maximumShadowBytes - budget->bytes)
            throw std::length_error("Source CPU shadow/lock-buffer budget exhausted: used="+std::to_string(budget->bytes)+
                " additional="+std::to_string(bytes-previous)+" limit="+std::to_string(budget->limits.maximumShadowBytes));
        // Avoid vector growth doubling capacity beyond the declared budget.
        std::vector<uint8_t> replacement(bytes);
        require(replacement.capacity() - previous <= budget->limits.maximumShadowBytes - budget->bytes, "Source CPU allocation exceeded its budget");
        std::copy(target.begin(), target.end(), replacement.begin());
        budget->bytes += replacement.capacity() - previous;
        target.swap(replacement);
    }
    void resize(size_t bytes) {
        require(!locked && bytes <= budget->limits.maximumBufferBytes, "Source buffer exceeds its capacity limit or is locked");
        if (bytes <= data.size()) return;
        resizeManaged(data, bytes);
    }
    bool lock(int count, bool append) {
        require(!locked && stride && count > 0, "Invalid Source buffer lock or missing dynamic cast");
        const size_t start = dynamic && append ? cursor : 0;
        if (start > data.size() || size_t(count) > (data.size() - start) / stride) return false;
        resizeManaged(scratch, size_t(count) * stride);
        std::fill(scratch.begin(), scratch.end(), 0);
        lockStart = start; lockCount = size_t(count); locked = true; modifying = readOnly = false;
        ++budget->lockedBuffers;
        return true;
    }
    void modify(bool read, int first, int count) {
        require(!locked && stride && first >= 0 && count > 0 && size_t(first) <= used / stride &&
            size_t(count) <= used / stride - size_t(first), "Source modify range is outside initialized data");
        const size_t start = size_t(first) * stride;
        resizeManaged(scratch, size_t(count) * stride);
        std::copy_n(data.begin() + start, scratch.size(), scratch.begin());
        lockStart = start; lockCount = size_t(count); locked = modifying = true; readOnly = read;
        ++budget->lockedBuffers;
    }
    void finish(int count, bool modification) {
        require(locked && modifying == modification && count >= 0 && size_t(count) <= lockCount,
                "Source unlock does not match its lock or written count");
        if (!readOnly) {
            const auto bytes = size_t(count) * stride;
            const auto nextUsed = modifying ? used : lockStart + bytes;
            if (used != nextUsed || (bytes && std::memcmp(data.data() + lockStart, scratch.data(), bytes))) {
                if(!dynamic)budget->pendingUploads.insert(this);
                if (bytes) std::memcpy(data.data() + lockStart, scratch.data(), bytes);
                used = nextUsed; ++revision;
            }
            if (!modifying) cursor = used;
        }
        cancel();
    }
    void cancel() {
        if(locked)--budget->lockedBuffers;
        locked = modifying = readOnly = false;
        if (dynamic) scratch.clear();
        else {
            // Static geometry keeps its recovery shadow, but needs lock staging
            // only while being edited. Retaining both doubled whole-map memory.
            budget->bytes-=scratch.capacity(); std::vector<uint8_t>().swap(scratch);
        }
    }
    void cast(size_t element) {
        require(dynamic && !locked && element, "Only unlocked dynamic buffers can change format");
        const auto aligned = (cursor + element - 1) / element * element;
        stride = element; cursor = std::min(aligned, data.size());
    }
    void validate(int count) const { require(locked && count >= 0 && size_t(count) <= lockCount, "Invalid Source locked-data range"); }
    int room() const { return stride ? int((data.size() - cursor) / stride) : 0; }
};
bool dynamicType(ShaderBufferType_t type) {
    require(type == SHADER_BUFFER_TYPE_STATIC || type == SHADER_BUFFER_TYPE_DYNAMIC, "Temporary Source buffers require a separate temporary-mesh path");
    return type == SHADER_BUFFER_TYPE_DYNAMIC;
}
size_t capacityBytes(int count, size_t stride) {
    require(count >= 0 && uint64_t(count) * stride <= 64 * 1024 * 1024, "Invalid Source buffer count or byte capacity");
    return size_t(count) * stride;
}
struct VertexBuffer final : IVertexBuffer {
    VertexFormat_t format;
    bool castActive = false;
    Storage storage;
    VertexBuffer(std::shared_ptr<Budget> budget, bool dynamic, VertexFormat_t value, int count)
        : format(value), storage(std::move(budget), dynamic, true, value ? sourceVertexLayout(value).stride : 0,
              capacityBytes(count, value ? sourceVertexLayout(value).stride : 1)) {
        require(dynamic || value, "Static Source vertices require a concrete format");
        castActive = value != 0;
    }
    int VertexCount() const override { return storage.stride ? int(storage.data.size() / storage.stride) : 0; }
    VertexFormat_t GetVertexFormat() const override { return castActive ? format : 0; }
    bool IsDynamic() const override { return storage.dynamic; }
    void BeginCastBuffer(VertexFormat_t value) override {
        require(!castActive || format == value, "End the previous Source vertex cast before changing format");
        storage.cast(sourceVertexLayout(value).stride); format = value; castActive = true;
    }
    void EndCastBuffer() override { require(storage.dynamic && !storage.locked && castActive, "Invalid Source vertex cast end"); castActive = false; }
    int GetRoomRemaining() const override { return castActive ? storage.room() : 0; }
    void describe(VertexDesc_t& desc) {
        desc = {};
        ComputeVertexDesc<false>(storage.scratch.data(), format, desc);
        desc.m_nFirstVertex = 0; desc.m_nOffset = uint32_t(storage.lockStart);
    }
    bool Lock(int count, bool append, VertexDesc_t& desc) override {
        desc = {};
        require(castActive, "Source dynamic vertex buffer must be cast before Lock");
        if (!storage.lock(count, append)) return false;
        describe(desc); return true;
    }
    void Unlock(int count, VertexDesc_t& desc) override { ValidateData(count, desc); storage.finish(count, false); }
    void Spew(int count, const VertexDesc_t& desc) override { ValidateData(count, desc); }
    void ValidateData(int count, const VertexDesc_t& desc) override {
        storage.validate(count);
        require(desc.m_ActualVertexSize == int(storage.stride) && desc.m_nOffset == storage.lockStart,
                "Source vertex descriptor does not match its lock");
    }
};
size_t indexSize(MaterialIndexFormat_t format) {
    require(format == MATERIAL_INDEX_FORMAT_16BIT || format == MATERIAL_INDEX_FORMAT_32BIT, "Unknown Source index format");
    return format == MATERIAL_INDEX_FORMAT_16BIT ? 2 : 4;
}
struct IndexBuffer final : IIndexBuffer {
    MaterialIndexFormat_t format;
    bool castActive = true;
    Storage storage;
    IndexBuffer(std::shared_ptr<Budget> budget, bool dynamic, MaterialIndexFormat_t value, int count)
        : format(value == MATERIAL_INDEX_FORMAT_UNKNOWN ? MATERIAL_INDEX_FORMAT_16BIT : value),
          storage(std::move(budget), dynamic, false, indexSize(format), capacityBytes(count, value == MATERIAL_INDEX_FORMAT_UNKNOWN ? 1 : indexSize(format))) {
        require(dynamic || value != MATERIAL_INDEX_FORMAT_UNKNOWN, "Static Source indices require a concrete format");
    }
    int IndexCount() const override { return int(storage.data.size() / storage.stride); }
    MaterialIndexFormat_t IndexFormat() const override { return castActive ? format : MATERIAL_INDEX_FORMAT_UNKNOWN; }
    bool IsDynamic() const override { return storage.dynamic; }
    void BeginCastBuffer(MaterialIndexFormat_t value) override {
        // Device-owned typeless buffers start with the legacy 16-bit default.
        // The first explicit cast may select 32-bit indices while still empty.
        require(!castActive || format == value || !storage.used, "End the previous Source index cast before changing format");
        storage.cast(indexSize(value)); format = value; castActive = true;
    }
    void EndCastBuffer() override { require(storage.dynamic && !storage.locked && castActive, "Invalid Source index cast end"); castActive = false; }
    int GetRoomRemaining() const override { return castActive ? storage.room() : 0; }
    void describe(IndexDesc_t& desc) {
        desc = {};
        desc.m_pIndices = reinterpret_cast<unsigned short*>(storage.scratch.data());
        desc.m_nOffset = uint32_t(storage.lockStart); desc.m_nFirstIndex = 0;
        desc.m_nIndexSize = uint32_t(storage.stride / 2);
    }
    bool Lock(int count, bool append, IndexDesc_t& desc) override {
        desc = {};
        require(castActive, "Source dynamic index buffer must be cast before Lock");
        if (!storage.lock(count, append)) return false;
        describe(desc); return true;
    }
    void Unlock(int count, IndexDesc_t& desc) override { ValidateData(count, desc); storage.finish(count, false); }
    void ModifyBegin(bool readOnly, int first, int count, IndexDesc_t& desc) override { storage.modify(readOnly, first, count); describe(desc); }
    void ModifyEnd(IndexDesc_t& desc) override { ValidateData(int(storage.lockCount), desc); storage.finish(int(storage.lockCount), true); }
    void Spew(int count, const IndexDesc_t& desc) override { ValidateData(count, desc); }
    void ValidateData(int count, const IndexDesc_t& desc) override {
        storage.validate(count);
        require(desc.m_pIndices == reinterpret_cast<const unsigned short*>(storage.scratch.data()) &&
            desc.m_nOffset == storage.lockStart && desc.m_nIndexSize == storage.stride / 2, "Source index descriptor does not match its lock");
    }
    IMesh* GetMesh() override { return nullptr; }
};

struct Mesh final : IMesh {
    VertexBuffer vertex;
    IndexBuffer index;
    VertexBuffer* vertexOverride = nullptr;
    IndexBuffer* indexOverride = nullptr;
    IMesh* colorMesh = nullptr;
    uint32_t colorOffset = 0;
    alignas(16) std::array<unsigned char,1024> ignoredVertex {};
    unsigned short ignoredIndex = 0;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    std::function<void(const SourceMeshDraw&)>& dispatch;
    bool meshLock = false, modification = false, lockVertex = false, lockIndex = false;
    uint64_t validatedVertexRevision = 0, validatedIndexRevision = 0;
    int validatedFirstIndex = -1, validatedIndexCount = 0;
    Mesh(std::shared_ptr<Budget> budget, VertexFormat_t format, std::function<void(const SourceMeshDraw&)>& sink, bool dynamic = false)
        : vertex(budget, dynamic, format, 0), index(std::move(budget), dynamic, MATERIAL_INDEX_FORMAT_16BIT, 0), dispatch(sink) {}
    VertexBuffer& effectiveVertex() const { return vertexOverride ? *vertexOverride : const_cast<VertexBuffer&>(vertex); }
    IndexBuffer& effectiveIndex() const { return indexOverride ? *indexOverride : const_cast<IndexBuffer&>(index); }
    int VertexCount() const override { const auto& v=effectiveVertex().storage; return int(v.used / v.stride); }
    VertexFormat_t GetVertexFormat() const override { return effectiveVertex().GetVertexFormat(); }
    int IndexCount() const override { const auto& i=effectiveIndex().storage; return int(i.used / i.stride); }
    MaterialIndexFormat_t IndexFormat() const override { return effectiveIndex().IndexFormat(); }
    bool IsDynamic() const override { return vertex.storage.dynamic; }
    void BeginCastBuffer(VertexFormat_t) override { unsupported("static mesh casts"); }
    void BeginCastBuffer(MaterialIndexFormat_t) override { unsupported("static mesh casts"); }
    void EndCastBuffer() override { unsupported("static mesh casts"); }
    int GetRoomRemaining() const override { return std::min(vertex.GetRoomRemaining(), index.GetRoomRemaining()); }
    bool Lock(int count, bool append, VertexDesc_t& desc) override { require(!meshLock, "Mesh is already locked"); return vertex.Lock(count, append, desc); }
    bool Lock(int count, bool append, IndexDesc_t& desc) override { require(!meshLock, "Mesh is already locked"); return index.Lock(count, append, desc); }
    void Unlock(int count, VertexDesc_t& desc) override { require(!meshLock, "Use UnlockMesh for a mesh lock"); vertex.Unlock(count, desc); }
    void Unlock(int count, IndexDesc_t& desc) override { require(!meshLock, "Use UnlockMesh for a mesh lock"); index.Unlock(count, desc); }
    void Spew(int count, const VertexDesc_t& desc) override { vertex.Spew(count, desc); }
    void Spew(int count, const IndexDesc_t& desc) override { index.Spew(count, desc); }
    void ValidateData(int count, const VertexDesc_t& desc) override { vertex.ValidateData(count, desc); }
    void ValidateData(int count, const IndexDesc_t& desc) override { index.ValidateData(count, desc); }
    IMesh* GetMesh() override { return this; }
    void ModifyBegin(bool readOnly, int first, int count, IndexDesc_t& desc) override {
        require(!meshLock, "Use ModifyBeginEx for a mesh modification"); index.ModifyBegin(readOnly, first, count, desc);
    }
    void ModifyEnd(IndexDesc_t& desc) override { require(!meshLock, "Use ModifyEnd(MeshDesc) for a mesh modification"); index.ModifyEnd(desc); }
    void SetPrimitiveType(MaterialPrimitiveType_t type) override {
        switch (type) {
        case MATERIAL_TRIANGLES: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case MATERIAL_TRIANGLE_STRIP: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case MATERIAL_LINES: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case MATERIAL_HETEROGENOUS: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break; // Vertex-only/color storage.
        default: unsupported("this Source mesh primitive type");
        }
    }
    void LockMesh(int vertices, int indices, MeshDesc_t& desc, MeshBuffersAllocationSettings_t* settings) override {
        if (vertexOverride) vertices=0;
        if (indexOverride) indices=0;
        require(!meshLock && !vertex.storage.locked && !index.storage.locked && vertices >= 0 && indices >= -1 &&
            (vertices || indices > 0 || vertexOverride || indexOverride) &&
            (!settings || !settings->m_uiIbUsageFlags), "Invalid Source mesh lock or allocation settings");
        if (IsDynamic()) require(capacityBytes(vertices, vertex.storage.stride) <= vertex.storage.budget->limits.dynamicVertexBytes &&
            (indices <= 0 || capacityBytes(indices, index.storage.stride) <= index.storage.budget->limits.dynamicIndexBytes),
            "Source dynamic mesh exceeds its vertex/index budget");
        if (vertices) vertex.storage.resize(capacityBytes(vertices, vertex.storage.stride));
        if (indices > 0) index.storage.resize(capacityBytes(indices, index.storage.stride));
        desc = {};
        if (vertices) require(vertex.Lock(vertices, false, desc), "Source mesh vertex lock failed");
        else {
            ComputeVertexDesc<false>(ignoredVertex.data(),GetVertexFormat(),desc);
            // Source permits callers to write dummy fields of an overridden stream.
            std::memset(&desc,0,offsetof(VertexDesc_t,m_ActualVertexSize));
        }
        desc.m_pIndices=&ignoredIndex; desc.m_nIndexSize=0;
        try {
            if (indices > 0) require(index.Lock(indices, false, desc), "Source mesh index lock failed");
        } catch (...) { if (vertices) vertex.storage.cancel(); throw; }
        lockVertex = vertices > 0; lockIndex = indices > 0; modification = false; meshLock = true;
    }
    void UnlockMesh(int vertices, int indices, MeshDesc_t& desc) override {
        require(meshLock && !modification && (lockIndex || indexOverride || indices == 0 || indices == -1), "Source mesh unlock does not match its lock");
        if (lockVertex) vertex.ValidateData(vertices, desc);
        if (lockIndex) index.ValidateData(indices, desc);
        if (lockVertex) vertex.storage.finish(vertices, false);
        if (lockIndex) index.storage.finish(indices, false);
        meshLock = lockVertex = lockIndex = false;
    }
    void ModifyBegin(int firstVertex, int vertices, int firstIndex, int indices, MeshDesc_t& desc) override {
        ModifyBeginEx(false, firstVertex, vertices, firstIndex, indices, desc);
    }
    void ModifyBeginEx(bool readOnly, int firstVertex, int vertices, int firstIndex, int indices, MeshDesc_t& desc) override {
        require(!meshLock && !vertex.storage.locked && !index.storage.locked && vertices >= 0 && indices >= 0 && (vertices || indices), "Invalid Source mesh modification");
        desc = {};
        if (vertices) { vertex.storage.modify(readOnly, firstVertex, vertices); vertex.describe(desc); }
        try { if (indices) index.ModifyBegin(readOnly, firstIndex, indices, desc); }
        catch (...) { if (vertices) vertex.storage.cancel(); throw; }
        lockVertex = vertices != 0; lockIndex = indices != 0; meshLock = modification = true;
    }
    void ModifyEnd(MeshDesc_t& desc) override {
        require(meshLock && modification, "Source mesh was not locked for modification");
        if (lockVertex) vertex.ValidateData(int(vertex.storage.lockCount), desc);
        if (lockIndex) index.ValidateData(int(index.storage.lockCount), desc);
        if (lockVertex) vertex.storage.finish(int(vertex.storage.lockCount), true);
        if (lockIndex) index.storage.finish(int(index.storage.lockCount), true);
        meshLock = modification = lockVertex = lockIndex = false;
    }
    void ValidateData(int vertices, int indices, const MeshDesc_t& desc) override {
        if (vertices) vertex.ValidateData(vertices, desc);
        if (indices) index.ValidateData(indices, desc);
    }
    void Spew(int vertices, int indices, const MeshDesc_t& desc) override { ValidateData(vertices, indices, desc); }
    void draw(int first, int count, const std::array<float,4>& modulation) {
        const auto& v=effectiveVertex().storage;
        const auto& indices=effectiveIndex().storage;
        require(!v.locked && !indices.locked, "Cannot draw a locked Source mesh");
        if (first == -1) { require(count == 0, "Whole-mesh draw cannot specify an index count"); first = 0; count = IndexCount(); }
        require(first >= 0 && count >= 0 && first <= IndexCount() && count <= IndexCount() - first, "Source mesh draw range is out of bounds");
        if (!count) return;
        require((topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP && count >= 3) ||
            (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && count % 3 == 0) ||
            (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST && count % 2 == 0), "Source mesh draw contains an incomplete primitive");
        if (validatedVertexRevision != v.revision || validatedIndexRevision != indices.revision ||
                validatedFirstIndex != first || validatedIndexCount != count) {
            // One overlay index buffer contains ranges for different static
            // vertex streams. Only this draw's range addresses this stream.
            for (int i = first; i < first + count; ++i) {
                uint16_t value; std::memcpy(&value, indices.data.data() + size_t(i) * 2, 2);
                require(value < VertexCount(), "Source mesh index references an uninitialized vertex");
            }
            validatedVertexRevision = v.revision; validatedIndexRevision = indices.revision;
            validatedFirstIndex = first; validatedIndexCount = count;
        }
        require(bool(dispatch), "No Source material draw sink is installed");
        SourceMeshDraw request {this, topology, uint32_t(first), uint32_t(count), modulation};
        request.colors=colorMesh; request.colorOffset=colorOffset;
        dispatch(request);
    }
    void Draw(int first, int count) override { draw(first, count, {1,1,1,1}); }
    void Draw(CPrimList* lists, int count) override {
        require(count >= 0 && (!count || lists), "Invalid Source primitive list");
        for (int i = 0; i < count; ++i) Draw(lists[i].m_FirstIndex, lists[i].m_NumIndices);
    }
    void DrawModulated(const Vector4D& value, int first, int count) override { draw(first, count, {value.x,value.y,value.z,value.w}); }
    void SetColorMesh(IMesh* mesh, int offset) override {
        require(offset>=0,"Invalid Source color-stream offset");
        colorMesh=mesh; colorOffset=uint32_t(offset);
    }
    void SetFlexMesh(IMesh* mesh, int offset) override { require(!mesh && !offset, "Source flex streams are not implemented"); }
    void DisableFlexMesh() override {}
    void MarkAsDrawn() override { require(!meshLock, "Cannot mark a locked mesh as drawn"); }
    unsigned ComputeMemoryUsed() override {
        return unsigned(vertex.storage.data.capacity() + vertex.storage.scratch.capacity() + index.storage.data.capacity() +
            index.storage.scratch.capacity() + vertex.storage.gpu.size() + index.storage.gpu.size());
    }
    void CopyToMeshBuilder(int, int, int, int, int, CMeshBuilder&) override { unsupported("temporary mesh copies"); }
    void* AccessRawHardwareDataStream(uint8, uint32, uint32, void*) override { unsupported("raw hardware mesh streams"); }
    ICachedPerFrameMeshData* GetCachedPerFrameMeshData() override { unsupported("cached mesh reconstruction"); }
    void ReconstructFromCachedPerFrameMeshData(ICachedPerFrameMeshData*) override { unsupported("cached mesh reconstruction"); }
};
} // namespace

struct SourceDevice::Impl final : IShaderDevice, IShaderDeviceMgr {
    Context& context;
    GraphicsDevice& graphics;
    FrameArena& dynamic;
    SourceShadow& shadow;
    SDL_Window* window;
    std::shared_ptr<Budget> budget;
    std::map<IVertexBuffer*, std::unique_ptr<VertexBuffer>> vertices;
    std::map<IIndexBuffer*, std::unique_ptr<IndexBuffer>> indices;
    std::map<IMesh*, std::unique_ptr<Mesh>> meshes;
    std::unordered_map<const IVertexBuffer*,Mesh*> vertexMeshes;
    std::unordered_map<const IIndexBuffer*,Mesh*> indexMeshes;
    std::unique_ptr<Mesh> dynamicMesh;
    std::map<int, IVertexBuffer*> dynamicVertices;
    IIndexBuffer* dynamicIndices = nullptr;
    struct ShaderRecord { SourceShaderStage stage; Shader shader; };
    std::map<uintptr_t, ShaderRecord> shaders;
    std::vector<UploadTicket> uploads;
    std::function<void(const SourceMeshDraw&)> sink, dispatch;
    SourceDeviceStatistics counts;
    bool active = false, suspended = false, connected = false, initialized = false, modeSet = false;
    Frame frame;
    VkExtent2D extent {};
    VkExtent2D requestedExtent {};
    bool resizeWithWindow = false;
    VkFormat backFormat = VK_FORMAT_UNDEFINED;
    std::array<float,4> outputGamma {1,1,0,0};
    mutable AspectRatioInfo_t aspect;
    std::vector<ShaderModeChangeCallbackFunc_t> modeCallbacks;
    std::vector<IShaderDeviceDependentObject*> dependents;
    IShaderAPI* api = nullptr;
    std::function<void(bool)> releaseAPI;
    static Impl* factoryOwner;

    Impl(Context& ctx, GraphicsDevice& gfx, FrameArena& arena, SourceShadow& shadows, SDL_Window* win, SourceDeviceLimits limits)
        : context(ctx), graphics(gfx), dynamic(arena), shadow(shadows), window(win), budget(std::make_shared<Budget>(Budget{limits})) {
        require(win && limits.maximumBufferBytes > 0 && limits.maximumBufferBytes <= 64 * 1024 * 1024 &&
            limits.maximumShadowBytes >= limits.maximumBufferBytes && limits.dynamicVertexBytes > 0 && limits.dynamicIndexBytes > 0 &&
            limits.dynamicVertexBytes <= limits.maximumBufferBytes && limits.dynamicIndexBytes <= limits.maximumBufferBytes,
            "Invalid Source device window or resource limits");
        dispatch = [&](const SourceMeshDraw& request) {
            require(bool(sink), "Source IMesh::Draw requires a material draw sink");
            if (active) detail::Access::validate(context, frame);
            sink(request); ++counts.meshDraws;
        };
    }
    ~Impl() { if (factoryOwner == this) factoryOwner = nullptr; }
    template<class Function> void eachStorage(Function function) {
        for (auto& [key, value] : vertices) function(value->storage, true);
        for (auto& [key, value] : indices) function(value->storage, false);
        for (auto& [key, value] : meshes) { function(value->vertex.storage, true); function(value->index.storage, false); }
        if (dynamicMesh) { function(dynamicMesh->vertex.storage, true); function(dynamicMesh->index.storage, false); }
    }
    VertexBuffer& vertexBuffer(const IVertexBuffer* pointer) const {
        if (dynamicMesh && static_cast<const IVertexBuffer*>(dynamicMesh.get()) == pointer) return dynamicMesh->effectiveVertex();
        const auto found = vertices.find(const_cast<IVertexBuffer*>(pointer));
        if (found != vertices.end()) return *found->second;
        const auto mesh=vertexMeshes.find(pointer);
        if(mesh!=vertexMeshes.end())return mesh->second->effectiveVertex();
        throw std::invalid_argument("Foreign or destroyed Source vertex buffer");
    }
    Storage& vertexStorage(const IVertexBuffer* pointer) const { return vertexBuffer(pointer).storage; }
    Storage& indexStorage(const IIndexBuffer* pointer) const {
        if (dynamicMesh && static_cast<const IIndexBuffer*>(dynamicMesh.get()) == pointer) return dynamicMesh->effectiveIndex().storage;
        const auto found = indices.find(const_cast<IIndexBuffer*>(pointer));
        if (found != indices.end()) return found->second->storage;
        const auto mesh=indexMeshes.find(pointer);
        if(mesh!=indexMeshes.end())return mesh->second->effectiveIndex().storage;
        throw std::invalid_argument("Foreign or destroyed Source index buffer");
    }
    void reapUploads() {
        uploads.erase(std::remove_if(uploads.begin(), uploads.end(), [](const auto& ticket) { return ticket.ready(); }), uploads.end());
    }
    void flush(const std::function<void(UploadBatch&)>& additional = {}) {
        require(!budget->lockedBuffers,"Source buffer is still locked at the frame/upload boundary");
        if(budget->pendingUploads.empty() && !additional)return;
        reapUploads();
        struct Pending { Storage* storage;uint64_t revision; };
        std::vector<Pending> dirty;
        for(auto it=budget->pendingUploads.begin();it!=budget->pendingUploads.end();) {
            auto* storage=*it;++counts.uploadInspections;
            if(storage->used && storage->gpuRevision!=storage->revision) {dirty.push_back({storage,storage->revision});++it;}
            else it=budget->pendingUploads.erase(it);
        }
        if (dirty.empty() && !additional) return;
        UploadBatch batch(context);
        std::vector<Buffer> replacements;
        for (const auto& upload : dirty) {
            auto* storage=upload.storage;
            const size_t size = (storage->used + 3) & ~size_t(3);
            std::vector<uint8_t> padded(size);
            std::memcpy(padded.data(), storage->data.data(), storage->used);
            auto buffer = context.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                (storage->vertex ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : VK_BUFFER_USAGE_INDEX_BUFFER_BIT), MemoryAccess::Device);
            batch.buffer({buffer,0,size}, padded.data(), {},
                {storage->vertex ? VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT : VK_ACCESS_INDEX_READ_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT});
            replacements.push_back(std::move(buffer));
        }
        if (additional) additional(batch);
        uploads.reserve(uploads.size() + 1);
        auto ticket = batch.submit();
        for (size_t i = 0; i < dirty.size(); ++i) {
            auto* storage=dirty[i].storage;storage->gpu=replacements[i];storage->gpuRevision=dirty[i].revision;
            if(storage->revision==dirty[i].revision)budget->pendingUploads.erase(storage);
        }
        uploads.push_back(std::move(ticket));
        ++counts.uploadSubmissions; counts.bufferUploads += dirty.size();
    }
    BufferSlice slice(Storage& storage, const Frame& value) {
        require(active && !suspended && !storage.locked && storage.used, "Source buffer is uninitialized, locked or outside a frame");
        const auto serial = detail::Access::serial(context, value);
        if (!storage.dynamic) {
            if (!storage.gpu || storage.gpuRevision != storage.revision) flush();
            return {storage.gpu,0,storage.used};
        }
        if (storage.cachedFrame != serial || storage.cachedRevision != storage.revision) {
            storage.cached = dynamic.write(value, storage.data.data(), storage.used);
            storage.cachedFrame = serial; storage.cachedRevision = storage.revision; ++counts.dynamicCopies;
        }
        return storage.cached;
    }
    bool begin(Frame& result) {
        require(!active, "Source frame already began");
        if (suspended) return false;
        reapUploads();
        flush();
        if (!context.beginFrame(result)) return false;
        frame = result; active = true; backFormat = result.colorFormat;
        if(resizeWithWindow)requestedExtent=result.extent;
        // Engine screen dimensions describe the rendered backbuffer. They can
        // change independently of the window/swapchain in scaled fullscreen.
        const auto rendered=requestedExtent.width?requestedExtent:result.extent;
        if (extent.width != rendered.width || extent.height != rendered.height) {
            extent = rendered;
            const auto callbacks = modeCallbacks;
            const auto objects = dependents;
            for (auto callback : callbacks) if (std::find(modeCallbacks.begin(), modeCallbacks.end(), callback) != modeCallbacks.end()) callback();
            for (auto* object : objects) if (std::find(dependents.begin(), dependents.end(), object) != dependents.end()) object->ScreenSizeChanged(int(extent.width), int(extent.height));
        }
        return true;
    }
    void ReleaseResources(bool managed) override {
        require(!active, "Cannot release Source resources during a frame");
        context.waitIdle(); suspended = true;
        if (releaseAPI) releaseAPI(managed);
        if (managed) eachStorage([](Storage& storage, bool) {
            if(!storage.dynamic && storage.used)storage.budget->pendingUploads.insert(&storage);
            storage.gpu = {}; storage.gpuRevision = 0; storage.cached = {}; storage.cachedFrame = 0;
        });
    }
    void ReacquireResources() override { require(!active, "Cannot reacquire Source resources during a frame"); suspended = false; }
    ImageFormat GetBackBufferFormat() const override {
        if (backFormat == VK_FORMAT_B8G8R8A8_UNORM || backFormat == VK_FORMAT_B8G8R8A8_SRGB) return IMAGE_FORMAT_BGRA8888;
        if (backFormat == VK_FORMAT_R8G8B8A8_UNORM || backFormat == VK_FORMAT_R8G8B8A8_SRGB) return IMAGE_FORMAT_RGBA8888;
        return IMAGE_FORMAT_BGRA8888;
    }
    void GetBackBufferDimensions(int& width, int& height) const override {
        if (requestedExtent.width) { width = int(requestedExtent.width); height = int(requestedExtent.height); }
        else if (extent.width) { width = int(extent.width); height = int(extent.height); }
        else require(platform::pixelSize(window, &width, &height), "Cannot query Source backbuffer extent");
    }
    const AspectRatioInfo_t& GetAspectRatioInfo() const override {
        int width, height; GetBackBufferDimensions(width, height);
        if (width > 0 && height > 0) {
            aspect.m_flFrameBufferAspectRatio = aspect.m_flPhysicalAspectRatio = float(width) / height;
            aspect.m_flFrameBuffertoPhysicalScalar = aspect.m_flPhysicalToFrameBufferScalar = 1;
            aspect.m_bIsWidescreen = aspect.m_flFrameBufferAspectRatio > 1.5f; aspect.m_bIsHidef = height >= 720; aspect.m_bInitialized = true;
        }
        return aspect;
    }
    int GetCurrentAdapter() const override { return 0; }
    bool IsUsingGraphics() const override { return initialized && modeSet && !suspended; }
    void SpewDriverInfo() const override { detail::Access::state(context)->log("VK_SOURCE_DEVICE: " + std::string(context.capabilities().properties.deviceName)); }
    int StencilBufferBits() const override { return 8; }
    bool IsAAEnabled() const override { return false; }
    void Present() override {
        if (api) api->OnPresent();
        if (!active) return; // Minimized/unavailable surfaces have no acquired image.
        context.endFrame(frame); active = false;
    }
    void GetWindowSize(int& width, int& height) const override { require(platform::windowSize(window, &width, &height), "Cannot query Source SDL window size"); }
    void SetHardwareGammaRamp(float gamma, float minimum, float maximum, float tvExponent, bool tv) override {
        require(std::isfinite(gamma) && gamma > 0 && std::isfinite(tvExponent) && tvExponent > 0 &&
            std::isfinite(minimum) && std::isfinite(maximum) && minimum >= 0 && maximum <= 255 && minimum <= maximum,
            "Invalid Source output gamma/range");
        // SDL3 deliberately has no desktop gamma ramp. Preserve Source's curve
        // in the final color-output shader without changing the user's desktop.
        outputGamma = {gamma / (tv ? tvExponent : 2.2f), tv ? (maximum-minimum)/255.f : 1.f,
            tv ? minimum/255.f : 0.f, 0};
    }
    bool AddView(void*) override { return false; }
    void RemoveView(void*) override { unsupported("multiple Source views"); }
    void SetView(void* view) override {
        // The engine clears its current view before app-system shutdown.
        require(!view || view == window, "Source device is attached to one SDL view");
    }
    IShaderBuffer* CompileShader(const char*, size_t, const char*) override { unsupported("runtime HLSL compilation; supply offline SPIR-V"); }
    uintptr_t createShader(IShaderBuffer* buffer, SourceShaderStage stage) {
        require(buffer, "Invalid Source shader buffer");
        const auto handle = nextSourceShader.fetch_add(1, std::memory_order_relaxed);
        require(handle < uintptr_t(0x7fffffff), "Source shader handle space exhausted");
        auto shader = graphics.createShader(buffer->GetBits(), buffer->GetSize(), stage == SourceShaderStage::Vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT);
        shaders.emplace(handle, ShaderRecord{stage, std::move(shader)});
        return handle;
    }
    void destroyShader(const void* handle, SourceShaderStage stage) {
        const auto found = shaders.find(reinterpret_cast<uintptr_t>(handle));
        require(found != shaders.end() && found->second.stage == stage, "Foreign, stale or wrong-stage Source shader handle");
        shaders.erase(found);
    }
    VertexShaderHandle_t CreateVertexShader(IShaderBuffer* buffer) override { return reinterpret_cast<VertexShaderHandle_t>(createShader(buffer, SourceShaderStage::Vertex)); }
    PixelShaderHandle_t CreatePixelShader(IShaderBuffer* buffer) override { return reinterpret_cast<PixelShaderHandle_t>(createShader(buffer, SourceShaderStage::Pixel)); }
    void DestroyVertexShader(VertexShaderHandle_t handle) override { destroyShader(handle, SourceShaderStage::Vertex); }
    void DestroyPixelShader(PixelShaderHandle_t handle) override { destroyShader(handle, SourceShaderStage::Pixel); }
    GeometryShaderHandle_t CreateGeometryShader(IShaderBuffer*) override { unsupported("geometry shaders"); }
    void DestroyGeometryShader(GeometryShaderHandle_t) override { unsupported("geometry shaders"); }
    IMesh* CreateStaticMesh(VertexFormat_t format, const char*, IMaterial* material, VertexStreamSpec_t* streams) override {
        require(!streams, "Multiple-stream mesh construction is not connected yet");
        if (!format && material) format = material->GetVertexFormat();
        auto mesh = std::make_unique<Mesh>(budget, format, dispatch);
        auto* pointer = mesh.get(); meshes.emplace(pointer, std::move(mesh));
        try {
            vertexMeshes.emplace(static_cast<IVertexBuffer*>(pointer),pointer);
            indexMeshes.emplace(static_cast<IIndexBuffer*>(pointer),pointer);
        } catch(...) {
            vertexMeshes.erase(static_cast<IVertexBuffer*>(pointer));indexMeshes.erase(static_cast<IIndexBuffer*>(pointer));
            meshes.erase(pointer);throw;
        }
        return pointer;
    }
    void DestroyStaticMesh(IMesh* pointer) override {
        const auto found = meshes.find(pointer);
        require(found != meshes.end() && !found->second->vertex.storage.locked && !found->second->index.storage.locked, "Foreign or locked Source mesh");
        auto detach=[&](Mesh& mesh) {
            if (mesh.vertexOverride==&found->second->vertex) mesh.vertexOverride=nullptr;
            if (mesh.indexOverride==&found->second->index) mesh.indexOverride=nullptr;
            if (mesh.colorMesh==pointer) mesh.colorMesh=nullptr;
            mesh.validatedVertexRevision=mesh.validatedIndexRevision=0;
        };
        if (dynamicMesh) detach(*dynamicMesh);
        for (auto& [key,mesh]:meshes) detach(*mesh);
        vertexMeshes.erase(static_cast<IVertexBuffer*>(pointer));indexMeshes.erase(static_cast<IIndexBuffer*>(pointer));
        meshes.erase(found);
    }
    IVertexBuffer* CreateVertexBuffer(ShaderBufferType_t type, VertexFormat_t format, int count, const char*) override {
        require(count > 0, "Source vertex buffer has no capacity");
        auto buffer = std::make_unique<VertexBuffer>(budget, dynamicType(type), format, count);
        auto* pointer = buffer.get(); vertices.emplace(pointer, std::move(buffer)); return pointer;
    }
    IIndexBuffer* CreateIndexBuffer(ShaderBufferType_t type, MaterialIndexFormat_t format, int count, const char*) override {
        require(count > 0, "Source index buffer has no capacity");
        auto buffer = std::make_unique<IndexBuffer>(budget, dynamicType(type), format, count);
        auto* pointer = buffer.get(); indices.emplace(pointer, std::move(buffer)); return pointer;
    }
    void DestroyVertexBuffer(IVertexBuffer* pointer) override {
        const auto found = vertices.find(pointer);
        require(found != vertices.end() && !found->second->storage.locked, "Foreign or locked Source vertex buffer");
        for (const auto& [stream, cached] : dynamicVertices) require(cached != pointer, "Device-owned dynamic vertex buffers cannot be destroyed by callers");
        vertices.erase(found);
    }
    void DestroyIndexBuffer(IIndexBuffer* pointer) override {
        const auto found = indices.find(pointer);
        require(found != indices.end() && !found->second->storage.locked && pointer != dynamicIndices, "Foreign, locked or device-owned Source index buffer");
        indices.erase(found);
    }
    IVertexBuffer* GetDynamicVertexBuffer(int stream, VertexFormat_t format, bool) override {
        require(stream >= 0 && uint32_t(stream) < context.capabilities().properties.limits.maxVertexInputBindings, "Source dynamic stream is out of range");
        sourceVertexLayout(format);
        auto found = dynamicVertices.find(stream);
        if (found == dynamicVertices.end()) {
            auto* buffer = CreateVertexBuffer(SHADER_BUFFER_TYPE_DYNAMIC, 0, int(budget->limits.dynamicVertexBytes), "SourceDynamic");
            found = dynamicVertices.emplace(stream, buffer).first;
        }
        return found->second; // CVertexBuilder performs BeginCastBuffer/EndCastBuffer.
    }
    IIndexBuffer* GetDynamicIndexBuffer() override {
        if (!dynamicIndices) dynamicIndices = CreateIndexBuffer(SHADER_BUFFER_TYPE_DYNAMIC, MATERIAL_INDEX_FORMAT_UNKNOWN, int(budget->limits.dynamicIndexBytes), "SourceDynamic");
        return dynamicIndices;
    }
    void EnableNonInteractiveMode(MaterialNonInteractiveMode_t mode, ShaderNonInteractiveInfo_t*) override { require(mode == MATERIAL_NON_INTERACTIVE_MODE_NONE, "Source noninteractive rendering is not implemented"); }
    void RefreshFrontBufferNonInteractive() override {
        // Linux calls this while loading even with NON_INTERACTIVE_MODE_NONE.
        // No asynchronous console loading renderer is running in that mode.
    }
    void HandleThreadEvent(uint32) override { unsupported("cross-thread device events"); }

    bool Connect(CreateInterfaceFn factory) override { if (!factory || connected) return false; connected = true; return true; }
    void Disconnect() override { require(!initialized && !active, "Shutdown the Source device manager before disconnecting"); connected = false; }
    void* QueryInterface(const char* name) override {
        if (!name) return nullptr;
        if (!std::strcmp(name, SHADER_DEVICE_INTERFACE_VERSION)) return static_cast<IShaderDevice*>(this);
        if (!std::strcmp(name, SHADER_DEVICE_MGR_INTERFACE_VERSION)) return static_cast<IShaderDeviceMgr*>(this);
        if (!std::strcmp(name, SHADERSHADOW_INTERFACE_VERSION)) return &shadow.interface();
        if (!std::strcmp(name, SHADERAPI_INTERFACE_VERSION)) return api;
        if (!std::strcmp(name, SHADERDYNAMIC_INTERFACE_VERSION)) return static_cast<IShaderDynamicAPI*>(api);
        return nullptr;
    }
    InitReturnVal_t Init() override { if (!connected) return INIT_FAILED; initialized = true; suspended = false; return INIT_OK; }
    void Shutdown() override { ReleaseResources(true); initialized = modeSet = false; }
    int GetAdapterCount() const override { return 1; }
    void GetAdapterInfo(int adapter, MaterialAdapterInfo_t& info) const override {
        require(adapter == 0, "Only the attached Vulkan adapter is exposed");
        const auto& properties = context.capabilities().properties;
        info = {}; std::snprintf(info.m_pDriverName, sizeof(info.m_pDriverName), "Source Vulkan: %s", properties.deviceName);
        info.m_VendorID = properties.vendorID; info.m_DeviceID = properties.deviceID;
        info.m_nDriverVersionLow = properties.driverVersion;
        info.m_nDXSupportLevel = info.m_nMinDXSupportLevel = info.m_nMaxDXSupportLevel = 95; // Legacy SM3 material tier, not a D3D device.
    }
    bool GetRecommendedConfigurationInfo(int, int, KeyValues*) override { return false; }
    bool GetRecommendedVideoConfig(int, KeyValues*) override { return false; }
    void mode(ShaderDisplayMode_t* info, const platform::DisplayMode& value) const {
        require(info, "Cannot query Source display mode");
        *info = ShaderDisplayMode_t(); info->m_nWidth = value.width; info->m_nHeight = value.height; info->m_Format = IMAGE_FORMAT_BGRA8888;
        info->m_nRefreshRateNumerator = value.refreshNumerator; info->m_nRefreshRateDenominator = value.refreshDenominator;
    }
    int GetModeCount(int adapter) const override {
        require(adapter == 0, "Invalid attached adapter"); return int(platform::displayModes(window).size());
    }
    void GetModeInfo(ShaderDisplayMode_t* info, int adapter, int index) const override {
        require(adapter == 0 && index >= 0, "Invalid attached adapter/display mode");
        const auto modes = platform::displayModes(window);
        require(size_t(index) < modes.size(), "Source display mode is out of range"); mode(info, modes[size_t(index)]);
    }
    void GetCurrentModeInfo(ShaderDisplayMode_t* info, int adapter) const override {
        require(adapter == 0, "Invalid attached adapter"); mode(info, platform::currentMode(window));
    }
    bool SetAdapter(int adapter, int flags) override {
        return adapter == 0 && !(flags & ~MATERIAL_INIT_ALLOCATE_FULLSCREEN_TEXTURE);
    }
    static void* factory(const char* name, int* status) {
        void* result = factoryOwner ? factoryOwner->QueryInterface(name) : nullptr;
        if (status) *status = result ? IFACE_OK : IFACE_FAILED;
        return result;
    }
    CreateInterfaceFn SetMode(void* view, int adapter, const ShaderDeviceInfo_t& mode) override {
        if (!initialized || active || view != window || adapter != 0 || (factoryOwner && factoryOwner != this) ||
            mode.m_nVersion != SHADER_DEVICE_INFO_VERSION || mode.m_DisplayMode.m_nVersion != SHADER_DISPLAY_MODE_VERSION ||
            mode.m_nAASamples > 1 || mode.m_nAASamples < 0 ||
            mode.m_nAAQuality || mode.m_bLimitWindowedSize || mode.m_bUsingMultipleWindows || mode.m_bScaleToOutputResolution ||
            (mode.m_nDXLevel && mode.m_nDXLevel != 95) || mode.m_DisplayMode.m_nWidth < 0 || mode.m_DisplayMode.m_nHeight < 0) return nullptr;
        int width = mode.m_DisplayMode.m_nWidth, height = mode.m_DisplayMode.m_nHeight;
        if (!width || !height) {
            const auto desktop = platform::currentMode(window, true);
            width = desktop.width; height = desktop.height;
        }
        if (!platform::resize(window, width, height, mode.m_bResizing)) return nullptr;
        requestedExtent = {uint32_t(width),uint32_t(height)};
        resizeWithWindow=mode.m_bResizing;
        context.requestResize(); factoryOwner = this; modeSet = true; return &factory;
    }
    void AddModeChangeCallback(ShaderModeChangeCallbackFunc_t callback) override {
        require(callback, "Null Source mode callback");
        if (std::find(modeCallbacks.begin(), modeCallbacks.end(), callback) == modeCallbacks.end()) modeCallbacks.push_back(callback);
    }
    void RemoveModeChangeCallback(ShaderModeChangeCallbackFunc_t callback) override { modeCallbacks.erase(std::remove(modeCallbacks.begin(), modeCallbacks.end(), callback), modeCallbacks.end()); }
    void AddDeviceDependentObject(IShaderDeviceDependentObject* object) override {
        require(object, "Null Source device-dependent object");
        if (std::find(dependents.begin(), dependents.end(), object) == dependents.end()) dependents.push_back(object);
    }
    void RemoveDeviceDependentObject(IShaderDeviceDependentObject* object) override { dependents.erase(std::remove(dependents.begin(), dependents.end(), object), dependents.end()); }
};
SourceDevice::Impl* SourceDevice::Impl::factoryOwner = nullptr;

SourceDevice::SourceDevice(Context& context, GraphicsDevice& graphics, FrameArena& dynamic, SourceShadow& shadow, SDL_Window* window, SourceDeviceLimits limits)
    : impl_(std::make_unique<Impl>(context, graphics, dynamic, shadow, window, limits)) {}
SourceDevice::~SourceDevice() = default;
int SourceDevice::maximumVertices(uint64_t format) const { return int(std::min<size_t>(65535,impl_->budget->limits.dynamicVertexBytes/sourceVertexLayout(format).stride)); }
int SourceDevice::maximumIndices() const { return int(impl_->budget->limits.dynamicIndexBytes/2); }
int SourceDevice::dynamicVertexBytes() const { return int(impl_->budget->limits.dynamicVertexBytes); }
std::array<float,4> SourceDevice::outputGamma() const { return impl_->outputGamma; }
IShaderDevice& SourceDevice::interface() { return *impl_; }
IShaderDeviceMgr& SourceDevice::manager() { return *impl_; }
void* SourceDevice::queryInterface(const char* name, int* status) {
    void* result = impl_->QueryInterface(name);
    if (status) *status = result ? IFACE_OK : IFACE_FAILED;
    return result;
}
void SourceDevice::flushUploads(const std::function<void(UploadBatch&)>& additional) { impl_->flush(additional); }
bool SourceDevice::beginFrame(Frame& frame) { return impl_->begin(frame); }
bool SourceDevice::frameActive() const { return impl_->active; }
IMesh* SourceDevice::dynamicMesh(uint64_t format, IMesh* vertices, IMesh* indices) {
    auto* v=vertices?dynamic_cast<Mesh*>(vertices):nullptr;
    auto* i=indices?dynamic_cast<Mesh*>(indices):nullptr;
    require((!vertices || v) && (!indices || i),"Foreign Source mesh override");
    if (v) format=v->GetVertexFormat();
    sourceVertexLayout(format);
    auto& mesh = impl_->dynamicMesh;
    require(!mesh || (!mesh->vertex.storage.locked && !mesh->index.storage.locked), "Source dynamic mesh is still locked");
    auto* vb=v?&v->effectiveVertex():nullptr;
    auto* ib=i?&i->effectiveIndex():nullptr;
    if (!mesh) mesh = std::make_unique<Mesh>(impl_->budget, format, impl_->dispatch, true);
    else if (mesh->vertex.format != format) {
        // Overlay batches first fill only indices, then reuse that IMesh as
        // the index override while binding static vertices of another format.
        // Keep the mesh and its index storage alive across vertex casts.
        require(vb!=&mesh->vertex,"Cannot reinterpret a dynamic mesh's own vertex data");
        mesh->vertex.format=format;
        auto& storage=mesh->vertex.storage;
        storage.stride=sourceVertexLayout(format).stride;
        storage.used=storage.cursor=0;
        ++storage.revision;
    }
    mesh->vertexOverride=vb==&mesh->vertex?nullptr:vb;
    mesh->indexOverride=ib==&mesh->index?nullptr:ib;
    mesh->colorMesh=nullptr; mesh->colorOffset=0;
    mesh->validatedVertexRevision=mesh->validatedIndexRevision=0;
    return mesh.get();
}
void SourceDevice::attachAPI(IShaderAPI* api, std::function<void(bool)> release) {
    require(api && !impl_->api && !impl_->active, "Source device already has an API or an active frame");
    impl_->api = api; impl_->releaseAPI = std::move(release);
}
void SourceDevice::detachAPI(IShaderAPI* api) {
    require(api && impl_->api == api, "Source API does not belong to this device");
    impl_->api = nullptr; impl_->releaseAPI = {}; impl_->sink = {};
}
BufferSlice SourceDevice::vertexSlice(const IVertexBuffer* buffer, const Frame& frame) { return impl_->slice(impl_->vertexStorage(buffer), frame); }
void SourceDevice::traceVertexData(const IVertexBuffer* buffer, uint32_t byteOffset) const {
    const auto& storage=impl_->vertexStorage(buffer);
    const size_t bytes=byteOffset<storage.used?std::min<size_t>(storage.used-byteOffset,256*storage.stride):0;
    unsigned sum=0,maximum=0;
    for(size_t i=0;i<bytes;++i) {sum+=storage.data[byteOffset+i];maximum=std::max<unsigned>(maximum,storage.data[byteOffset+i]);}
    std::fprintf(stderr,"VK_VERTEX_TRACE: buffer=%p stride=%zu used=%zu revision=%llu gpu=%llu offset=%u mean=%g max=%u first=",
        static_cast<const void*>(buffer),storage.stride,storage.used,static_cast<unsigned long long>(storage.revision),
        static_cast<unsigned long long>(storage.gpuRevision),byteOffset,bytes?double(sum)/bytes:0,maximum);
    for(size_t i=0;i<std::min<size_t>(12,bytes);++i)std::fprintf(stderr,"%02x",storage.data[byteOffset+i]);
    if(bytes>=12 && (vertexFormat(buffer)&VERTEX_POSITION)) {
        float position[3];std::memcpy(position,storage.data.data()+byteOffset,sizeof(position));
        std::fprintf(stderr," position=%g,%g,%g",position[0],position[1],position[2]);
    }
    std::fprintf(stderr,"\n");
}
uint64_t SourceDevice::vertexFormat(const IVertexBuffer* buffer) const {
    return impl_->vertexBuffer(buffer).format;
}
BufferSlice SourceDevice::indexSlice(const IIndexBuffer* buffer, const Frame& frame) { return impl_->slice(impl_->indexStorage(buffer), frame); }
VkIndexType SourceDevice::indexType(const IIndexBuffer* buffer) const { return impl_->indexStorage(buffer).stride == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32; }
void SourceDevice::validateIndexedDraw(const IVertexBuffer* vertex, size_t vertexOffset, uint32_t firstVertex,
        uint32_t vertexCount, const IIndexBuffer* index, size_t indexOffset, uint32_t firstIndex, uint32_t indexCount) const {
    const auto& v = impl_->vertexStorage(vertex);
    const auto& i = impl_->indexStorage(index);
    require(!v.locked && !i.locked && v.stride && i.stride && vertexCount && indexCount &&
        vertexOffset <= v.used && uint64_t(firstVertex) + vertexCount <= (v.used - vertexOffset) / v.stride &&
        indexOffset % i.stride == 0 && indexOffset <= i.used &&
        uint64_t(firstIndex) + indexCount <= (i.used - indexOffset) / i.stride, "Source indexed draw exceeds initialized buffer data");
    for (uint32_t n = 0; n < indexCount; ++n) {
        const auto* data = i.data.data() + indexOffset + (size_t(firstIndex) + n) * i.stride;
        uint32_t value = 0;
        if (i.stride == 2) { uint16_t word; std::memcpy(&word, data, 2); value = word; }
        else std::memcpy(&value, data, 4);
        require(value < vertexCount, "Source index references a vertex outside the bound range");
    }
}
Shader SourceDevice::shader(const void* handle, SourceShaderStage stage) const {
    const auto found = impl_->shaders.find(reinterpret_cast<uintptr_t>(handle));
    require(found != impl_->shaders.end() && found->second.stage == stage, "Foreign, stale or wrong-stage Source shader handle");
    return found->second.shader;
}
void SourceDevice::setDrawSink(std::function<void(const SourceMeshDraw&)> sink) { impl_->sink = std::move(sink); }
SourceDeviceStatistics SourceDevice::statistics() const {
    auto result = impl_->counts;
    result.shadowBytes = impl_->budget->bytes; result.vertexBuffers = impl_->vertices.size(); result.indexBuffers = impl_->indices.size();
    result.meshes = impl_->meshes.size() + bool(impl_->dynamicMesh); result.shaders = impl_->shaders.size(); return result;
}
} // namespace sourcevk
