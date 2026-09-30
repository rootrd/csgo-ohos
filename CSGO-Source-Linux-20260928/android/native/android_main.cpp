#include "source_assets.h"
#include "probe/vulkan_probe.h"
#include "ohos_compat.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <d3d9.h>
#include <wsi/native_sdl3.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <unistd.h>
#include <vector>

int runSourceEngine(int argc, char **argv, const char *resourceRoot, const char *errorPath);

namespace {
#ifdef __OHOS__
// OHOS 沙箱的真实路径因设备/版本而异（haps/entry 视图 vs 物理路径），且
// CSGO_OHOS_GAME_ROOT 依赖 ArkTS 侧 setenv 的时序，不能盲信。按优先级收集
// 候选根，取第一个能读到随包资源哨兵（mobile_ui/manifest.txt 或
// csgo/gameinfo.txt）的；全不达标时退回首选候选并把失败留给显式报错。
std::string& OhosResourceRoot()
{
    static std::string cached;
    if (cached.empty())
    {
        const char* fromEnv = getenv("CSGO_OHOS_GAME_ROOT");
        const char* sdlPath = SDL_GetOpenHarmonyInternalStoragePath();
        std::vector<std::string> candidates;
        if (fromEnv && *fromEnv) candidates.emplace_back(fromEnv);
        if (sdlPath && *sdlPath)
        {
            candidates.emplace_back(std::string(sdlPath) + "/csgo");
            // SDL 返回应用级目录（…/el2/base/files），ArkTS filesDir 是 HAP 级
            // （…/el2/base/haps/entry/files）：同一存储的另一视图，两处都探测。
            std::string hapView(sdlPath);
            const std::string marker = "/el2/base/files";
            const size_t at = hapView.find(marker);
            if (at != std::string::npos)
                hapView.replace(at, marker.size(), "/el2/base/haps/entry/files");
            candidates.push_back(hapView + "/csgo");
        }
        candidates.emplace_back("/data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo");
        auto looksLikeGameRoot = [](const std::string& root) {
            std::ifstream probe(root + "/mobile_ui/manifest.txt", std::ios::binary);
            if (probe.good()) return true;
            probe.open(root + "/csgo/gameinfo.txt", std::ios::binary);
            return probe.good();
        };
        for (const auto& candidate : candidates)
            if (looksLikeGameRoot(candidate)) { cached = candidate; break; }
        if (cached.empty() && !candidates.empty()) cached = candidates.front();
    }
    return cached;
}
#else
constexpr const char* kFallbackResourceRoot = "/storage/emulated/0/Games/CSGO";
#endif

const char* ResourceRoot() {
#ifdef __OHOS__
    return OhosResourceRoot().c_str();
#else
    return kFallbackResourceRoot;
#endif
}
FILE* logFile = nullptr;
std::string errorPath;

void log(const char* format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
#ifdef __OHOS__
    ohos_log(format, args);
#else
    __android_log_vprint(ANDROID_LOG_INFO, "CSGO", format, args);
#endif
    if (logFile) {
        flockfile(logFile);
        vfprintf(logFile, format, copy);
        fputc('\n', logFile);
        funlockfile(logFile);
    }
    va_end(copy);
    va_end(args);
}

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char message[192];
        snprintf(message, sizeof(message), "%s failed: 0x%08x", operation, unsigned(result));
        throw std::runtime_error(message);
    }
}

struct Lifecycle {
    std::atomic<bool> paused {false}, quit {false};
    std::atomic<unsigned> generation {0};
};

bool SDLCALL watchLifecycle(void* userdata, SDL_Event* event) {
    auto& state = *static_cast<Lifecycle*>(userdata);
    // SDL3 dispatches these events only to watchers, never to SDL_PollEvent.
    // Leave GPU work on the main thread; callbacks may run on a platform thread.
    switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        state.paused = true;
        log("LIFECYCLE: background");
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        ++state.generation;
        state.paused = false;
        log("LIFECYCLE: foreground");
        break;
    case SDL_EVENT_TERMINATING:
        state.quit = true;
        break;
    default: break;
    }
    return true;
}

template<typename T> struct Com {
    T* value = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() { if (value) { value->Release(); value = nullptr; } }
    T* operator->() const { return value; }
    T** put() { reset(); return &value; }
};

// Shader Model 2 bytecode assembled from the public D3D9 token definitions.
// vs: position = mul(matrix, v0); uv = v1. ps: texture(s0, uv) * tint.
constexpr DWORD reg(DWORD type, DWORD index) {
    return 0x80000000u | ((type & 7) << D3DSP_REGTYPE_SHIFT)
        | ((type & 0x18) << D3DSP_REGTYPE_SHIFT2) | index;
}
constexpr DWORD dst(DWORD type, DWORD index) { return reg(type, index) | D3DSP_WRITEMASK_ALL; }
constexpr DWORD src(DWORD type, DWORD index) { return reg(type, index) | D3DVS_NOSWIZZLE; }
constexpr DWORD op(DWORD opcode, DWORD words) { return opcode | (words << 24); }
constexpr DWORD VertexShader[] = {
    D3DVS_VERSION(2, 0),
    op(D3DSIO_DCL, 2), 0x80000000u | D3DDECLUSAGE_POSITION, dst(D3DSPR_INPUT, 0),
    op(D3DSIO_DCL, 2), 0x80000000u | D3DDECLUSAGE_TEXCOORD, dst(D3DSPR_INPUT, 1),
    op(D3DSIO_M4x4, 3), dst(D3DSPR_RASTOUT, 0), src(D3DSPR_INPUT, 0), src(D3DSPR_CONST, 0),
    op(D3DSIO_MOV, 2), dst(D3DSPR_TEXCRDOUT, 0), src(D3DSPR_INPUT, 1), D3DSIO_END
};
constexpr DWORD PixelShader[] = {
    D3DPS_VERSION(2, 0),
    op(D3DSIO_DCL, 2), 0x80000000u, dst(D3DSPR_TEXTURE, 0),
    op(D3DSIO_DCL, 2), 0x80000000u | D3DSTT_2D, dst(D3DSPR_SAMPLER, 0),
    op(D3DSIO_TEX, 3), dst(D3DSPR_TEMP, 0), src(D3DSPR_TEXTURE, 0), src(D3DSPR_SAMPLER, 0),
    op(D3DSIO_MUL, 3), dst(D3DSPR_COLOROUT, 0), src(D3DSPR_TEMP, 0), src(D3DSPR_CONST, 0), D3DSIO_END
};
constexpr float Identity[] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
struct Vertex { float x, y, z, u, v; };

D3DFORMAT textureFormat(const SourceTexture& texture) {
    // The asset payload describes its encoding and block geometry independently
    // of D3D9/VTF enums, so future ASTC assets can use the same upload boundary.
    switch (texture.encoding) {
    case TextureEncoding::BC1: return D3DFMT_DXT1;
    case TextureEncoding::BC2: return D3DFMT_DXT3;
    case TextureEncoding::BC3: return D3DFMT_DXT5;
    case TextureEncoding::ASTC:
        throw std::runtime_error("ASTC assets require the future D3D9 ASTC backend; no decode fallback is enabled");
    }
    throw std::runtime_error("Unknown texture encoding");
}

class GraphicsProbe {
    // Members are destroyed in reverse order; the device must outlive its resources.
    Com<IDirect3D9> d3d;
    Com<IDirect3DDevice9> device;
    Com<IDirect3DVertexShader9> vertexShader;
    Com<IDirect3DPixelShader9> pixelShader;
    Com<IDirect3DVertexDeclaration9> declaration;
    Com<IDirect3DTexture9> texture, white;
    unsigned width, height;

    void tint(float r, float g, float b, float a = 1) {
        const float color[] = {r, g, b, a};
        check(device->SetPixelShaderConstantF(0, color, 1), "SetPixelShaderConstantF");
    }

    void rectangle(float left, float top, float right, float bottom, float depth) {
        const Vertex vertices[] = {
            {left,top,depth,0,0}, {right,top,depth,1,0}, {right,bottom,depth,1,1},
            {left,top,depth,0,0}, {right,bottom,depth,1,1}, {left,bottom,depth,0,1}
        };
        check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, vertices, sizeof(Vertex)), "DrawPrimitiveUP");
    }

    void textureUpload(Com<IDirect3DTexture9>& target, unsigned w, unsigned h, D3DFORMAT format,
                       const void* pixels, unsigned rowBytes, unsigned rows) {
        check(device->CreateTexture(w, h, 1, 0, format, D3DPOOL_MANAGED, target.put(), nullptr), "CreateTexture");
        D3DLOCKED_RECT locked {};
        check(target->LockRect(0, &locked, nullptr, 0), "LockRect(texture)");
        if (locked.Pitch < int(rowBytes)) {
            target->UnlockRect(0);
            throw std::runtime_error("Invalid texture upload pitch");
        }
        for (unsigned row = 0; row < rows; ++row)
            memcpy(static_cast<char*>(locked.pBits) + row * locked.Pitch,
                   static_cast<const char*>(pixels) + row * rowBytes, rowBytes);
        check(target->UnlockRect(0), "UnlockRect(texture)");
    }

    void validatePixels() {
        Com<IDirect3DSurface9> originalColor, originalDepth, color, depth, readback;
        check(device->GetRenderTarget(0, originalColor.put()), "GetRenderTarget");
        check(device->GetDepthStencilSurface(originalDepth.put()), "GetDepthStencilSurface");
        check(device->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, false,
                                        color.put(), nullptr), "CreateRenderTarget");
        check(device->CreateDepthStencilSurface(64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, true,
                                               depth.put(), nullptr), "CreateDepthStencilSurface");
        check(device->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                 readback.put(), nullptr), "Create readback surface");
        check(device->SetRenderTarget(0, color.value), "SetRenderTarget");
        check(device->SetDepthStencilSurface(depth.value), "SetDepthStencilSurface");
        const D3DVIEWPORT9 viewport {0, 0, 64, 64, 0, 1};
        check(device->SetViewport(&viewport), "SetViewport");
        check(device->SetTexture(0, white.value), "SetTexture(BC1)");
        check(device->SetVertexShaderConstantF(0, Identity, 4), "SetVertexShaderConstantF");
        check(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff102030, 1, 0), "Clear");
        check(device->BeginScene(), "BeginScene");
        tint(1, 0, 0); rectangle(-0.75f, 0.75f, 0.75f, -0.75f, 0.5f);
        // The later green rectangle is behind the red rectangle and must fail depth testing.
        tint(0, 1, 0); rectangle(-0.75f, 0.75f, 0.75f, -0.75f, 0.75f);
        check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, true), "Enable alpha blend");
        tint(0, 0, 1, 0.5f); rectangle(-0.75f, 0.75f, 0, -0.75f, 0.25f);
        check(device->EndScene(), "EndScene");
        check(device->GetRenderTargetData(color.value, readback.value), "GetRenderTargetData");
        D3DLOCKED_RECT locked {};
        check(readback->LockRect(&locked, nullptr, D3DLOCK_READONLY), "LockRect(readback)");
        auto pixel = [&locked](int x, int y) {
            uint32_t result;
            memcpy(&result, static_cast<const char*>(locked.pBits) + y * locked.Pitch + x * 4, 4);
            return result;
        };
        const uint32_t background = pixel(2, 2), opaque = pixel(48, 32), blended = pixel(16, 32);
        readback->UnlockRect();
        auto close = [](uint32_t actual, uint32_t expected) {
            for (int shift : {0, 8, 16})
                if (std::abs(int((actual >> shift) & 255) - int((expected >> shift) & 255)) > 2) return false;
            return true;
        };
        log("GPU pixels: clear=%08x depth=%08x blend=%08x", background, opaque, blended);
        if (!close(background, 0x102030) || !close(opaque, 0xff0000) || !close(blended, 0x800080))
            throw std::runtime_error("GPU pixel validation failed (shader / BC1 texture / depth / alpha blend)");
        check(device->SetRenderTarget(0, originalColor.value), "Restore render target");
        check(device->SetDepthStencilSurface(originalDepth.value), "Restore depth surface");
        const D3DVIEWPORT9 restored {0, 0, width, height, 0, 1};
        check(device->SetViewport(&restored), "Restore viewport");
        check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, false), "Disable alpha blend");
        log("GPU_SELF_TEST_PASS: SM2 vertex/pixel shaders, BC1 sampling, depth rejection, alpha blending, readback");
    }

public:
    GraphicsProbe(SDL_Window* window, unsigned w, unsigned h, const SourceTexture& asset)
        : width(w), height(h) {
        d3d.value = Direct3DCreate9(D3D_SDK_VERSION);
        if (!d3d.value) throw std::runtime_error("DXVK Direct3DCreate9 failed; see DXVK log");
        D3DADAPTER_IDENTIFIER9 adapter {};
        check(d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapter), "GetAdapterIdentifier");
        log("D3D9 compatibility adapter: %s; vendor=%04x device=%04x", adapter.Description, adapter.VendorId, adapter.DeviceId);
        for (auto format : {D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5}) {
            if (FAILED(d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                                             0, D3DRTYPE_TEXTURE, format)))
                throw std::runtime_error("GPU/driver does not support the required BC1/BC2/BC3 texture formats");
        }
        D3DPRESENT_PARAMETERS parameters {};
        parameters.BackBufferWidth = width;
        parameters.BackBufferHeight = height;
        parameters.BackBufferFormat = D3DFMT_X8R8G8B8;
        parameters.BackBufferCount = 1;
        parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
        parameters.hDeviceWindow = dxvk::wsi::toHwnd(window);
        parameters.Windowed = true;
        parameters.EnableAutoDepthStencil = true;
        parameters.AutoDepthStencilFormat = D3DFMT_D24S8;
        parameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
        check(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, parameters.hDeviceWindow,
                               D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                               &parameters, device.put()), "CreateDevice");
        const D3DVERTEXELEMENT9 elements[] = {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()
        };
        check(device->CreateVertexDeclaration(elements, declaration.put()), "CreateVertexDeclaration");
        check(device->CreateVertexShader(VertexShader, vertexShader.put()), "CreateVertexShader");
        check(device->CreatePixelShader(PixelShader, pixelShader.put()), "CreatePixelShader");
        check(device->SetVertexDeclaration(declaration.value), "SetVertexDeclaration");
        check(device->SetVertexShader(vertexShader.value), "SetVertexShader");
        check(device->SetPixelShader(pixelShader.value), "SetPixelShader");
        check(device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), "Set cull mode");
        check(device->SetRenderState(D3DRS_ZENABLE, true), "Enable depth");
        check(device->SetRenderState(D3DRS_ZWRITEENABLE, true), "Enable depth writes");
        check(device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL), "Set depth function");
        check(device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA), "Set source blend");
        check(device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA), "Set destination blend");
        check(device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT), "Set min filter");
        check(device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT), "Set mag filter");
        const uint8_t whiteBlock[8] = {0xff, 0xff, 0, 0, 0, 0, 0, 0};
        textureUpload(white, 4, 4, D3DFMT_DXT1, whiteBlock, 8, 1);
        textureUpload(texture, asset.width, asset.height, textureFormat(asset),
                      asset.pixels.data(), asset.rowBytes, asset.rows);
        validatePixels();
        // Prewarm every currently used state combination with a real draw,
        // then wait for completion before the first visible frame. Creating a
        // shader object alone does not compile its Vulkan graphics pipeline.
        const auto warmupStart = SDL_GetTicks();
        log("SHADER_WARMUP_BEGIN");
        check(render(0, false), "Draw startup pipeline variants");
        Com<IDirect3DQuery9> completed;
        check(device->CreateQuery(D3DQUERYTYPE_EVENT, completed.put()), "Create warmup fence");
        check(completed->Issue(D3DISSUE_END), "Issue warmup fence");
        HRESULT ready;
        while ((ready = completed->GetData(nullptr, 0, D3DGETDATA_FLUSH)) == S_FALSE) {
            if (SDL_GetTicks() - warmupStart > 30000) throw std::runtime_error("Shader warmup timed out");
            SDL_Delay(1);
        }
        check(ready, "Wait for startup pipelines");
        log("SHADER_WARMUP_READY: %llu ms; asynchronous pipelines enabled", static_cast<unsigned long long>(SDL_GetTicks() - warmupStart));
    }

    HRESULT render(float seconds, bool present = true) {
        check(device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff18202a, 1, 0), "Clear frame");
        check(device->BeginScene(), "Begin frame");
        const float angle = seconds * 0.6f, cy = std::cos(angle), sy = std::sin(angle);
        const float cx = std::cos(0.45f), sx = std::sin(0.45f);
        const float scale = 1.7f, aspect = float(width) / float(height), z = 1.01f;
        const float matrix[] = {
            cy * scale / aspect, 0, sy * scale / aspect, 0,
            sx * sy * scale, cx * scale, -sx * cy * scale, 0,
            -cx * sy * z, sx * z, cx * cy * z, 4 * z - 0.1f,
            -cx * sy, sx, cx * cy, 4
        };
        check(device->SetVertexShaderConstantF(0, matrix, 4), "Set cube transform");
        check(device->SetTexture(0, texture.value), "Set VTF texture");
        constexpr float points[8][3] = {
            {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1},
            {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1}
        };
        constexpr unsigned faces[6][4] = {{0,3,2,1}, {4,5,6,7}, {0,4,7,3}, {1,2,6,5}, {3,7,6,2}, {0,1,5,4}};
        constexpr float colors[6][3] = {{1,0.7f,0.5f}, {0.6f,0.8f,1}, {0.6f,1,0.8f}, {1,0.9f,0.5f}, {1,1,1}, {0.6f,0.6f,0.6f}};
        constexpr float uv[4][2] = {{0,1}, {0,0}, {1,0}, {1,1}};
        constexpr unsigned indices[] = {0,1,2,0,2,3};
        for (unsigned face = 0; face < 6; ++face) {
            Vertex vertices[6];
            for (unsigned i = 0; i < 6; ++i) {
                const auto corner = indices[i];
                const auto* point = points[faces[face][corner]];
                vertices[i] = {point[0], point[1], point[2], uv[corner][0], uv[corner][1]};
            }
            tint(colors[face][0], colors[face][1], colors[face][2]);
            check(device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, vertices, sizeof(Vertex)), "Draw textured cube");
        }
        check(device->SetVertexShaderConstantF(0, Identity, 4), "Set overlay transform");
        check(device->SetTexture(0, white.value), "Set overlay texture");
        check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, true), "Enable overlay blend");
        tint(0.25f, 0.65f, 1, 0.3f);
        rectangle(-0.65f, -0.55f, 0.65f, -0.7f, 0.01f);
        check(device->SetRenderState(D3DRS_ALPHABLENDENABLE, false), "Disable overlay blend");
        check(device->EndScene(), "End frame");
        return present ? device->Present(nullptr, nullptr, nullptr, nullptr) : D3D_OK;
    }
};

void initializePaths(bool sourceResources) {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path logs = fs::path(ResourceRoot()) / "logs";
    if (!sourceResources) {
#ifdef __OHOS__
        const char* privatePath = SDL_GetOpenHarmonyInternalStoragePath();
#else
        const char* privatePath = SDL_GetAndroidInternalStoragePath();
#endif
        if (!privatePath) throw std::runtime_error("No writable diagnostic directory");
        logs = privatePath;
    }
    fs::create_directories(logs, error);
    if (error || access(logs.c_str(), W_OK) != 0) {
#ifdef __OHOS__
        const char* privatePath = SDL_GetOpenHarmonyInternalStoragePath();
#else
        const char* privatePath = SDL_GetAndroidInternalStoragePath();
#endif
        if (!privatePath) throw std::runtime_error("No writable log directory");
        logs = privatePath;
    }
    fs::rename(logs / "launcher.log", logs / "launcher.previous.log", error);
    errorPath = (logs / "error.txt").string();
    logFile = fopen((logs / "launcher.log").c_str(), "w");
    if (!logFile) throw std::runtime_error("Cannot open launcher.log");
    setvbuf(logFile, nullptr, _IOLBF, 64 * 1024);
    fs::rename(logs / "stdio.log", logs / "stdio.previous.log", error);
    if (!freopen((logs / "stdio.log").c_str(), "w", stdout) || dup2(fileno(stdout), STDERR_FILENO) < 0)
        throw std::runtime_error("Cannot redirect engine stdout/stderr");
    setvbuf(stdout, nullptr, _IOLBF, 64 * 1024);
    setvbuf(stderr, nullptr, _IOLBF, 64 * 1024);
    log("CSGO Android %s; build=%s; pid=%d; SDL %d; ARM64; page size=%ld; logs=%s",
        CSGO_BUILD_TYPE, CSGO_BUILD_ID, getpid(), SDL_GetVersion(), sysconf(_SC_PAGESIZE), logs.c_str());
    {
        const char* envRoot = getenv("CSGO_OHOS_GAME_ROOT");
        const char* sdlPath = SDL_GetOpenHarmonyInternalStoragePath();
        log("CSGO_TRACE: ResourceRoot='%s' env='%s' sdl='%s'",
            ResourceRoot(), envRoot ? envRoot : "(unset)", sdlPath ? sdlPath : "(null)");
    }
    // The Vulkan foundation diagnostic needs neither Source assets nor storage
    // permission. Its private logs remain accessible through Debug run-as.
    if (!sourceResources) return;
    setenv("DXVK_LOG_PATH", logs.c_str(), 1);
    setenv("DXVK_WSI_DRIVER", "SDL3", 1);
    const auto cache = fs::path(ResourceRoot()) / "cache";
    fs::create_directories(cache, error);
    if (error) throw std::runtime_error("Cannot create game cache: " + error.message());
    setenv("DXVK_STATE_CACHE_PATH", cache.c_str(), 1);
    // UI assets ship with the executable. Extract into the app's private storage,
    // then mount this small overlay without modifying the user's retail code.pbin.
    // Android read these from APK assets via SDL's asset fallback; on OHOS the
    // assets live under the game root (extracted from HAP rawfile by ArkTS),
    // so resolve mobile_ui/ against ResourceRoot explicitly.
    const auto uiAssets = fs::path(ResourceRoot()) / "mobile_ui";
    const auto uiRoot = fs::path(
#ifdef __OHOS__
        SDL_GetOpenHarmonyInternalStoragePath()
#else
        SDL_GetAndroidInternalStoragePath()
#endif
    ) / "mobile-ui";
    fs::create_directories(uiRoot / "panorama/mobile", error);
    if (error) throw std::runtime_error("Cannot create mobile UI directory: " + error.message());
    // SDL_LoadFile 在 OHOS 后端对绝对路径解析失败（它把路径交给 rawfile
    // AssetManager 处理），改用直接文件读取。
    auto LoadLocalFile = [](const std::string &path, size_t *outSize) -> void * {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            fprintf(stderr, "CSGO_TRACE: LoadLocalFile FAILED path=%s errno=%d\n", path.c_str(), errno);
            return nullptr;
        }
        file.seekg(0, std::ios::end);
        std::streamoff size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size < 0) return nullptr;
        auto *data = static_cast<void *>(new char[static_cast<size_t>(size) + 1]);
        file.read(static_cast<char *>(data), size);
        static_cast<char *>(data)[size] = '\0';
        if (outSize) *outSize = static_cast<size_t>(size);
        return data;
    };
    auto FreeLocalFile = [](void *data) { delete[] static_cast<char *>(data); };
    size_t manifestLength = 0;
    void *manifestData = LoadLocalFile((uiAssets / "manifest.txt").string(), &manifestLength);
    if (!manifestData)
        throw std::runtime_error("Missing packaged mobile UI manifest: " + (uiAssets / "manifest.txt").string()
            + " (errno " + std::to_string(errno) + ")");
    std::istringstream manifest(std::string(static_cast<const char *>(manifestData), manifestLength));
    FreeLocalFile(manifestData);
    std::string name;
    while (std::getline(manifest, name)) {
        if (name.empty()) continue;
        const fs::path relative(name);
        if (relative.is_absolute() || relative.lexically_normal() != relative || name.find("..") != std::string::npos)
            throw std::runtime_error("Invalid packaged mobile UI path");
        const std::string asset = (uiAssets / name).string();
        size_t length = 0;
        void *data = LoadLocalFile(asset, &length);
        if (!data) throw std::runtime_error("Missing packaged UI asset: " + asset);
        const auto path = uiRoot / "panorama/mobile" / name;
        fs::create_directories(path.parent_path(), error);
        if (error) { SDL_free(data); throw std::runtime_error("Cannot create mobile UI asset directory"); }
        const auto temporary = path.string() + ".tmp";
        FILE *file = fopen(temporary.c_str(), "wb");
        bool saved = file && fwrite(data, 1, length, file) == length;
        if (file && fclose(file) != 0) saved = false;
        FreeLocalFile(data);
        if (!saved || rename(temporary.c_str(), path.c_str()) != 0)
            throw std::runtime_error("Cannot update mobile UI asset: " + path.string());
    }
    setenv("CSGO_MOBILE_UI_PATH", uiRoot.c_str(), 1);
    const auto fontCache = fs::path(
#ifdef __OHOS__
        SDL_GetOpenHarmonyInternalStoragePath()
#else
        SDL_GetAndroidInternalStoragePath()
#endif
    ) / "panorama-fonts";
    fs::create_directories(fontCache, error);
    if (error) throw std::runtime_error("Cannot create font cache: " + error.message());
    setenv("CSGO_FONT_CACHE", fontCache.c_str(), 1);
    setenv("DXVK_HUD", CSGO_DEV_BUILD ? "devinfo,fps" : "", 1);
    setenv("DXVK_CONFIG", "dxvk.enableAsync = True", 1); // fps_max is controlled by the in-game settings.
    setenv("USRLOCALCSGO", (fs::path(ResourceRoot()) / "csgo/local").c_str(), 1);
    if (chdir(ResourceRoot()) != 0) throw std::runtime_error("Cannot enter game resource directory");
    log("SOURCE_RESOURCE_ROOT: %s", ResourceRoot());
}
} // namespace

void androidEngineLog(const char *message) { log("%s", message); }

int main(int argc, char** argv) {
    int seconds = 0, result = 0;
    bool graphicsProbe = false, vulkanProbe = false, vulkanValidation = false;
    Lifecycle lifecycle;
    bool watching = false;
#ifdef __OHOS__
    // Android 的 Java 入口（SDL_AndroidRunMain）会调 SDL_SetMainReady，但 SDL3 的
    // OHOS dlopen 入口没有；不调它，引擎第一次 SDL_Init(SubSystem) 就报
    // "Application didn't initialize properly, did you include SDL_main.h..."。
    SDL_SetMainReady();
#endif
#ifdef __OHOS__
#if CSGO_OHOS_PROBE
    // SDL3 启动 SDL_main 时不带自定义 argv，probe 开关走编译期宏：
    // 隔离验证 DXVK→Vulkan→XComponent 上屏链路（不加载游戏资源）。
    vulkanProbe = true;
    if (seconds == 0) seconds = 15;
#endif
#endif
    for (int i = 1; i < argc; ++i) {
        if (CSGO_DEV_BUILD && strcmp(argv[i], "--graphics-probe") == 0) graphicsProbe = true;
        if (CSGO_DEV_BUILD && strcmp(argv[i], "--vulkan-probe") == 0) vulkanProbe = true;
        if (CSGO_DEV_BUILD && strcmp(argv[i], "--vulkan-validation") == 0) vulkanValidation = true;
        if (strcmp(argv[i], "--probe-seconds") == 0 && i + 1 < argc) seconds = std::atoi(argv[++i]);
    }
    SDL_SetHint(
#ifdef __OHOS__
        ""
#else
        SDL_HINT_ANDROID_BLOCK_ON_PAUSE
#endif
        , "0");
    SDL_SetAppMetadata(
#ifdef __OHOS__
        "CSGO OHOS", "0.1-dev", "com.csgosource.ohos"
#else
        "CSGO Android", "0.1-dev", CSGO_ANDROID_PACKAGE
#endif
    );
    SDL_Window* window = nullptr;
    try {
        initializePaths(!vulkanProbe);
        if (vulkanProbe) {
            if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
            window = SDL_CreateWindow("CSGO native Vulkan diagnostic", 1280, 720,
                SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
            if (!window) throw std::runtime_error(SDL_GetError());
            sourcevk::ProbeOptions options;
            options.seconds = seconds > 0 ? unsigned(seconds) : 15;
            options.validation = vulkanValidation;
            options.outputDirectory = std::filesystem::path(errorPath).parent_path().string();
            options.log = [](const std::string& message) { log("%s", message.c_str()); };
            sourcevk::runProbe(window, options);
        } else if (!graphicsProbe) {
            result = runSourceEngine(argc, argv, ResourceRoot(), errorPath.c_str());
        } else {
        if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
        if (!SDL_AddEventWatch(watchLifecycle, &lifecycle)) throw std::runtime_error(SDL_GetError());
        watching = true;
        const auto asset = readSourceAssets(ResourceRoot());
        log("SOURCE_ASSETS_PASS: %s", asset.description.c_str());
        window = SDL_CreateWindow("CSGO Android graphics check", 1280, 720,
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!window) throw std::runtime_error(SDL_GetError());
        std::unique_ptr<GraphicsProbe> graphics;
        bool quit = false, resize = false;
        unsigned previousGeneration = lifecycle.generation;
        void* previousSurface = nullptr;
        const uint64_t started = SDL_GetTicks();
        uint64_t frames = 0, nextLog = started + 5000;
        unsigned generations = 0;
        while (!quit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    quit = true; break;
                case SDL_EVENT_KEY_DOWN:
                    if (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_AC_BACK) quit = true;
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    resize = true; break;
                default: break;
                }
            }
            if (quit || lifecycle.quit || (seconds > 0 && SDL_GetTicks() - started >= uint64_t(seconds) * 1000)) break;
            void* surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
#ifdef __OHOS__
                                                   SDL_PROP_WINDOW_OPENHARMONY_WINDOW_POINTER,
#else
                                                   SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER,
#endif
                                                   nullptr);
            if (lifecycle.paused || !surface) {
                graphics.reset();
                previousSurface = nullptr;
                SDL_Delay(30);
                continue;
            }
            if (!graphics || resize || surface != previousSurface || lifecycle.generation != previousGeneration) {
                graphics.reset();
                int width = 0, height = 0;
                if (!SDL_GetWindowSizeInPixels(window, &width, &height) || width <= 0 || height <= 0) {
                    SDL_Delay(30); continue;
                }
                graphics = std::make_unique<GraphicsProbe>(window, unsigned(width), unsigned(height), asset);
                previousSurface = surface;
                previousGeneration = lifecycle.generation;
                resize = false;
                log("SURFACE_READY: generation=%u %dx%d", ++generations, width, height);
            }
            HRESULT presented = graphics->render(float(SDL_GetTicks() - started) / 1000);
            if (presented == D3DERR_DEVICELOST || presented == D3DERR_DEVICENOTRESET) {
                log("Present requested device recreation: %08x", unsigned(presented));
                graphics.reset();
                SDL_Delay(30);
                continue;
            }
            check(presented, "Present");
            if (++frames == 1) log("FIRST_FRAME_PRESENTED");
            if (SDL_GetTicks() >= nextLog) {
                log("RENDER_PROGRESS: frames=%llu generation=%u", static_cast<unsigned long long>(frames), generations);
                nextLog = SDL_GetTicks() + 5000;
            }
        }
        graphics.reset();
        log("NORMAL_EXIT: frames=%llu generations=%u", static_cast<unsigned long long>(frames), generations);
    }
    } catch (const std::exception& error) {
        log("STARTUP_FAILED: %s", error.what());
        if (FILE* report = fopen(errorPath.c_str(), "w")) {
            fprintf(report, "CSGO Android %s / %s\nSDL %d / ARM64 / %s\n%s\n\n%s\n",
                    CSGO_BUILD_TYPE, CSGO_BUILD_ID, SDL_GetVersion(),
                    vulkanProbe ? "native Vulkan 1.1" : "DXVK D3D9", ResourceRoot(), error.what());
            fclose(report);
        }
        result = 1;
    }
    if (watching) SDL_RemoveEventWatch(watchLifecycle, &lifecycle);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    if (logFile) { fclose(logFile); logFile = nullptr; }
    return result;
}
