#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdint>
#include <unwind.h>
#include "tier0/logging.h"
#include "tier0/threadtools.h"
#ifdef __OHOS__
#include "ohos_compat.h"
#endif

extern void androidEngineLog(const char *message);

namespace {
_Unwind_Reason_Code logFrame(_Unwind_Context *context, void *userdata) {
    auto *report = static_cast<FILE *>(userdata);
    // Return addresses point just after the call; use the calling instruction.
    uintptr_t pc = _Unwind_GetIP(context);
    if (!pc) return _URC_END_OF_STACK;
    pc -= 4; // AArch64 instructions are four bytes wide.
    Dl_info module{};
    char frame[1024];
    if (dladdr(reinterpret_cast<void *>(pc), &module) && module.dli_fbase) {
        snprintf(frame, sizeof(frame), "  pc %012llx %s (%s)",
                 static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(module.dli_fbase)),
                 module.dli_fname, module.dli_sname ? module.dli_sname : "?");
    } else {
        snprintf(frame, sizeof(frame), "  pc %012llx <unknown>", static_cast<unsigned long long>(pc));
    }
    androidEngineLog(frame);
    if (report) fprintf(report, "%s\n", frame);
    return _URC_NO_REASON;
}

class EngineLog final : public ILoggingListener {
    std::string m_errorPath;
    bool m_reportedFatal = false;
public:
    std::string lastWarning;
    explicit EngineLog(const char *errorPath) : m_errorPath(errorPath) {
        LoggingSystem_RegisterLoggingListener(this);
    }
    ~EngineLog() { LoggingSystem_UnregisterLoggingListener(this); }
    void Log(const LoggingContext_t *context, const tchar *message) override {
        androidEngineLog(message);
        if (context->m_Severity == LS_WARNING || context->m_Severity == LS_ERROR)
            lastWarning = message;
        // Source's fatal response can terminate the process. Persist its reason
        // before that happens so the Java launcher can offer "copy error".
        if (context->m_Severity == LS_ERROR && !m_reportedFatal) {
            m_reportedFatal = true;
            if (FILE *report = std::fopen(m_errorPath.c_str(), "w")) {
                fprintf(report, "CSGO Android %s / %s\n", CSGO_BUILD_TYPE, CSGO_BUILD_ID);
                std::fputs(message, report);
                std::fputc('\n', report);
                _Unwind_Backtrace(logFrame, report);
                std::fclose(report);
            } else {
                _Unwind_Backtrace(logFrame, nullptr);
            }
        }
    }
};

}

int runSourceEngine(int argc, char **argv, const char *resourceRoot, const char *errorPath) {
    using LauncherMain = int (*)(int, char **);
    EngineLog listener(errorPath);
    // Static module constructors must be covered by the logging listener too.
    androidEngineLog("ENGINE_LOAD: liblauncher_client.so");
    // Load only code packaged in the APK's native library namespace.
    void *launcher = dlopen("liblauncher_client.so", RTLD_NOW | RTLD_LOCAL);
    if (!launcher) throw std::runtime_error(std::string("Cannot load launcher: ") + dlerror());
    auto entry = reinterpret_cast<LauncherMain>(dlsym(launcher, "LauncherMain"));
    if (!entry) {
        const std::string error = dlerror();
        dlclose(launcher);
        throw std::runtime_error("LauncherMain export is missing: " + error);
    }
    // Android's process executable is app_process64, not a game executable.
    // Provide Source's base directory and writable lock/cache location explicitly.
    setenv("TMPDIR",
#ifdef __OHOS__
        SDL_GetOpenHarmonyInternalStoragePath(),
#else
        SDL_GetAndroidInternalStoragePath(),
#endif
        1);
    setenv("SteamAppId", "730", 1);
    // DXVK's SDL3 WSI loads Vulkan through SDL, which requires the video subsystem to be
    // up before materialsystem's CONNECT stage calls Direct3DCreate9. The launcher's SDL
    // manager only does this in its Init() stage, which runs later, so prime it here.
    setenv("DXVK_WSI_DRIVER", "SDL3", 1);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        throw std::runtime_error(std::string("SDL video init failed: ") + SDL_GetError());
#ifdef __OHOS__
    // 预加载着色器 DLL：materialsystem 首次查找非内置 shader（如 "Wireframe"）时才
    // 惰性 dlopen libstdshader_dx9_client.so，实测在 musl 加载器锁下死锁
    // （12 个 debug 材质中 11 个内置 shader 成功，首个自定义 shader 必挂）。
    // 提前用 RTLD_GLOBAL 加载完成，之后 shader 系统查找直接命中已加载库。
    for (const char* dll : { "libstdshader_dx9_client.so" })
    {
        if (!dlopen(dll, RTLD_NOW | RTLD_GLOBAL))
            fprintf(stderr, "CSGO_TRACE: preload %s failed: %s\n", dll, dlerror());
        else
            fprintf(stderr, "CSGO_TRACE: preloaded %s\n", dll);
    }
#endif
    // tier0 captured the "main thread" id when its shared library loaded on Android's UI
    // thread, but the engine actually runs here on the SDL thread. Re-anchor it so
    // ThreadInMainThread() (e.g. CMaterialSystem::ForceSingleThreaded) is correct.
    DeclareCurrentThreadIsMainThread();
    // Keep the window/backbuffer native-sized for HUD, menus and touch coordinates.
    // The client applies the saved scale to the 3D viewport before drawing the HUD.
    const SDL_DisplayMode *mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (!mode || mode->w <= 0 || mode->h <= 0)
        throw std::runtime_error(std::string("SDL display mode query failed: ") + SDL_GetError());
    // 联调注入通道 = 文件通道（唯一可靠）：csgo/cmdline.txt（| 分隔）。
    // env 通道（libentry setenv CSGO_OHOS_ARGS）在 libentry/libmain 间实测隔离，
    // 永远读不到；历史 bug 还让文件内容只写 debug 不生效（已修）。注意本块必须在
    // 窗口/参数构造之前，render=/maxtex= 档位才能被 width/height 采用。
    //   maxtex=NN   → 引擎侧贴图上限（DXVK caps 上报，默认 512 已于 d3d9_adapter）
    //   render=WxH  → 渲染分辨率（引擎视频模式取该值；不改窗口与触控坐标）
    //   其它 token  → 原样追加进引擎命令行（+cvar value 等）
    int renderW = 0, renderH = 0;
    std::vector<std::string> injectedArgs;
    const char* extraArgs = nullptr;
    const std::string cmdFilePath = std::string(resourceRoot) + "/csgo/cmdline.txt";
    {
        FILE* f = fopen(cmdFilePath.c_str(), "r");
        if (!f) {
            // stderr 此刻尚未重定向到 stdio.log，用 debug 文件落盘
            FILE* ef = fopen((std::string(resourceRoot) + "/csgo/cmdline_debug.txt").c_str(), "w");
            if (ef) { fprintf(ef, "fopen FAILED errno=%d path=%s\n", errno, cmdFilePath.c_str()); fclose(ef); }
        } else {
            static char cmdBuf[1024] = {0};
            size_t n = fread(cmdBuf, 1, sizeof(cmdBuf) - 1, f);
            fclose(f);
            while (n && (cmdBuf[n-1] == '\n' || cmdBuf[n-1] == '\r')) cmdBuf[--n] = 0;
            FILE* ef = fopen((std::string(resourceRoot) + "/csgo/cmdline_debug.txt").c_str(), "w");
            if (ef) { fprintf(ef, "read n=%zu content=%s\n", n, cmdBuf); fclose(ef); }
            if (n) extraArgs = cmdBuf;   // cmdBuf 是 static，生命周期安全
        }
    }
    if (!extraArgs)
        extraArgs = getenv("CSGO_OHOS_ARGS");   // 隔离环境下实际到不了这里，仅兜底
    if (extraArgs) {
        std::string token;
        auto handleTok = [&](const std::string& t) {
            if (t.rfind("maxtex=", 0) == 0) {
                setenv("CSGO_OHOS_MAXTEX", t.c_str() + 7, 1);
                fprintf(stderr, "CSGO_TRACE: perf token maxtex=%s\n", t.c_str() + 7);
            } else if (t.rfind("render=", 0) == 0) {
                unsigned w = 0, h = 0;
                if (sscanf(t.c_str() + 7, "%ux%u", &w, &h) == 2 && w >= 640 && h >= 360) {
                    renderW = (int)w;
                    renderH = (int)h;
                    fprintf(stderr, "CSGO_TRACE: perf token render=%ux%u\n", w, h);
                }
            } else {
                injectedArgs.push_back(t);
            }
        };
        for (const char* p = extraArgs; ; ++p) {
            if (*p == '|' || *p == '\0') {
                if (!token.empty()) handleTok(token);
                token.clear();
                if (*p == '\0') break;
            } else {
                token.push_back(*p);
            }
        }
    }
#ifdef __OHOS__
    // DXVK's OHOS monitor stubs read the display size from here; the engine's
    // D3D9 device/swapchain sizing follows it too. 必须与 -w/-h 的横屏归一化
    // 一致：OHOS 面板原生是竖屏（如 1276x2848），直接透传会让 D3D9 交换链按
    // 竖屏建 buffer → 合成器旋转/留白/条纹撕裂（实测）。历史轮次带 render=
    // 档位时恰好绕过了这个不一致，故只在无 render= 时显形。
    {
        char displaySize[64];
        if (renderW && renderH)
            snprintf(displaySize, sizeof(displaySize), "%dx%d", renderW, renderH);
        else
            snprintf(displaySize, sizeof(displaySize), "%dx%d",
                mode->w > mode->h ? mode->w : mode->h,
                mode->w > mode->h ? mode->h : mode->w);
        setenv("CSGO_OHOS_DISPLAY", displaySize, 1);
        // 决定性追踪：mode 原始值 → 归一化后的 env（DXVK monitor 会读它建交换链）
        fprintf(stderr, "CSGO_TRACE: display mode raw=%dx%d -> CSGO_OHOS_DISPLAY=%s\n",
            mode->w, mode->h, displaySize);
    }
    // Create the game window NOW: the XComponent surface already exists (SDL_main
    // is launched from SurfaceCreated), and materialsystem's device/swapchain
    // creation inside LauncherMain needs a window with a bound OHNativeWindow
    // (DXVK autoRegisterFromSdl reads SDL.prop.window.openharmony.window.pointer).
    // sdlmgr's own pre-created window reuses this one (pointer passed via env:
    // libSDL3 的 OPENHARMONY_Window 全局是 hidden visibility，launcher_client
    // 链接不到；环境变量跨库可见且无符号可见性问题)
    SDL_Window *earlyWindow = SDL_CreateWindow("CSGO", mode->w, mode->h,
                          SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!earlyWindow)
        throw std::runtime_error(std::string("Early game window creation failed: ") + SDL_GetError());
    {
        char windowPtr[32];
        snprintf(windowPtr, sizeof(windowPtr), "%p", (void *)earlyWindow);
        setenv("CSGO_OHOS_WINDOW", windowPtr, 1);
    }
#endif
    const int width = renderW ? renderW : (mode->w > mode->h ? mode->w : mode->h);
    const int height = renderH ? renderH : (mode->w > mode->h ? mode->h : mode->w);
    std::vector<std::string> arguments = {
        "csgo_android", "-basedir", resourceRoot, "-game", "csgo",
        "-nosteam", "-insecure", "-novid",
        "-w", std::to_string(width), "-h", std::to_string(height)
    };
    fprintf(stderr, "CSGO_TRACE: inject args=%s (file=%s)\n",
        extraArgs ? extraArgs : "(none)", cmdFilePath.c_str());
    arguments.insert(arguments.end(), injectedArgs.begin(), injectedArgs.end());
    // Installed offline ASTC texture pack (scripts/build-android.sh sync).
    const std::string astcPack = std::string(resourceRoot) + "/astc";
    struct stat astcPackInfo;
    if (stat(astcPack.c_str(), &astcPackInfo) == 0 && S_ISDIR(astcPackInfo.st_mode)) {
        arguments.emplace_back("-astcpack");
        arguments.emplace_back(astcPack);
    }
#if CSGO_DEV_BUILD
    arguments.emplace_back("-condebug");
    arguments.emplace_back("-conclearlog");
#endif
    for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
    std::string command = "ENGINE_ARGS:";
    for (size_t i = 0; i < arguments.size(); ++i)
        command += " [" + (i && arguments[i - 1] == "-netconpassword" ? std::string("<redacted>") : arguments[i]) + "]";
    androidEngineLog(command.c_str());
    std::vector<char *> pointers;
    for (auto &argument : arguments) pointers.push_back(&argument[0]);
    pointers.push_back(nullptr);

    androidEngineLog("ENGINE_ENTRY: calling Source LauncherMain");
    const int result = entry(int(arguments.size()), pointers.data());
    // The engine owns process-wide callbacks and thread-local storage. Keep its
    // module mapped until SDLActivity finishes the process instead of dlclosing it.
    if (result != 0)
        throw std::runtime_error("Source LauncherMain exited with code " + std::to_string(result)
                                 + (listener.lastWarning.empty() ? "" : ": " + listener.lastWarning));
    androidEngineLog("ENGINE_EXIT: Source LauncherMain returned successfully");
    return result;
}
