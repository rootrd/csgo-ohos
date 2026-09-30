# 移植状态：完成项与当前卡点（2026-09-30）

## 一、已完成（全部有真机日志证据）

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
