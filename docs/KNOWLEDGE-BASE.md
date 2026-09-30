# csgo-ohos 知识库 —— 知识点与坑全集

> 从移植全过程提炼，按主题组织。每条尽量给「现象 → 根因 → 解法」。
> 持续更新；联调战报见 [PORTING-PLAN.md](PORTING-PLAN.md)，当前状态见 [../STATUS.md](../STATUS.md)。

## 1. 工具链与交叉编译

- **WSL 布局**：`~/ohos-native/`（Linux 版 command-line-tools 解压：llvm+sysroot+cmake）、
  `~/csgo-src/`（源码构建副本，ext4 vhdx 放 E 盘）。E: 树是补丁真相源，WSL 是构建区，
  方向性同步：**源码 E:→WSL，构建产物 WSL→E:**。
- **OHOS clang 必须显式带 target+sysroot**：`clang --target=aarch64-linux-ohos --sysroot=...`。
  工具链文件只覆盖 cmake 路径；手动 make 不带参数会编成宿主 x86-64（报错在链接期才暴露）。
- **VPC 生成 makefile**：`vpc_linux /csgo /androidarm64 /define:DEVELOPMENT_ONLY @launcher @engine ...`
  → `csgo_android_engine.mak`；`DEVELOPMENT_ONLY=1` 让 Panorama 走散装文件而非 code.pbin。
- **makefile_base_posix.mak 参数化**：`ANDROID_TOOLCHAIN/ANDROID_TARGET/SSE2NEON_DIR/
  DEPS_PREFIX/ENGINE_SYSLIBS ?= ` 全部环境变量可覆盖，WSL 脚本注入。
- **重链模块必须删两处产物**：`game/bin/...` 和 `src/<模块>/obj_*/...`——make 有 copy 步骤
  会把旧产物拷回去，只删一处等于没编。
- **WSL 互操作**：WSL 可直接执行 Windows exe（`/mnt/c/Program Files/Git/bin/bash.exe`）。
  **勿用 cmd.exe /c 中转**——引号转义不可靠，曾把 `\n` 字面量转成真实换行打进源码。
- **iterate.sh 教训**：构建命令后跟 `| grep ... || true` 会把失败静默吞掉 → 旧库静默上机
  浪费一轮真机验证。构建失败必须中止（`set -euo pipefail` + tee 留全量日志）。
- **WSL 树大小写陷阱**：E: 树历史遗留 `appsystemgroup.cpp`（小写），VPC 实际编译
  `AppSystemGroup.cpp`（大写）——打点/补丁必须打真文件，否则静默无效。
- **hvigor 外部原生构建**：`build-profile.json5` 的 `externalNativeOptions` 会自编
  `src/main/cpp` 的 libentry.so；**不要再手工拷贝同名 .so 进 hap/entry/libs/**，
  否则 ProcessLibs 报 00306049 Duplicated files。

## 2. OHOS 沙箱与路径

- **沙箱双路径**：应用级 `/data/storage/el2/base/files`（SDL `GetOpenHarmonyInternalStoragePath`
  返回）≠ HAP 级 `/data/storage/el2/base/haps/entry/files`（ArkTS `context.filesDir`）。
  同一存储的两个视图；资源解包在后者，引擎若用前者路径全错。物理视图
  `/data/app/el2/100/base/<bundle>/haps/entry/files` shell 可直读（日志抓取用）。
- **ResourceRoot 多候选探测**（核心设计）：不要盲信环境变量（ArkTS setenv 有时序风险）。
  候选序列：env → SDL路径+/csgo → SDL 路径把 `/el2/base/files` 替换为
  `/el2/base/haps/entry/files` +/csgo → 物理路径硬编码；取第一个能读到哨兵文件
  （`mobile_ui/manifest.txt` 或 `csgo/gameinfo.txt`）的，全不达标回退首选并显式报错。
- **资源布局**：HAP `rawfile/csgo/` = 游戏根（内含 `csgo/`、`platform/`、`dxvk.conf`、
  `mobile_ui/`、`fontconfig/fonts.conf`）。种子解包（~90 小文件）在 onCreate **同步**完成，
  避免与引擎启动抢资源目录；19GB 大资源走启动页异步导入。
- **mobile_ui manifest**：打包时 `find . -type f ! -name manifest.txt | sort` 自动生成，
  引擎逐文件提取到 SDL 私有目录。
- **引擎读文件**：SDL_LoadFile 在 OHOS 后端把绝对路径交给 rawfile AssetManager 处理会失败，
  沙箱绝对路径用 ifstream 自封装（注意 new[]/delete[] 配对）。
- **bundle 库目录 shell 无权限**：`/data/storage/el1/bundle/libs/arm64/` grep 会
  Permission denied——验证库内容改在本地对 HAP 内 .so 做 `grep -ac`。
- **faultlogger 无权限**：`/data/log/faultlog/` 读不了，崩溃定位靠 stdio.log + hilog 流式。

## 3. NAPI / ArkTS / HAP 工程

- **UIAbility 里必须用 `this.context`**：全局 `getContext(this)` 在 onCreate 阶段返回
  undefined → TypeError（`JSON.stringify(TypeError)==={}` 极具迷惑性）→ JsError 进程被杀。
- **NAPI 参数必须真正赋给全局**：曾犯参数读进 buf 后忘赋 `g_gameRoot` 就 setenv 的错误——
  写进环境变量的是**空串**，下游全部静默错位（manifest 找不到、fontconfig 打转）。
  NAPI 层加 OH_LOG 打印实际值，一眼可见。
- **资源 API 用 Sync 版**：`getRawFileContent`（Promise 版）与引擎启动竞态；
  大文件导入全异步（同步 copyFileSync 9.8GB 阻塞主线程会被 watchdog 杀）。
- **versionCode 每次 package 自动 +1**：OHOS `install -r` 在 versionCode 不变时
  **静默跳过 native libs 更新**。装机后必须 `aa force-stop` + `aa start`（旧进程跑旧库）。
- **不息屏**：`mainWindow.setWindowKeepScreenOn(true)` 只防息屏不防锁屏。
- **锁屏限制**：开发者模式下 hdc 无法解锁（错误 10106102），`uitest uiInput swipe` 无效，
  必须用户手动解锁。等待解锁用轮询 `aa start` 直到成功的后台哨兵。
  **锁屏期间所有「进程已退出」判断都是假的**——应用根本没起来，stdio.log 是上一轮残留。
- **沉浸式全屏**：`setWindowLayoutFullScreen(true)` + 系统栏透明，去底部白条。

## 4. 图形栈（DXVK / Vulkan / Maleoon）

- **DXVK d3d9 按运行名打包**：soname 是 `d3d9.so`，HAP 里也叫 `d3d9.so`（不能带 lib 前缀
  改名），依赖 `libdxvk_dxgi.so.0`（带版本 soname）。
- **适配器枚举**：`enumerateByDisplays` 依赖 `EnumDisplayDevicesA`，stub 返回 FALSE 会
  导致 0 适配器 → `m_Adapters[0]` memcpy(NULL) SEGV。stub 实现单显示器报告；
  显示分辨率从 `CSGO_OHOS_DISPLAY=WxH` 环境变量读。
- **Maleoon 920 quirk**（Vulkan probe 实测）：同一 render pass 内多次
  `vkCmdClearAttachments` **只有第一次生效**（第二次 clear 的白心画不出来）。
  游戏渲染出现象限/背景异常时优先怀疑这里。
- **probe 上屏验证法**：`CSGO_OHOS_PROBE` 开关（android/CMakeLists.txt，CACHE 值需 FORCE
  才刷新）画四色图案——不依赖引擎就能证明「Vulkan→surface」全链通。
  实测 2132 帧 ~120FPS。
- **Vulkan 1.3.309**（Maleoon 920 驱动）满足 DXVK legacy 需求；DXVK_STATE_CACHE_PATH
  指向沙箱可写目录。
- **DXVK 配置**：`DXVK_CONFIG = "dxvk.enableAsync = True"`（着色器异步编译），
  `DXVK_LOG_PATH`/`DXVK_WSI_DRIVER=SDL3`；dxvk.conf 放游戏根（CWD 被 chdir 到那里）。

## 5. SDL3 OHOS 后端

- **双实例坑**：带版本 soname（libSDL3.so.0）+ XComponent dlopen libSDL3.so 会加载
  两份互不相通的实例（触控回调注册在 A，引擎调用在 B）。SDL3 CMakeLists 在 OHOS 下
  不设 VERSION/SOVERSION，输出单一 libSDL3.so。
- **SDL_SetMainReady**：SDL3 OHOS 入口不自动调，libmain 需手动调，否则
  "SDL video init failed"。
- **触控空指针**：引擎加载早期 OPENHARMONY_Window 未创建，触控/鼠标/滚轮回调
  解引用 NULL → SEGV（「不点不闪退」）。events.c 三个 dispatch 加空防护。
- **SDL_GetOpenHarmonyInternalStoragePath** 返回应用级路径（见 §2 双路径）。
- **SDL_GetVersion 0x3005000**（SDL 3.0.5）；lifecycle 事件（WILL_ENTER_BACKGROUND 等）
  只发给 watcher，不进 SDL_PollEvent。

## 6. 引擎层（Source 2019）

- **launcher 组 InitSystems 顺序**：engine → filesystem_stdio → inputsystem → vphysics →
  materialsystem → datacache×3 → studiorender → soundemittersystem → vscript →
  vguimatsurface → vgui2 → engine（末位=CEngineAPI）。
- **AppSystem 两份实例**：`AppSystemGroup.cpp` 同时链进 launcher_client 和 engine_client
  （各自 Connect/InitSystems）——打点会从两份打出，属正常。第二层组
  `CModAppSystemGroup`（RunListenServer 内）加载 client_panorama/server/matchmaking。
- **vgui2 之后的流程**：`Main() = g_pEngineAPI->Run()` → `RunListenServer()` →
  `ModInit` → `CModAppSystemGroup.Run()` → 帧循环。
- **v8 桩 ≠ vscript 不可用**：引擎 vscript 语言是 **Squirrel（真编译）**；v8 桩
  （1.4MB，6912 符号零返回）只影响 **Panorama UI 的 JS 逻辑**（主菜单交互）。
- **fontconfig**：默认配置扫 `/system/fonts`（208 文件）+ 不存在的 XDG 目录，
  FcFontList 在 vgui 字体初始化打转。`FONTCONFIG_FILE` 指向游戏内
  `fontconfig/fonts.conf`：只扫游戏字体目录 + 可写 cachedir。
  相对路径 `platform/vgui/fonts` 依赖 CWD=游戏根（initializePaths 里 chdir）。
- **vguimatsurface Init 顺序**：localize → white-mat → fullscreen-buffer → embedded-panel
  → cursors → fonts（FontManager 单例构造：FT_Init_FreeType + setlocale）。
- **CFontManager() 构造在首次 `FontManager()` 调用时**（函数级 static）——不是 dlopen 时。
- **stderr 打点直写 stdio.log**：initializePaths 里 `freopen(stdout)+dup2(stderr)`，
  `setvbuf(_IOLBF)` 行缓冲，`fprintf(stderr, "CSGO_TRACE: ...\n")` 即时落盘。
  Warning()/Msg() 走引擎控制台**不进 stdio.log**。
- **phase 早期打点会被吞**：stdio 重定向之前的 fprintf(stderr) 进 hilog/丢失——
  早期诊断要走 launcher.log（log() 函数）或 hilog。

## 7. 调试方法论（真机联调循环）

- **打点规范**：`fprintf(stderr, "CSGO_TRACE: ...\n")` → stdio.log（最可靠）。
  hilog 环形缓冲会被系统噪音冲掉，必须**流式抓取**（后台 `hdc shell hilog > log &`）。
- **二分定位法**：挂点前后插成对打点（enter/done），最后一个「有 enter 没 done」即卡点；
  每轮 2 分钟。打点经 python 脚本注入（避免 shell 转义层——曾把 \n 写成真实换行
  导致字符串字面量断裂、编译错误被 make -k 吞掉）。
- **一键迭代**：`BUILD_JOBS=5 bash scripts/iterate.sh [engine|native|all|none]`
  编译→stage→打包（Windows hvigor 互操作）→装机→force-stop→启动→75s→自动摘要。
- **日志位置**：`/data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs/`
  下 launcher.log / stdio.log / error.txt（每次启动 stdio 改名 .previous 滚动）。
- **线程级定位**：`top -H -p <pid>` 看 100% 线程名（SDL_main=引擎主线程）。
- **改一动三**：源码改动 → wsl-sync.sh 列表（否则被旧文件覆盖）→ 重编 → 确认设备上
  库真的更新（本地 grep HAP 内 .so 特征串）。

## 8. 资源与 CSNO 对标

- **下载协议**：Steam depot `730 / 731 / manifest 8472803367147551147`
  （kisak-strike 2019 版本），用 DepotDownloader + 真实 Steam 账号。
  CSNO 的 boot 与 manifest 自 0.2.0 起从未变——已下载资源持续有效。
- **CSNO 版本跟进**：0.2.0→0.2.3 的差异在 UI 功能（准星自定义 csno_crosshair、loadout），
  overlay 资源同步到 `ohos/overlay`，打包顺序：boot 先、overlay 后覆盖。
- **导入路径**：启动页【本地导入资源包】→ 异步解压到 HAP 级 files/csgo →
  资源检查绿灯 → 启动游戏。资源 zip 用 pack-resource-zip.ps1 从下载目录打包。
