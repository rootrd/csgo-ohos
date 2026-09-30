# CS:GO 鸿蒙移植 — 接手文档

> 最后更新：2026-09-28
> 项目根目录：`E:\csgo`
> 进度真相源：`E:\csgo\PORTING-PLAN.md`（务必先读此文件）

---

## 1. 项目一句话

将 CS:GO 2019 引擎泄露源码（Android 已完美移植版）移植到 HarmonyOS（鸿蒙）。核心技术栈：DXVK(D3D9→Vulkan) + SDL3 + sse2neon + Panorama 触控 UI。性质是"拼装"而非"攻坚"——在 Android 移植基础上做平台层替换。

## 2. 关键约束

- **所有工作文件放 E 盘，禁 C 盘**（工具如 SDK 可在 C 盘只读引用）
- **编译用 WSL**（Ubuntu-24.04），不用 Windows 原生环境
- **签名由用户自行完成**——只需提供 HAP 工程路径 `E:\csgo\hap`
- **用简体中文交流**

## 3. 已完成阶段（0-6）

### 阶段 0：环境与资源准备 ✅
- 源码 tarball 已解压到 `E:\csgo\CSGO-Source-Linux-20260928\`（639MB）
- OHOS SDK 26.0.0 确认可用（`C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\`）

### 阶段 1：DXVK D3D9→Vulkan 鸿蒙化 ✅
- `d3d9.so`（30MB，ELF aarch64 OHOS native，16KB 对齐）编译成功
- 位置：`E:\csgo\deps\dxvk-ohos-legacy\build.ohos\src\d3d9\d3d9.so`
- 关键修改：
  - `deps/dxvk-ohos-legacy/include/native/windows/windows_base.h` — 加了 ~60 行 `#ifdef __OHOS__` Win32 stub（HCURSOR/WNDPROC/ICONINFO/DISPLAY_DEVICEA/窗口管理 API 等）
  - `deps/dxvk-ohos-legacy/src/dxvk/dxvk_memory.h` — `DxvkSharedHandleInfo.handle` 改为无条件定义
  - `deps/dxvk-ohos-legacy/src/d3d9/meson.build` — 加了 `dxgi_dep + lib_native_window` 依赖
  - `deps/dxvk-ohos-legacy/meson.build` — 加了 16KB"对齐 `add_project_link_arguments`
  - `deps/dxvk-ohos-legacy/build-ohos-win.cross`7 — Windows 路径 cross 文件（含 glslangValidator）

### 阶段 2：SDL3 OHOS 后端 ✅
- `libSDL3.so.0.5.0`（2.2MB，ELF aarch64 OHOS native，16KB 对齐）编译成功
- 位置：`E:\csgo\deps\SDL\build.ohos\libSDL3.so.0.5.0`
- 启用 openharmony video/audio/camera + Vulkan + ogl_es2 后端
- SDL3 OHOS 架构关键点：
  - NAPI 模块名 `"SDL3"`（`src/core/openharmony/SDL_openharmony.c:1199`）
  - 唯一 ArkTS 接口：`provideArkTSObjects(ability, atManager, i18n.System, pointerFn, imeController)`!` — 5 个参数，接管 UIAbility
  - SDL3 自动加载 `pages/Index`，XComponent surface 创建后自动 `dlopen("libmain.so")` → `dlsym("SDL_main")` → `pthread_create` 调用
  - 触控/鼠标/键盘事件由 XComponent 回调自动处理，无需 ArkTS 层转发

### 阶段 3：引擎平台层适配 ✅
- `-DANDROID`→`-D__OHOS__`（CMakeLists.txt + 源码 `#ifdef __OHOS__`）
- `android/log`→`hilog`（`ohos_compat.h` + `android_main.cpp`）
- 资源根路径→OHOS 沙箱（`/data/app/el2/100/base/com.csgosource.ohos/files/csgo`）
- SDL API 替换（`SDL_GetOpenHarmonyInternalStoragePath`、`SDL_PROP_WINDOW_OPENHARMONY_WINDOW_POINTER`）
- 16KB 页对齐验证通过（d3d9.so + libSDL3.so 均 0x4000）
- 引擎入口确认：`#include <SDL3/SDL_main.h>` → `#define main SDL_main` 自动重命名，SDL3 dlopen libmain.so 后调用 SDL_main

### 阶段 4：ArkTS 壳层 + 触控 UI ✅
- HAP 工程：`E:\csgo\hap`（DevEco Studio 可打开签名）
- APK 触控 UI 资源已复制到 `ohos/overlay/` 和 `ohos/boot/`
- DXVK 配置 + autoexec.cfg + boot 资源已复制
- ArkTS 壳层架构（极简）：
  - `EntryAbility.ets` — `onCreate` 中调用 `sdl.provideArkTSObjects(this, ...)` + `entry.setGameRoot()` + `entry.prepareGameEnv()`
  - `Index.ets` — 纯 XComponent（`libraryname: 'SDL3'`），SDL3 接管所有渲染和输入
  - `napi_init.cpp` — NAPI entry 模块（setGameRoot/prepareGameEnv/pingGame）
  - `CommonConstants.ets` — 常量（XCOMPONENT_LIBRARY_NAME='SDL3', GAME_LIB='libcsgo.so', GAME_ENTRY='SDL_main'）
- DevEco Studio 项目配置文件已补齐（oh-package.json5/hvigor-config.json5/hvigorfile.ts/code-linter.json5 等）

### 阶段 5：构建系统 OHOS 化 ✅
- `scripts/build-ohos.sh` — 6 阶段构建脚本（stage_prebuilt/build_napi/stage_resources/package_hap/install_hap/show_logs）
- `android/CMakeLists.txt` — 目标名改为 `main`（产出 `libmain.so`，SDL3 dlopen 约定），sse2neon 路径改为 `runtime/ohos/deps/`，`<OHOS__` 定义 + `hilog_ndk.z` + 16KB 对齐
- HAP 打包流程：预编译 .so 复制到 `hap/entry/libs/arm64-v8a/`，overlay/boot 资源复制到 rawfile/

### 阶段 6：真机联调准备 ✅
- `scripts/diagnose-ohos.sh` — 诊断脚本（check/install/run/logs/probe/crash/all）
- `docs/ohos-debug-checklist.md` — 6 大类 30+ 检查项
- Vulkan 上屏链路确认：SDL3→vkCreateSurfaceOHOS→DXVK D3D9→Present
- Maleoon quirk 确认：DXVK legacy 内置自动检测（strstr(deviceName,"Maleoon")），含 Cube Dref 补齐/CubeArray Dref 模拟/单样本 A2C 禁用/4-MRT+D32S8 修复
- dxvk.conf 移动端优化配置确认（maxFrameLatency=1/kisakEvictManagedOnce/kisakFreeEmptyChunks/kisakChunkSizeMB=32）

## 4. ~~当前阻塞点（阶段 8：WSL 交叉编译）~~ 已解决（2026-09-28）

**原问题**：WSL Ubuntu-24.04 中用系统 clang 18 + OHOS Windows SDK sysroot 交叉编译失败（Windows 版 SDK 的 clang 是 PE32+ 二进制，无法在 WSL 运行）。

**解决方案（方案 a，已落地）**：`E:\commandline-tools-linux-x64-26.0.0.851.zip`（Linux 版命令行工具，内含完整 SDK）解压到 `E:\ohos-cli\`，其中 llvm+sysroot+build+build-tools 复制到 WSL `~/ohos-native`。Linux 版 OHOS clang 15.0.4 在 WSL 原生运行，C/C++ 交叉编译验证通过，`__OHOS__` 为内建宏，sysroot 自动探测。

**构建体系（新增）**：`E:\csgo\scripts\build-ohos-engine.sh`（在 WSL 内运行）— vpc/foundation/engine-deps/text-stack/engine/native/stage 分阶段构建，详见 `PORTING-PLAN.md` §2 阶段 8 的 8.1/8.2 条目（含全部踩坑记录）。源码在 WSL `~/csgo-src` 构建（vhdx 在 E:\WSL，物理仍在 E 盘），产物由 stage 回传 E 盘树。

## 5. 待完成工作

### 阶段 8：引擎 WSL 交叉编译 ✅ 全部完成（2026-09-29，踩坑全记录见 PORTING-PLAN.md §2 阶段 8）

- [x] 8.1 WSL 交叉编译环境（Linux 版 commandline-tools + makefile 参数化）
- [x] 8.2 引擎基础库（tier0/vstdlib/filesystem_stdio.so + tier1/tier2/mathlib/interfaces/vpklib/bitmap/vtf.a）
- [x] 8.3 引擎依赖库 + **Panorama 文本栈**（fribidi/libffi/pcre2/glib/harfbuzz/fontconfig/pixman/cairo/pango 全部源码构建，musl 共享库）
- [x] 8.4 引擎主模块 26 个 .so（client_panorama 32.6MB / server 26.3MB / engine 17.5MB 等）
- [x] 8.5 native 层 libmain.so（AArch64 / 16KB / 导出 SDL_main）
- [x] 8.6 产物回传 E 盘树 + `hap/entry/libs/arm64-v8a/`（90 个库，依赖闭合验收通过）

**复现构建**（WSL 内）：`cd /mnt/e/csgo && BUILD_JOBS=5 bash scripts/build-ohos-engine.sh [vpc|foundation|engine-deps|text-stack|engine|native|stage]`；源码改动先跑 `bash scripts/wsl-sync.sh`。

### 阶段 6 剩余（真机验证）
- [x] 6.4b 签名打包 ✅（2026-09-29）：**`E:\csgo\csgo-ohos-0.1.0-signed.hap`（131MB）已签名可直接装机**（hap-sign-tool 验签通过，debug profile 绑定生成时的设备 UDID，换设备需 DevEco 重新生成 profile）。命令行构建：`cd CSGO-Source-Linux-20260928 && bash scripts/build-ohos.sh package`（CLI_TOOLS + DevEco JBR java；EntryAbility 含 rawfile→沙箱种子解包器）
- [ ] 6.5 性能基线（帧率采样）
- [ ] 6.6 触控 UI 真机验证
- [ ] 6.7 音频 OHAudio 出声验证
- [ ] 6.8 稳定性（前后台切换/Surface 重建/崩溃取证）

**已知运行期风险**：① v8 为零返回桩（保证可加载、vscript 不可用，真机看日志再定）；② fontconfig 需运行期字体目录（Panorama 文本不显示时查 FONTCONFIG_FILE）；③ phonon 为死代码桩（安全）；④ libSDL3.so / libSDL3.so.0 双名已打包。

### 阶段 7：收尾验收
- [ ] 7.1 完整可玩：Dust II 进图 + 移动 + 武器 + 触控全流程
- [ ] 7.2 文档沉淀：回写经验到 `G:\知识库\鸿蒙移植经验库\`
- [ ] 7.3 清理临时产物

## 6. 关键文件位置

### 工作目录
| 文件/目录 | 路径 | 说明 |
|---|---|---|
| 进度真相源 | `E:\csgo\PORTING-PLAN.md` | 必读，所有阶段进度记录 |
| 源码树 | `E:\csgo\CSGO-Source-Linux-20260928\` | 639MB，引擎+安卓移植层 |
| HAP 工程 | `E:\csgo\hap\` | DevEco Studio 可打开签名 |
| 构建脚本 | `E:\csgo\CSGO-Source-Linux-20260928\scripts\build-ohos.sh` | OHOS 构建脚本 |
| 诊断脚本 | `E:\csgo\CSGO-Source-Linux-20260928\scripts\diagnose-ohos.sh` | 真机联调诊断 |
| 检查清单 | `E:\csgo\CSGO-Source-Linux-20260928\docs\ohos-debug-checklist.md` | 6 大类 30+ 检查项 |
| OHOS SDK 副本 | `E:\csgo\ohos-sdk\` | sysroot+clang runtime+toolchain（无空格路径） |

### 预编译产出
| 文件 | 路径 | 大小 |
|---|---|---|
| d3d9.so | `E:\csgo\deps\dxvk-ohos-legacy\build.ohos\src\d3d9\d3d9.so` | 30MB |
| libSDL3.so | `E:\csgo\deps\SDL\build.ohos\libSDL3.so.0.5.0` | 2.2MB |

### OHOS SDK（C 盘，只读引用）
| 路径 | 说明 |
|---|---|
| `C:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\` | OHOS SDK 26.0.0.32 |
| `...\native\llvm\bin\clang.exe` | OHOS clang 15.0.4（BiSheng） |
| `...\native\sysroot\usr\lib\aarch64-linux-ohos\` | libvulkan.so/libnative_window.so 等 |
| `...\native\build\cmake\ohos.toolchain.cmake` | CMake 工具链文件 |

### 参考项目
| 路径 | 说明 |
|---|---|
| `G:\知识?识库\` | 鸿蒙移植经验库（01-27 + README） |
| `G:\知识库\SourceOH_CSGO鸿蒙移植学习笔记.md` | 前序项目经验（SDL2+togles 路线） |
| `E:\StardewValley-HOS\hap\` | Stardew Valley 鸿蒙 HAP（壳层范式参考） |
| `E:\csgo\deps\SDL\openharmony-project\` | SDL3 官方 OHOS 示例工程 |

## 7. SDL3 OHOS 架构要点（接手必读）

SDL3 的 OHOS 后端与 SDL2 完全不同，ArkTS 层极简：

1. **`EntryAbility.ets`** 的 `onCreate` 中调用 `sdl.provideArkTSObjects(this, abilityAccessCtrl.createAtManager(), i18n.System, pointer.setPointerVisibleSync, inputMethod.getController())` — SDL3 接管 UIAbility
2. SDL3 自动在 `onWindowStageCreate` 中加载 `pages/Index`
3. **`Index.ets`** 只有一个 XComponent（`libraryname: 'SDL3'`）
4. XComponent surface 创建后，SDL3 自动 `dlopen("libmain.so")` → `dlsym("SDL_main")` → 在新线程调用
5. 引擎的 `main()` 函数因 `#include <SDL3/SDL_main.h>` 被 `#define main SDL_main` 重命名为 `SDL_main`
6. 触控/鼠标/键盘事件由 XComponent 回调自动处理，无需 ArkTS 层2手动转发
7. Panorama 触控 UI（csno_touch_hud.js 等）在引擎内部运行，不依赖 ArkTS 按钮

## 8. 引擎启动链路

```
ArkTS EntryAbility.onCreate()
  → sdl.provideArkTSObjects(this, ...)  // SDL3 接管 UIAbility
  → entry.setGameRoot(filesDir + '/csgo', filesDir)  // NAPI 设置游戏根目录
  → entry.prepareGameEnv()  // NAPI 创建目录结构

SDL3 onWindowStageCreate()
  → windowStage.loadContent('pages/Index')

Index.ets 渲染
  → XComponent(libraryname='SDL3') 创建
  → SDL3 注册 XComponent 回调（surface/touch/mouse/key）

XComponent surface created
  → SDL_OpenHarmonyMainSurfaceCreated()
  → SDL_RunApp(0, NULL, RunAppOpenHarmonyMain, NULL)
  →$RunAppOpenHarmonyMain()
  → dlopen("libmain.so")
  → dlsym("SDL_main")
  → pthread_create → SDL_main()  // 引擎 main() 入口

SDL_main() [android_main.cpp:443]
  → initializePaths()
    → ResourceRoot = "/data/app/el2/100/base/com.csgosource.ohos/files/csgo"
    → setenv("DXVK_WSI_DRIVER", "SDL3", 1)
    → setenv("DXVK_LOG_PATH", logs, 1)
  → runSourceEngine(argc, argv, ResourceRoot, errorPath)
    → dlopen("liblauncher_client.so")
    → dlsym("LauncherMain")
    → LauncherMain(argc, argv)  // Source 引擎启动
```

## 9. 构建依赖关系

引擎 .so 编译需要以下依赖按顺序构建：

1. **sse2neon**（SSE→NEON 转换）— `git clone https://github.com/DLTcollab/sse2neon.git`
2. **VPC**（Valve Project Creator）— `make -C src/utils/vpc CC=clang CXX=clang++`
3. **基础库**（tier0/tier1/mathlib/interfaces/vstdlib/tier2/vpklib/filesystem_stdio/bitmap/vtf）— VPC + make
4. **依赖库**（protobuf/crypto++/mbedtls/curl/jpeg/freetype/png/parsifal/phonon）— 各自构建系统
5. **引擎主模块**（launcher/engine/filesystem/inputsystem/vphysics/materialsystem/shaderapidx9 等）— VPC + make
6. **native 层**（libmain.so = android_main.cpp + engine_startup.cpp + source_assets.cpp）— CMake

## 10. 接手步骤

1. 读 `E:\csgo\PORTING-PLAN.md` 了解完整进度
2. 读本文档了解项目全貌
3. 从阶段 8.1（解决 WSL 交叉编译环境）开始
4. 推荐方案：下载 Linux 版 OHOS SDK 到 E 盘，在 WSL 中使用
5. 编译顺序：sse2neon → VPC → 基础库 → 依赖库 → 引擎主模块 → native 层
6. 产出 .so 复制到 `E:\csgo\hap\entry\libs\arm64-v8a\`
7. 用户用 DevEco Studio 打开 `E:\csgo\hap` 签名打包
8. 用 `diagnose-ohos.sh` 真机联调