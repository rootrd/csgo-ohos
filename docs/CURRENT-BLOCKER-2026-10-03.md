# CS:GO 鸿蒙移植 · 当前卡点分析（2026-10-03 凌晨）

> **【第二轮离线分析修正版（10:30）——以此节为准，下文旧结论有误】**
>
> **撤回上一版"vDSO 死循环"结论**。那组恒定的 0x59cd659xxx 帧实际是**信号跳板**（vDSO 固定页里的 rt_sigreturn 路径被 unwind 扫出来的垃圾帧），不是故障现场。真正的现场是 ucontext 里的 pc/lr。
>
> **硬证据（12 张快照完全一致，跨 6 分钟）**：
> - `ctx pc=0x59cd208fb8` —— ld-musl 基址固定为 0x59cd150000（bundle 库有 ASLR：glib 在不同进程间从 5de3b73000 移到 5de3573000；但 **ld-musl 与 vDSO 是固定地址**，跨进程可对账）。pc 文件偏移 = 0xB8FB8，反汇编证实**恰好落在 musl `syscall()` 函数的 `svc #0` 指令上**（`mov x8,x0; ...; svc #0; b __syscall_ret`）→ **主线程永久阻塞在一条原始系统调用里**（pc=svc、SA_RESTART 重启语义、sp 恒定全部吻合）。
> - `ctx lr=0x5de36130cc` —— 运行时 maps lookup 证实它在 **libglib-2.0 的 `fork_exec` 内**（文件偏移 0x14F0CC）；反汇编该区域 = fork_exec 的收尾段（一连串 `g_close()` ×10 + `g_free()` ×3）。`fork_exec` 是静态函数，**只被 `g_spawn_*` 调用** → 主线程是在 **glib 的进程派生（g_spawn）路径里调用系统调用后一去不返**。
> - 挂死时**进程没有任何子进程**（ps 证实）→ fork 从未产出子进程；所有线程 S 态空闲；引擎日志在同一帧内冻结。
>
> **这说明**：地图开始的某一帧里有代码走了 glib 的 g_spawn（经 gio 内部：g_app_info/g_subprocess/dbus 之类），而这套 fork/exec 机制在 OHOS 沙箱里**永远完不成**（不是失败返回，而是卡死在系统调用里）。静态扫描证实：pango/pangoft2/引擎全部库都不直接引用 gio 的 spawn API，调用方必是 gio 内部某条路径（或 glib 内部）——需要运行时抓现行。
>
> **还差最后一格数据（下一步一次装机即可拿到）**：
> 1. 快照处理器加打印 **x8 / x0–x3**（ucontext.regs[8]/[0..3]）→ 直接读出**系统调用号和参数**（220=clone / 260=wait4 / 63=read / 436=close_range / 98=futex…一锤定音）；
> 2. 已装机的 1000133 还带 sp 栈指纹 dump（sp..sp+2KB），可离线扫出 fork_exec 之上完整调用链，定位"谁在调 g_spawn"；
> 3. 可选：glib 源码打点（g_spawn_* 入口打 argv[0]+backtrace）并做 **g_spawn 快速失败版**（直接返回 ENOSYS 不 fork）——若游戏越过卡点，说明该 spawn 是旁路功能，可一举解锁。
>
> 另：早前部署的 tier0/SDL3 "绕 vDSO" 补丁是基于误读的修复，保留无害，但不是本案正解。下文旧分析保留供对照。

---

> （旧版分析，部分结论已作废，见上方修正节）

> **【09:30 更新：已破案，修复已装机待验证】**
> 主线程栈快照看门狗（SIGUSR2 每 30s 自 unwind + 落盘 /proc/self/maps）拿到铁证：
> 8+ 张快照主线程 pc 全部钉死在 `0x59cd659ec4 / 0x59cd65947c`；main_maps.txt 显示该地址
> 位于 `59cd658000-59cd65b000 r-xs [shmm]` —— **vDSO 时钟页**（紧邻 `[kshare]` 内核共享页）。
> 即 musl `gettimeofday/clock_gettime` 的 vDSO 序列锁在设备上永不稳定 → 调用方无限自旋。
> 这同时解释了早上 SIGSEGV 的"野跳转"假象（同一个自旋点 + 崩溃处理器自身二次故障的垃圾 unwind）。
> **修复（versionCode 1000130 已装机）**：
> - tier0 `Plat_FloatTime/Plat_MSTime/Plat_USTime` → `CLOCK_MONOTONIC` 裸 syscall（`src/tier0/platform_posix.cpp`，`__OHOS__` 分支）
> - SDL3 `SDL_GetPerformanceCounter/CheckMonotonicTime` → 同样裸 syscall（`deps/SDL/src/timer/unix/SDL_systimer.c`，Windows 侧 `cmake --build deps/SDL/build.ohos` 增量重编）
> - 验证方法：解锁后启动，若主线程快照 pc 开始在真实代码里流动（不再钉死 vDSO 页）即生效；若卡点复现但 pc 落在其它 vDSO 调用方（DXVK std::chrono、pango），同法逐个绕开
> 下文保留原始分析过程供参考。

---

## 一页结论（原始分析，已上述更新取代结论部分）

**今天已通关**：HUD 骨架布局导致的 SIGSEGV 链（真正的元凶是散装骨架文件遮蔽了 code.pbin 里的真实布局 + 零售面板类型在静态库里没被链接）。
**当前卡点**：`+map de_dust2` 后，引擎主循环**在完成 "+map 命令处理的那一帧之后、下一次 `HostState_Frame` 之前**永久停滞。屏幕只剩一帧黑底 + 左上角一行坏像素。进程活着，主线程 S 态（无 D 态线程 → 不是磁盘 IO）。
**最大嫌疑**：渲染 present/swapchain 段（与 Maleoon 920 的 transformHint=3 竖屏提示 vs 引擎横屏不预旋转的错配相关），其次是真实加载匾（panorama loading screen 首次启用）与主线程的交互死锁。
**下一步已就绪**：已在 libmain 内置「主线程栈快照看门狗」（SIGUSR2 每 30s 让主线程自 unwind 写 `logs/crash_bt.txt`），装包 1000125 已完成但**手机锁屏导致无法启动**（10106102，开发者模式 hdc 不能解锁）。解锁后哨兵会自动拉起，看快照即可定位。

---

## 背景（30 秒版）

- 设备：Pura 80 Ultra，Maleoon 920（Vulkan 1.3.309），2848x1276 横屏（自然方向竖屏 1276x2848），page size 4096
- 应用：`com.csgosource.ohos`，CSGO-Source-Linux-20260928 源码交叉编译（WSL，OHOS clang）
- 图形栈：引擎 D3D9 → DXVK(d3d9.so) → Vulkan（Maleoon ICD），WSI 走 SDL3 OHOS 后端；Panorama UI 用零售版 code.pbin 的内容展开为散装资源
- 一键循环：`scripts/run-win-package.sh`（打包+装机+启动+自动点「启动游戏」）+ `scripts/build-ohos-engine.sh <stage>`（WSL 内编译）
- 日志（沙箱内，shell 可读）：`/data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs/`
  - `stdio.log`：引擎 stderr（CSGO_TRACE 打点）
  - `launcher.log`：引擎 Msg
  - `error.txt`：Plat_FatalError 的消息+栈（布局类错误看这里！）
  - `crash_bt.txt`：SIGSEGV/ABRT/BUS 自捕栈 + 新版主线程周期栈快照
  - `appspawn_d3d9.log`：DXVK 日志

---

## 今天（10-03 凌晨）已修复的三层问题（不要重查）

### 1. HUD 加载 SIGSEGV（已过 ✅）
- 现象：启动即 SIGSEGV，自捕栈符号化到 panorama `BApplyPanelDescription/AddClassesInternal/SetPanelEvent`
- 真因 A：`ShouldUseLoosePanoramaResources()`（`src/public/panorama/iuifilesystem.h`）在 `__OHOS__ && DEVELOPMENT_ONLY` 下**恒返回 true** → code.pbin 完全被忽略，散装树里的手工骨架 XML（28 个，把所有子面板生成为通用 `<Panel>`）遮蔽了 pbin 内的真实布局。HUD 元件构造函数对 `RequireChildInLayoutFile` 结果做 `panel_cast`，通用面板 → 野虚表调用（符号化定到 `CCSGO_HudTeamCounter::UpdateMiniScoreboard`）
- 修复：`scripts/materialize_pbin_loose.py` 把 pbin 全量 514 文件展开为散装（保留触屏 hud.xml 与 CSNO 启动菜单覆盖）+ `scripts/sanitize_hud_xml.py` 把源码快照里不存在的零售类型（CSGORadialRadio 等）替换为 `<Panel>`
- 真因 B：`CircularProgressBar` 只编在 `panorama_client_client.a`（静态库，给 client 链接），client 没有任何代码引用它 → `.a` 成员不被拉入 → 工厂不注册 → compass.xml 解析必炸。修复：`csgo_compass.cpp` 加 `__attribute__((used))` 的 never-called 函数 new 一个，强制拉入
- 另删了 `csgo_hudradio.cpp` 文件尾三行坏追加宏（上轮会话遗留）
- 结果：引擎越过 HUD 关，`+map` 走到 `HostState_NewGame done`（此前所有崩溃点全过）

### 2. 打包链（已修 ✅，与本卡点无关但排障时别再踩）
- ZCode 会话经 WSL→winbash 时 interop 环境 PATH 近空 → es2abc `spawn cmd.exe ENOENT`
- `build-ohos.sh` 已改为直接 `node.exe hvigor/bin/hvigorw.js`；注意 CLI_TOOLS 里的 tool/node 是 **Linux** 版；**PATH 条目带 `/..` 会让 node 子进程 spawn 全崩**（要用 dirname）
- vpc 重新生成 .mak 后不删旧 .so，make 会认为无需重链（新 .o 不进链接）——删 `game/bin` 与 `obj_*/release` 两处产物再编

### 3. 崩溃自捕栈升级（✅）
- SIGSEGV/ABRT/BUS 处理器带 SA_SIGINFO：写 `ctx pc/lr/sp/si_addr` + pc 所在 maps 区域行
- **新增主线程栈快照看门狗**：进程内线程每 30s 向主线程发 SIGUSR2，处理器用 `_Unwind_Backtrace` 把主线程栈写 `crash_bt.txt`（`=== MAIN SNAPSHOT #n ===`）。shell 无权限发信号、processdump/lldb-server 被 SELinux 拒，只能进程内做

---

## 当前卡点：主循环停滞（黑屏一帧）

### 症状时间线（pid 47625 那一轮，02:57 启动）
```
stdio.log 尾部（完整序列，之后不再增长）：
CSGO_TRACE: Init system[2]
CSGO_TRACE: HostStateFrame state=4          ← 只打印过这一次！
CSGO_TRACE: Host_Map_Helper enter
CSGO_TRACE: Map_IsValid OK de_dust2
CSGO_TRACE: HostState_NewGame calling de_dust2.bsp
CSGO_TRACE: HostState_NewGame body enter de_dust2.bsp
CSGO_TRACE: HostState_NewGame done de_dust2.bsp
（此后 stdio 不再有任何输出）
```

### 关键代码事实（主循环精确定位）
- `src/engine/host_state.cpp:56`：`HS_NEW_GAME = 0`；枚举顺延，**state=4 是 HS_RUN**
- 帧流向（已核实调用链）：
  ```
  CEngineAPI::Main 外层循环（sys_dll2.cpp:1444）：while(true){ PumpMessages(); eng->Frame(); }
    └ CEngine::Frame（sys_engine.cpp:572）→ HostState_Frame(m_flFrameTime)
        └ CHostState::FrameUpdate（host_state.cpp:766）：while 循环里【无条件】fprintf "HostStateFrame state=%d"（:797）
            └ case HS_RUN → State_Run → Host_RunFrame（host.cpp:4629）→ _Host_RunFrame（host.cpp:3983）
                ├ Cbuf_Execute()（host.cpp:4132）← +map 在这里执行：Host_Map_Helper 打印
                │   enter/IsValid/calling/body/done 并 SetNextState(HS_NEW_GAME)
                ├ 每 tick 模拟：_Host_RunFrame_Input / _Host_RunFrame_Server / _Host_RunFrame_Client
                ├ _Host_RunFrame_Render()（host.cpp:3425）← 渲染 + present
                ├ _Host_RunFrame_Sound()
                └ ClientDLL_Update()（客户端模拟）
  ```
- `CHostState::FrameUpdate` 每帧必打一行（无条件 fprintf）→ **"+map 那一帧"之后再也没有 "HostStateFrame state=0"** ⇒ 主线程没能活到下一次 `CEngine::Frame`
- ⇒ 卡点必然在这一帧的后半段：`_Host_RunFrame_Render`（渲染/present）、`_Host_RunFrame_Sound`、`ClientDLL_Update`，或 `CEngine::Frame` 尾部——**在 `case HS_NEW_GAME: BeginMapLoad` 之前**（那两行从未打印）
- `case HS_NEW_GAME: g_pMDLCache->BeginMapLoad(); fprintf("State_NewGame calling")`（host_state.cpp:801-803）从未执行

### 运行时证据
- 主线程 S 态；抽样线程全部 S（**无 D 态** → 不是 IO 等待，是锁/条件等待）
- 屏幕定格：全黑 + 左上角一行坏像素（约 380x10 px 的花屏条）→ **只有一帧被 present 过**
- DXVK 日志：device 创建成功；`Buffer size: 1276x2848`（竖屏 buffer）而显示是 2848x1276；`G9_DXVK_SUBMIT_PHASE submissions=37`（提交极少）
- hilog：
  - `XComponent[csgoXComponent] native OnSurfaceCreated`（启动时）
  - `VulkanSwapchainLayer <681>SetWindowTransform: The App Is Not Doing Pre-rotation, transformHint: 3, preTransform(to native): 0`（**重复报**；transformHint 3 = 显示侧要求 270° 旋转，引擎不预旋转）
  - panorama 工作线程在持续加载缺失的 panorama 图片（`Resource panorama/images/... failed to load`），02:57:32 后也停止
- 系统 `AppDfr/appfreeze_manager` 每 5 分钟探测一次（02:48/02:53/02:54，killReason ioctl errno=14）→ 系统也认为应用冻结

### 与「早上」的对照（用户视角）
早上到现在的显示没有区别：黑屏+花屏条。区别只在日志深度：早上的运行连 NewGame 都进不去（SIGSEGV），现在能进到 NewGame done 并定格一帧。

---

## 已排除项（省得再查）

| 假设 | 排除依据 |
|---|---|
| 磁盘 IO 挂死 | 所有线程 S 态，无 D 态 |
| `MDLCache::BeginMapLoad` 死锁 | 埋点（libdatacache_client.so 内 `BeginMapLoad enter`）从未打印——状态机根本没再进入 |
| 引擎状态机自身逻辑 bug | `HostState_NewGame` 只设状态立即返回；卡点在其后的帧间路径 |
| 布局解析残留问题 | 本轮 `error.txt` 无新内容（最后一次 Plat_FatalError 是 01:51 的 compass 时代） |
| 库没装上 | bm dump versionCode=1000125 ✓；HAP 内 libdatacache 已验证含新埋点字符串 |

## 候选假设（按嫌疑排序，结合帧流向精确化）

1. **`_Host_RunFrame_Render` 内的 present/swapchain 挂死（嫌疑最大）**：渲染段位于 Cbuf(+map) 之后同帧内。旁证：只有一帧上屏（黑+花屏条）、GPU 提交数极低（submissions=37）、`SetWindowTransform` 反复报 transformHint=3 错配（显示侧要求 270° 旋转而 DXVK preTransform=0）、主线程 S。若 present 等一个永不触发的 fence/缓冲即全部吻合。
2. **`ClientDLL_Update` 首次带图客户端模拟阻塞**：+map 刚设状态，客户端 DLL 在下一状态前被推了一帧模拟；若 GameRules/实体系统在某资源缺失上自旋等待（全 S 态吻合），或与 panorama HUD（真实 hud.xml 首次整链加载）互等。
3. **`_Host_RunFrame_Sound` 阻塞**：声音系统初始化/混音线程等待（已知 phonon 依赖是自编的）。全 S 态也吻合。
4. **加载匾（loading plaque）死锁**：今天第一次启用真实 `base_loadingscreen.xml`/`loadingscreen.xml`（之前是 CSNO 空壳）。SCR_BeginLoadingPlaque/SCR_UpdateScreen 走 panorama，可能与 V8/主线程互等。
5. 渲染资源（de_dust2 材质/bsp 附属文件）加载线程静默死亡，主线程等它。panorama 图片加载在 02:57:32 停止是弱旁证（也可能只是加载完列表）。

## 复现步骤（手机解锁后即可全自动）

```bash
# Windows Git Bash：
E:/csgo/scripts/run-win-package.sh   # 打包+装机+启动+自动点击(1424,927)+等75s
# 或已装机时：
hdc shell "aa start -a EntryAbility -b com.csgosource.ohos"
sleep 15; hdc shell "uitest uiInput click 1424 927"   # 点「启动游戏」→ valve.rc +map de_dust2
# 约 90~180s 后到达卡点；然后：
hdc shell "cat /data/app/el2/100/base/com.csgosource.ohos/haps/entry/files/csgo/logs/crash_bt.txt | grep -A12 'MAIN SNAPSHOT'"
# 每 30s 一张主线程栈快照（module+offset），用 WSL llvm-addr2line -Cfipe <lib> <offset> 符号化
# 库文件：/mnt/e/csgo/CSGO-Source-Linux-20260928/game/{bin,csgo/bin}/androidarm64/release/*.so
```

## 符号化注意事项（踩过的坑）

- 设备 `libclient_panorama_client.so` ← 构建 `game/csgo/bin/.../libclient_panorama_client.so`（cstrike15 client 大库，24MB）；`libpanorama_client.so` ← `game/bin/.../libpanorama_client.so`（panorama 核心库）。别搞混
- 自捕栈 `_Unwind_Backtrace` 在野跳转之后帧不可信（会扫到陈旧返回地址）；但本次卡死是正常阻塞，栈应可信
- 处理器已写 `ctx pc/lr/sp`（SIGSEGV 场景）；MAIN SNAPSHOT 只有 unwind 帧（无 ucontext，首帧 = 信号处理函数本身，第二帧 = trampoline，往下才是主线程真实栈）

## 其它已知环境坑（分析时避免误判）

- 手机锁屏 → `aa start` 报 10106102（开发者模式不能自动解锁），需人工解锁一次；后台哨兵 `scripts/sentinel-relaunch.sh` 在轮询
- install -r 在应用运行中执行，native 库更新可能延迟提交（hilog `Updated upgraded app ... version from X to Y` 出现才算完）；保险顺序 = force-stop → install → 等 upgrade 提交 → start
- stdio.log 每次启动轮转为 stdio.previous.log；看错文件会得出错误结论
- 设备上 `/root/csgo-src`（WSL 内的源码副本）是死树，**真正的构建树是 E:\csgo\CSGO-Source-Linux-20260928（经 /mnt/e）**；在死树里删产物是徒劳的
- Maleoon 920 已知怪癖：同 render pass 多次 vkCmdClearAttachments 只有第一次生效；vkCreateDevice 成功后再次创建必 SIGSEGV（dxvk_adapter 8 步回退梯子已处理）

## 当前部署状态

- versionCode 1000125 已安装（含 BeginMapLoad 埋点、MAIN SNAPSHOT 看门狗、compass 强制链接、全部真实布局）
- 待办：解锁手机 → 哨兵自动拉起 → 读 3~5 张 MAIN SNAPSHOT → 符号化主线程栈 → 按栈修卡点
