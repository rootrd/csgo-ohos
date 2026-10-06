# 移植状态：完成项与当前卡点

## 2026-10-05 深夜：当前进度与在案卡点（**以此为准**）

### 本轮净进展（均可验证）
| # | 成果 | 证据 |
|---|---|---|
| 1 | **主菜单 1fps → 58.7fps**（引擎计时 399 帧 avg 17.03ms） | 根因=`CMaterialSystem::EndFrame` 等异步队列渲染作业（默认 `mat_queue_mode -1` 自动走 QUEUED_THREADED，作业被服务延迟 ~1s + `YieldWait` 1ms 步进自旋）；修复=`+mat_queue_mode 0` 注入 |
| 2 | **显存压降**：进图短图纹理 1128→458MB、缓冲 404→283MB；菜单态 GL ~466MB | `maxtex=256`（d3d9 caps 运行时上报上限）+ dxvk.conf 内存封顶 |
| 3 | **显示尺寸链修复**：`CSGO_OHOS_DISPLAY` → DXVK monitor → 引擎设备模式 → 交换链 buffer 全链 **2848x1276**（修前被面板原生竖屏 1276x2848 带偏、还钳成 1276x1276 方形）；**左上角 HUD 显示恢复正常** | 日志三连 + 用户目视确认 |
| 4 | **注入通道修复**：所有 cmdline 注入此前从未生效（napi 读浅路径陈旧文件 + engine_startup 读深路径却不赋值）；现已修复并在每条注入实验前自证 | stdio `inject args` 与 overlay 逐字一致 |
| 5 | **看门狗自爆修复**：全线程 SIGUSR2 快照撞刚创建的 FFRT 线程导致进程 SIGILL；已改默认关闭 + 跳过 `OS_*` | 系统 faultlog 实锤 |
| 6 | 呈现/帧率实验三连：**FIFO 已生效**（`d3d9.presentInterval=1` → `VK_PRESENT_MODE_FIFO_KHR`）不解决卡点；**鸿蒙帧率声明**（SDL3 `SetExpectedFrameRateRange(60,120,120)`，返回 0）不解决；系统栏隐藏/横屏预置在该版本 API 未生效 | 真机日志/截图 |
| 7 | 知识库沉淀：`G:\知识库\CSGO-鸿蒙移植\`（SKILL+9 篇）+ 经验库 31/32（soname 双载、注入断链、BiSheng 编译爆栈）+ SourceOH 交叉引用 | — |

### 在案卡点（均已完成定性，含下一步）
1. **合成撕裂**（竖条纹 + 内容只在左上角小块 + 上下白边）：尺寸链全部对齐后仍存在；残留差异为 buffer(2848x1276) vs 窗口可视区(2848x1045)+系统栏。下一步：OHOS buffer geometry/transformHint 与合成器对齐研究；系统栏改 `setSpecificSystemBarEnabled` 变体。
2. **进图 ~1s/帧（"被按住"）**：off-CPU 采样 99.5% 在 `D3D9SwapChainEx::Present → PresentImage → SyncFrameLatency()`（等帧的 GPU 完成信号）；主线程 CPU 1.3%、无发热。菜单态同路径仅 0.7ms。下一步：GPU 时间戳（vkCmdWriteTimestamp）或全屏内容消融定位。

### 已知坑（本轮新增，详见知识库）
- **hvigor 增量缓存可能不重打 libs**：改了库但行为不变时先 `rm -rf hap/entry/build` 再打包。
- **DXVK fork 上游（PomeloTechLabs）已领先 3 周+**（ASTC/EAC、present telemetry、cache recovery）；两条线已分流（父线有 d3d9 BC 模拟族），**整体覆盖不可行，需三方合并**；参考克隆留 `deps/dxvk-ohos-upstream-ref/`。

---

## 历史记录（2026-10-01 → 10-04）

## 重大突破（2026-09-30 → 10-01 通宵推进）

**D3D9 设备 + swapchain 在 Maleoon 920 上完全工作**：
```
19 个引擎系统 Init 全部通过 ✓（历史首次）
→ CSourceAppGroup Main enter ✓（引擎主流程启动，历史首次）
→ 游戏窗口复用 ✓（SDL 单窗口限制绕开）
→ DXVK vkCreateDevice ✓（8 步回退梯子，csgoVkFeatureMask 掩码机制）
→ swapchain 创建 ✓（2848x1045 surface，4 image，MAILBOX，RGBA8）
→ 引擎开始创建纹理 ✓
```

### 本段攻克关卡（接续下表编号）

| # | 卡点 | 根因 | 修复 |
|---|---|---|---|
| 13 | DXVK `CacheModes` 100% CPU 死循环 | OHOS 分支 `GetMonitorDisplayMode` 忽略 modeIndex 且永远返回 TRUE | 只有 index 0/枚举常量返回 TRUE |
| 14 | `GetModeCount` 打转（=13 的表象） | CacheModes 的 `while(GetMonitorDisplayMode(...modeIndex++...))` 永真 | 同上；dxvk GetMonitorDisplayMode 打 idx 日志 |
| 15 | panorama code.pbin "invalid data" 退出 | crypto++ RSASSA 在 OHOS/arm64 对官方签名 pbin 返回 false（待查） | DEVELOPMENT_ONLY 放行（trace 醒目） |
| 16 | 二次建窗撞 SDL 单窗口限制 | sdlmgr 窗口复用在 `#if !defined(ANDROID)` 内（OHOS 构建也定义 ANDROID=死代码）+ GetWindowFromID(1) 实测 NULL | 护栏加 `|| defined(__OHOS__)`；engine_startup 经环境变量 CSGO_OHOS_WINDOW 传窗口指针（libSDL3 全局是 hidden visibility） |
| 17 | **DXVK vkCreateDevice 全拒**（FEATURE_NOT_PRESENT） | **Maleoon 驱动 bug：properties2 报支持的 core feature，vkCreateDevice 一律拒绝**；任何非零 feature（含 Features2-pNext 传法）都 FEATURE_NOT_PRESENT；成功后再创建必 SIGSEGV | dxvk_adapter.cpp 加 **8 步回退梯子**（禁扩展组→剥 pNext→swapchain-only→curated→curated-F2→零 feature），VkResult+扩展清单诊断日志 |
| 18 | Presenter "window not drawable" | autoRegisterFromSdl 查询的属性名错误（`SDL.prop.window.openharmony.window.pointer`，SDL 3.0.5 实名 `SDL.window.openharmony.window`） | 修正属性名（保留旧名 fallback） |

### 当前状态（精确，2026-10-02 下午）

**主菜单已激活稳定运行**（真 V8 5.8.283 + BC 软解 + pbin/CSM/HUD 修复全链，GitHub 862d6b4f/86e19b53/6c071144）。

**地图加载已打通到引擎深处**：`+map de_dust2` 经 valve.rc（stuffcmds 通道）执行 →
Host_Map_Helper ✓ → Map_IsValid OK ✓（de_dust2.bsp 242MB 在位）→ HostState_NewGame 排队 ✓

**最后死点（三次收敛 2026-10-02）**：
1. pbin 毒布局 → 已剥离 ✓
2. hud.xml CS2 类型 → 引擎版 hud.xml ✓
3. base_hud.xml 空壳/HudTopLeft → 引擎版子面板 ✓
4. **当前墙**：CSGOHudRadio 构造时 RequireLoadLayout("hudradio.xml") 失败 →
   Error 退出。panorama/layout/hud/ 下的元件级布局（hudradio.xml 等几百个）
   **在 Steam depot 731 的散装文件里**（不在 VPK——已扫 pak01_dir/cstrike_pak/
   hl2_misc/platform_misc 确认无 panorama/layout），而用户导入的 19GB 包
   （CSNO 打包版）不含桌面布局散装树（CSNO 触控 fork 不需要）。
   **panorama/styles 的 css 同样缺失**（styles/hud/hud.css 不存在）。

**资源补充方案（任选）**：
1. DepotDownloader 补下 depot 731 的 csgo/panorama/ 散装树（布局+css+js）
2. 从 PC 版 CS:GO 安装目录拷 csgo/panorama/（版本接近 2019.10 即可）
3. 注意版本匹配：引擎是 2019.10（kisak-strike），布局需同代；CS2 布局的
   CCSGOTabletPanoLayer 等新面板类型会像 pbin 一样解析失败（但只警告不崩）

**已装机的打点**：State_NewGame 函数体 4 断点（ValidGame/InitGameDLL/
MapIsValid/Host_NewGame）——状态机真进入该帧时会立即给出断裂位置。

**下一步**：
1. 解锁跑一轮：若 NG ValidGame 打印 → 按 4 断点走；若仍无 → 崩在
   排队与帧处理之间的 GameUI/Panorama 路径（查 CGameUI::SwitchToUI
   /LoadingProgress/PlayGame 的 NEW_GAME 响应链，gameui 目录）
2. 重点怀疑：Panorama 主菜单收到 NEW_GAME 后调用的
   MainMenu::LoadMap → 客户端 GameUI_Shutdown/重清理路径

**注入通道最终形态**：valve.rc 直载 `map de_dust2`（libmain 的 env/文件通道均被
shadow：napi setenv 不跨库可见、cmdline.txt 的 ".." 路径 fopen 被沙箱拒绝——
valve.rc 的 stuffcmds 通道已验证可达引擎命令解析器）。

**下一步（精确到行）**：
1. State_Run 尾部 switch 的 `case HS_NEW_GAME: SCR_BeginLoadingPlaque(...)` 打点
   （NewGame 排队后状态机走 HS_GAME_SHUTDOWN 先关旧"游戏"，加载画布在此触发）
2. FrameUpdate 顶部补打 m_nextState
3. SCR_BeginLoadingPlaque 内部打点（它触发 Panorama loading screen 加载——
   加载画布面板类型/脚本在 CSNO 资源包里可能同样有 CS2 时代内容）
已排除：ChangeLevelSP 崩（未到）、MDLCache BeginMapLoad 崩（未到）、
GameShutdown 崩（未到——都没进主循环帧）

### 已验证的驱动行为模型（Maleoon 920 Vulkan 1.3.309）

1. vkCreateDevice：query 报支持的 core feature，创建时**一律 FEATURE_NOT_PRESENT**（pEnabledFeatures 与 Features2-pNext 两种传法皆然）
2. **全零 core feature 可创建成功**
3. 部分位（BC+robust 已证、12 位集已证）通过掩码置位可被接受 → 拒绝的是特定位组合/特定位
4. **成功创建后再调 vkCreateDevice = 驱动 SIGSEGV**（任何后续 attempt 都不行）——梯子必须"失败重试、成功即止"
5. vkEnumerateDeviceExtensionProperties 枚举的扩展（timeline_semaphore 等）创建时也会被拒（扩展无辜，feature 链问题）
6. 失败→失败→…→成功 序列安全；崩溃只发生在成功后的再次创建
7. **BC 硬件缺失的治本方案已合入**（yifengling0 补丁 862d6b4f：D3D9 接 DXVK 现有
   BC 软解 util_bc，DXT 纹理上传/锁定/回读全路径，保留压缩数据与 mip）

## 一、已完成（2026-09-30 及之前，全部有真机日志证据）

| # | 关卡 | 根因 | 修复 |
|---|---|---|---|
| 1 | 启动即闪退（dlopen 失败） | DXVK 库 soname 是 `d3d9.so`，HAP 里叫 `libdxvk_d3d9.so` | 按运行名打包 `d3d9.so` + 依赖 `libdxvk_dxgi.so.0` |
| 2 | onCreate 崩溃（TypeError: filesDir of undefined） | UIAbility 里 `getContext(this)` 返回 undefined | 改用 `this.context` |
| 3 | 日志乱码/错误信息为空 | `ohos_log(format, args)` 把 va_list 传给变参函数 | ohos_log 改收 va_list，OH_LOG_PrintMsg 输出 |
| 4 | Cannot create game cache: Permission denied | ResourceRoot 硬编码安卓路径 | ArkTS setGameRoot 写 `CSGO_OHOS_GAME_ROOT` 环境变量 |
| 5 | Missing packaged mobile UI manifest（两次根因） | ① mobile_ui 未打包；② **napi `SetGameRoot` 参数读进 buf 后从未赋给 `g_gameRoot`** → setenv 写入空串 → 引擎回退 SDL 应用级路径（el2/base/files），与 ArkTS 解包目录（haps/entry/files）错位 | ① mobile_ui 进 rawfile；② napi 赋值修复 + ResourceRoot 改多候选探测（env → SDL+/csgo → haps 视图 → 物理路径，逐个验证 manifest/gameinfo 存在） |
| 6 | SDL video init failed | SDL3 OHOS 不调 SDL_SetMainReady | libmain 入口手动调用 |
| 7 | gameinfo.txt 不存在 | rawfile 资源层级错误 | 正确布局：`rawfile/csgo/` = 游戏根 |
| 8 | 黑屏时点屏幕必崩 | SDL3 触控回调在窗口创建前解引用 NULL | events.c 三处 dispatch 加空防护 |
| 9 | GPU 四色全错 | EnumDisplayDevicesA stub 返回 FALSE → DXVK 0 适配器 | stub 报告 1 个虚拟显示器 |
| 10 | LoadLocalFile 堆损坏（潜伏地雷） | `new std::vector<char>(size)` 后把文件读进 vector 对象头 + SDL_free 释放 | `new char[]` + 配对 FreeLocalFile；失败打印 path+errno |
| 11 | hvigor 00306049 Duplicated libentry.so | build-ohos.sh napi 拷贝自编副本与 hvigor externalNativeOptions 冲突 | build_napi 不再拷贝 |
| 12 | iterate.sh 的 package/hdc 在 WSL 下静默失效 | 混用 /mnt/e 与 /c/ 路径 + `|| true` 吞掉构建失败 | WSL 互操作直调 Git Bash/hdc.exe；失败即中止 |

**真机已验证的初始化推进链**：

```
ResourceRoot ✓ → D3D9 device ✓ → RESZ/INTZ ✓ → 适配器=1 ✓
→ studiorender 全部调试材质 ✓
→ vguimatsurface localize/white-mat/fullscreen-buffer/cursors ✓
→ fonts 块全过 ✓ → vgui2 Init ✓
```

## 二、当前卡点

**vgui2 之后 SDL_main 线程 100% CPU 纯计算打转**（无崩溃、无新日志）。

vgui2 之后的代码路径：

```
InitSystems[全部完成] → CSourceAppSystemGroup::Main
→ g_pEngineAPI->Run() → RunListenServer()
→ ModInit（重 CPU：着色器预缓存）
→ CModAppSystemGroup.Run()   ← 第二层 AppSystemGroup：加载 client_panorama/server/matchmaking
→ 引擎帧循环
```

**已部署的定位手段**（HAP 已装机，等真机解锁跑一轮）：
`AppSystemGroup.cpp` 的 `ConnectSystems`/`InitSystems` 逐系统 `InitSys[i] enter/done`
stderr 打点（launcher_client + engine_client 双副本覆盖）+ `Main enter` 打点。
序列最后一个 "enter 无 done" 的索引即打转系统。

**后续高危预告**：
- `materials->ModInit()`（CEngineAPI::OnStartup 内）着色器预缓存——预期重 CPU 但应有界
- client_panorama 加载后 Panorama UI 的 JS 引擎是 **1.4MB 零返回 v8 桩**（6912 符号）——
  引擎 vscript 用真编译 Squirrel 不受影响，但主菜单 JS 逻辑可能失效/挂起
- Maleoon multi-clear quirk 可能造成渲染象限问题（probe 实测过）

## 三、待办（按优先级）

1. 真机解锁 → 跑 InitSys 二分 → 定位 vgui2 后打转系统（一轮 2 分钟）
2. 若打转在 ModInit：给 OnStartup 各步加 COM_TimestampedLog 输出验证
3. 若打转在 client_panorama：v8 桩行为分析（JSContext 创建路径）
4. 第一帧渲染（DXVK present → XComponent surface，probe 已证明可行）
5. 主菜单出现 → map 加载 → 可玩
