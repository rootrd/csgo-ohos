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

- **Maleoon 920 vkCreateDevice 行为模型（2026-10-01 实证，最关键）**：
  1) properties2 查询报支持的 core feature，vkCreateDevice **一律 FEATURE_NOT_PRESENT**（pEnabledFeatures / Features2-pNext 两种传法皆拒）
  2) 全零 core feature 可创建；**部分位经掩码置位可被接受**（robust+BC 已证、12 位集已证）→ 用 dxvk.conf `csgoVkFeatureMask`（位序掩码，DXVK 分支已支持）试探真实支持集
  3) **成功创建后绝不能再调 vkCreateDevice = 驱动 SIGSEGV**；失败→失败→…→成功序列安全 → 回退梯子必须「失败重试、成功即止」
  4) 扩展枚举无辜（timeline_semaphore 等枚举得到但创建链被拒是 feature 链问题）
- **DXVK 8 步回退梯子**（dxvk_adapter.cpp createDevice）：禁扩展组(1:ts+m4, 2:demote/r2/eds, 3:tf/hqr/rp2/dsr/dic/vad)→剥 pNext(4)→swapchain-only(5)→curated(6)→curated-F2(7)→零+mask(8)。每个 FallbackStep 同时 disable 扩展并从 pNext 摘除对应 feature struct——**禁扩展必须同步摘 struct，否则链上残留照样被拒**
- **SDL3 窗口属性真名**：`SDL.window.openharmony.window`（`SDL_PROP_WINDOW_OPENHARMONY_WINDOW_POINTER`）。旧式 `SDL.prop.window.*` 名不存在→查 properties 得 NULL
- **跨库传 SDL_Window\***：libSDL3 全局（OPENHARMONY_Window）是 hidden visibility，其他库链接不到；用环境变量传 `%p`（strtoull base16 解回）或走 SDL properties
- **GetWindowFromID(1) 不可靠**：窗口 ID 不保证从 1 开始（实测首个窗口 id=2）
- **DXVK d3d9 按运行名打包**：soname `d3d9.so`（不能改名），依赖 `libdxvk_dxgi.so.0`
- **适配器枚举**：EnumDisplayDevicesA stub 需实现单显示器；`CSGO_OHOS_DISPLAY=WxH` 控制 DXVK 分辨率
- ** Maleoon quirk**：同一 render pass 多次 vkCmdClearAttachments 只有第一次生效（probe 实测）
- **ohos_log 必须 (format, va_list)**；**DXVK 诊断**：错误要带 VkResult 和请求扩展清单（默认丢弃，排查全靠补日志）
- **dxvk.conf 特殊键**：本移植加 `csgoVkFeatureMask`（dxvk_adapter 直接解析）；dxvk.conf 在游戏根，经 overlay → rawfile → seed 每次启动覆盖（shell 写不进沙盒，改源头）

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

## 9. 显存压降、卡顿定位与在案卡点（2026-10-05）

- **菜单 1fps → 58.7fps（定案）**：根因 `CMaterialSystem::EndFrame()`
  （cmaterialsystem.cpp:4241）同步等 `m_pActiveAsyncJob`（MATERIAL_QUEUED_THREADED，
  `mat_queue_mode` 默认 -1 且核数≥2 自动触发）；`CJob::WaitForFinish → CThreadPool::YieldWait
  → CThreadSyncObject::WaitForMultiple`（threadtools.cpp:3210）1ms/事件自旋。
  **解法=`+mat_queue_mode 0`**（注入 cmdline.txt）。
- **显存压降（实测）**：D3D9 caps MaxTextureWidth/Height 走 `CSGO_OHOS_MAXTEX`
  （默认 512，可调 256~16384，d3d9_adapter.cpp）。`maxtex=256` 后：
  images 1128→458MB、buffers 404→283MB，菜单态 GL 466MB。
  盘点工具 `hidumper --mem <pid>`。
- **注入通道（唯一可靠路径）**：`ohos/overlay/csgo/cmdline.txt`（`|` 分隔）→ 打包 →
  沙箱 `files/csgo/csgo/cmdline.txt` → engine_startup 读取并**赋值 extraArgs**。
  perf token：`maxtex=NN`（纹理上限）、`render=WxH`（渲染分辨率）。
  双坑：napi 必须**深路径优先**（浅路径是历史遗留）、engine_startup 读到后必须真正赋值。
- **显示尺寸链（已修通）**：engine_startup 视频模式**横屏归一化**（w>h 时才原样，
  否则交换）→ `CSGO_OHOS_DISPLAY` → DXVK util_monitor.cpp `OhosDisplaySize()` →
  GetMonitorDisplayMode → 交换链 buffer。目标机 2848x1276，左上角 HUD 显示正常（用户确认）。
- **SyncFrameLatency 卡点（在案）**：进图后 ~1s 帧间隔，off-CPU 99.5% 在
  `D3D9SwapChainEx::Present → PresentImage → SyncFrameLatency()`（等帧的 GPU 完成信号）；
  主线程 CPU 1.3%、整机无发热 = **纯等待，非软件负担**。musl 下
  `pthread_cond_timedwait` 是 `pthread_cond_wait` 的内部实现（无真超时，排除「1s 超时」假说）。
  下一步：`vkCmdWriteTimestamp` 打 GPU 时间戳，或全屏内容消融（空渲染看 Present 耗时）。
- **合成撕裂卡点（在案）**：画面竖条纹/左上内容小块/上下白边；全链尺寸对齐后仍在，
  残留差异 = buffer 2848x1276 vs 窗口可视区 2848x1045+系统栏。下一步研究
  OHOS buffer geometry / transformHint 与合成器对齐（含 `setSpecificSystemBarEnabled` 变体）。
- **hvigor 增量缓存陷阱**：改 hap/entry/libs 或 native 代码后，**必须 `rm -rf hap/entry/build`**
  重打，否则旧 .so 静默上机（outputs/default 偶发 busy → 稍候重试）。
- **SDL3 帧率声明（我们新增，官方无）**：`OH_NativeXComponent_SetExpectedFrameRateRange
  (60,120,120)` 已加在 SDL_openharmonyvideo.c surface created；FIFO 走
  dxvk.conf `d3d9.presentInterval=1` 已生效但**不解决卡顿**（帧率声明与 FIFO 均已排除）。
- **hiperf 采样陷阱**：采样分布可能误导（显示 90% 在 SDL_main，但 /proc ticks 实测 1.3%）
  —— 以 `/proc/<pid>/task/*/stat` 计数为准。cppcrash 日志用 `hdc file recv` 取证
  （shell cat 无权限）。

## 10. 上游 DXVK fork 与参考情报（2026-10-05）

- **fork 谱系**：PomeloTechLabs `dxvk-ohos-legacy`（DXVK 1.10.3，**我们在用**）
  vs `dxvk-ohos-modern`（DXVK 2.6.2，渲染同步架构重写）。两条线已分流。
- **上游 HEAD（9-27）新增**：ASTC/EAC 格式支持 + etcpak 编码器 + present telemetry；
  但**整体覆盖同步不可行**（父线有 `d3d9 IsBcEmulated` BC 模拟族 26 处，现代线没有）
  → 需三方合并。参考克隆在 `deps/dxvk-ohos-upstream-ref/`（.gitignore 排除）。
- **Maleoon 驱动怪癖（@WINEHUA_FORK.md）**：Cube Dref 需 padded vec4；原生 CubeArray Dref
  挂死 Host Venus ring → 用 2D-array 模拟；不支持 BC/DXT → 软件解 BC1-7。
- **参考情报**：CS:NO 0.3.0 同走 DXVK 且**不链 glib/pango**（freetype+harfbuzz+sheenbidi
  文本栈）——我们的 Panorama 文本栈坑属自引入负担；死亡细胞 ASTC 管线成功但死于
  BiSheng 编译器栈爆（小栈线程 + `libbishenggpucompiler` 深递归，修法=pthread_attr_setstacksize
  或大栈线程编译）。
