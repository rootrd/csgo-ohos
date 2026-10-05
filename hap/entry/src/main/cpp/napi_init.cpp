#include "napi/native_api.h"
#include <string>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <hilog/log.h>

#define LOG_DOMAIN 0x4353
#define LOG_TAG "CSGOHOS"

static std::string g_gameRoot;
static std::string g_filesDir;
static int g_displayW = 0;
static int g_displayH = 0;

static napi_value SetGameRoot(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_type_error(env, nullptr, "Expected 2 arguments");
        return nullptr;
    }
    char buf1[512] = {0};
    char buf2[512] = {0};
    size_t len1 = 0, len2 = 0;
    napi_get_value_string_utf8(env, args[0], buf1, sizeof(buf1), &len1);
    napi_get_value_string_utf8(env, args[1], buf2, sizeof(buf2), &len2);
    g_gameRoot = buf1;
    g_filesDir = buf2;
    // 传给引擎（libmain.so）：android_main 的 ResourceRoot 优先读 CSGO_OHOS_GAME_ROOT，
    // 覆盖硬编码回退路径（真机沙箱布局因设备/版本而异）
    setenv("CSGO_OHOS_GAME_ROOT", g_gameRoot.c_str(), 1);
    setenv("CSGO_OHOS_FILES_DIR", g_filesDir.c_str(), 1);
    // fontconfig 沙箱配置：默认配置扫 /system/fonts + 不存在的 XDG 目录，
    // FcFontList 在 vgui 字体初始化时打转。只给游戏自带字体目录。
    setenv("FONTCONFIG_FILE", (g_gameRoot + "/fontconfig/fonts.conf").c_str(), 1);
    mkdir(g_gameRoot.c_str(), 0755);
    // 联调注入通道：cmdline.txt（| 分隔）→ CSGO_OHOS_ARGS → 引擎命令行追加。
    // 首选游戏根内的 csgo/cmdline.txt（随 HAP rawfile 打包/随沙箱同步，可维护）；
    // files/cmdline.txt 是 10-02 遗留的旧桥（hdc 已写不进沙箱），仅在首选缺失时兜底，
    // 否则陈旧的 +map|de_dust2 会静默覆盖真实配置（踩过的坑）。
    {
        std::string cmdPath = g_gameRoot + "/csgo/cmdline.txt";
        FILE* f = fopen(cmdPath.c_str(), "r");
        if (!f) {
            cmdPath = g_filesDir + "/cmdline.txt";
            f = fopen(cmdPath.c_str(), "r");
            if (f) OH_LOG_WARN(LOG_APP, "cmdline: fallback to legacy %{public}s", cmdPath.c_str());
        }
        if (f) {
            char buf[1024] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
            if (n) {
                setenv("CSGO_OHOS_ARGS", buf, 1);
                OH_LOG_INFO(LOG_APP, "cmdline inject(%{public}s): %{public}s", cmdPath.c_str(), buf);
            }
        } else {
            OH_LOG_WARN(LOG_APP, "cmdline: no file at %{public}s", cmdPath.c_str());
        }
    }
    OH_LOG_INFO(LOG_APP, "setGameRoot: %{public}s | files=%{public}s", g_gameRoot.c_str(), g_filesDir.c_str());
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value SetDisplaySize(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_type_error(env, nullptr, "Expected 2 arguments");
        return nullptr;
    }
    napi_get_value_int32(env, args[0], &g_displayW);
    napi_get_value_int32(env, args[1], &g_displayH);
    OH_LOG_INFO(LOG_APP, "setDisplaySize: %{public}d x %{public}d", g_displayW, g_displayH);
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value PrepareGameEnv(napi_env env, napi_callback_info info) {
    OH_LOG_INFO(LOG_APP, "prepareGameEnv called");
    if (!g_gameRoot.empty()) {
        std::string csgoDir = g_gameRoot;
        mkdir(csgoDir.c_str(), 0755);
        std::string cfgDir = csgoDir + "/cfg";
        mkdir(cfgDir.c_str(), 0755);
        std::string panoramaDir = csgoDir + "/panorama";
        mkdir(panoramaDir.c_str(), 0755);
    }
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value PingGame(napi_env env, napi_callback_info info) {
    OH_LOG_INFO(LOG_APP, "pingGame called");
    napi_value result;
    napi_create_object(env, &result);
    napi_value okVal;
    napi_get_boolean(env, true, &okVal);
    napi_set_named_property(env, result, "ok", okVal);
    napi_value detailVal;
    napi_create_string_utf8(env, "csgo_hos_napi_bridge_ok", NAPI_AUTO_LENGTH, &detailVal);
    napi_set_named_property(env, result, "detail", detailVal);
    return result;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        {"setGameRoot", nullptr, SetGameRoot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setDisplaySize", nullptr, SetDisplaySize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"prepareGameEnv", nullptr, PrepareGameEnv, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"pingGame", nullptr, PingGame, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "entry",
    .nm_priv = ((void *)0),
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterEntryModule(void) {
    napi_module_register(&demoModule);
}