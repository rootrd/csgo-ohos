# CS:GO 鸿蒙移植 — 真机联调战报（2026-09-30）

> 本文是 2026-09-29/30 真机联调 sessions 的完整战报，承接 PORTING-PLAN.md（进度真相源）。
> 当前状态：**ResourceRoot 链三连根因已修（napi 空串/堆损坏/单一路径），HAP 1000027 已装机，等解锁验证初始化推进**。

## 一、已攻克的关卡（全部有日志证据）

| # | 卡点 | 根因 | 修复 |
|---|---|---|---|
| 1 | 启动即闪退（dlopen 失败） | DXVK 库 soname 是 `d3d9.so`，HAP 里叫 `libdxvk_d3d9.so` | 按运行名打包 `d3d9.so` + 依赖 `libdxvk_dxgi.so.0` |
| 2 | onCreate 崩溃（TypeError: filesDir of undefined） | UIAbility 里 `getContext(this)` 返回 undefined | 改用 `this.context` |
| 3 | 日志乱码/错误信息为空 | `ohos_log(format, args)` 把 va_list 传给变参函数 | ohos_log 改收 va_list，OH_LOG_PrintMsg 输出 |
| 4 | Cannot create game cache: Permission denied | ResourceRoot 硬编码安卓路径，真机沙箱不同 | setGameRoot 写 `CSGO_OHOS_GAME_ROOT` 环境变量，引擎优先读取 |
| 5 | Missing packaged mobile UI manifest | mobile_ui 未打包且引擎从 CWD 读 | mobile_ui 进 rawfile/csgo，引擎从 ResourceRoot/mobile_ui 读 |
| 6 | SDL video init failed（SDL_main.h 报错） | SDL3 OHOS dlopen 入口不调 SDL_SetMainReady | libmain 入口手动 SDL_SetMainReady() |
| 7 | gameinfo.txt 不存在 | rawfile 资源层级错误（csgo/csgo 双层） | 正确布局：`rawfile/csgo/` = 游戏根（内含 csgo/、platform/） |
| 8 | 黑屏时点屏幕必崩 | SDL3 触控回调在窗口创建前解引用 OPENHARMONY_Window | 触控/鼠标/滚轮回调加窗口空防护（libSDL3 已重编） |
| 9 | GPU 四色全错 | EnumDisplayDevicesA stub 返回 FALSE → DXVK 0 适配器 | stub 报告 1 个虚拟显示器；IsIconic 加 OHOS 分支（隔壁修改，已采纳） |
| 10 | **"Missing packaged mobile UI manifest" 复现 + fonts 打转（2026-09-30 根因）** | `napi_init.cpp SetGameRoot` 参数读进 buf 后**从未赋给 g_gameRoot** → setenv 写入**空串** → 引擎回退 SDL 应用级路径（el2/base/files），与 ArkTS 解包目录（haps/entry/files）错位；FONTCONFIG_FILE 也拼成空根 | napi 赋值修复 + hilog 打印实际值 |
| 11 | LoadLocalFile 堆损坏（潜伏地雷） | `new std::vector<char>(size)` 后把文件读进 vector **对象头** + SDL_free 释放 | 改 `new char[]` + FreeLocalFile 配对；失败时打 path+errno |
| 12 | ResourceRoot 单点故障 | 只信 env（时序脆弱）或 SDL 路径（视图错位） | 多候选探测：env → SDL+/csgo → haps 视图 → 物理路径，取第一个能读到 mobile_ui/manifest.txt 或 csgo/gameinfo.txt 的，env 失效自愈 |
| 13 | hvigor 00306049 Duplicated libentry.so | build-ohos.sh napi 把自编副本拷进 hap libs，与 hvigor externalNativeOptions 自编冲突 | build_napi 不再拷贝（hvigor 自编为准） |
| 14 | iterate.sh 的 package/hdc 在 WSL 下从未跑通 | 混用 /mnt/e 与 /c/ 路径；E:/ohos-cli Windows 路径 WSL 不识别 | 互操作直调 `/mnt/c/Program Files/Git/bin/bash.exe`（勿用 cmd.exe，转义不可靠）+ hdc.exe 走 /mnt/c 路径 |

## 二、当前卡点（精确位置）

引擎初始化推进序列（全部有 CSGO_TRACE stderr 打点证据）：

```
PreInit → 资源挂载 → SetStartupInfo → filesystem ✓ → materialsystem ✓
→ datacache ✓（DATACACHE 接口）→ 【卡：MDLCACHE / 后续系统】
```

特征：进程存活不崩、主线程低 CPU 睡眠——**等待某条件**（非死循环）。
下一轮：给 MDLCache（mdlcache.cpp）、studiorender、vgui2、engine 主循环继续加同款打点（fprintf stderr → stdio.log），收敛循环已建立（每轮 3 分钟）。

## 三、调试基建（已就位，可复用）

1. **一键迭代**：WSL 内 `BUILD_JOBS=5 bash /mnt/e/csgo/scripts/iterate.sh [all|engine|native|none]`
   - 增量编译 → stage → 打包（versionCode 自动+1）→ 装机 → 启动 → 75 秒后自动输出引擎日志摘要
   - **关键**：install 后必须 `aa force-stop` 再 `aa start`，否则旧进程继续跑旧库
2. **打点规范**：`fprintf(stderr, "CSGO_TRACE: ...\n")` → stdio.log（hilog 会被轮转丢失，stderr 直写可靠）
3. **流式抓日志**：`(timeout 120 hdc shell hilog > log 2>&1 &)` 后台流 + 启动应用——hilog 环形缓冲会被系统噪音冲掉，必须流式
4. **WSL 同步**：源码改动后先跑 `scripts/wsl-sync.sh`（E 盘 → WSL 副本）；**新增改动文件记得加进 wsl-sync.sh 列表**
5. **OHOS 安装坑**：`install -r` 在 versionCode 不变时静默跳过库更新 → build-ohos.sh 打包时自动 versionCode+1
6. **probe 模式**：android/CMakeLists.txt `CSGO_OHOS_PROBE` 开关（CACHE 用 FORCE，否则缓存不刷新）——当前已关（0），游戏模式

## 四、资源链路（已验证可用）

- DepotDownloader 命令：`-app 730 -depot 731 -manifest 8472803367147551147`（CSNO 锁定的 kisak-strike 2019 版本，需真实 Steam 账号）
- 打包：`powershell -File E:\csgo\scripts\pack-resource-zip.ps1 <下载目录>` → 桌面 zip
- 导入：启动页【本地导入资源包】（异步化修复后 9.8GB 导入成功验证）
- CSNO 0.2.2 overlay（反馈/画质/视频设置更新）已同步到 ohos/overlay

## 五、遗留风险清单

1. v8 是零返回桩（6912 符号）——保证 client_panorama 可加载，vscript 不可用
2. phonon 是汇编桩——ANDROID 构建下 snd_dma 的 phonon 初始化被编译排除，安全
3. fontconfig 字体目录运行期配置——Panorama 文字不显示时查 FONTCONFIG_FILE
4. **Maleoon 驱动 multi-clear quirk**：同一 render pass 内多次 vkCmdClearAttachments 只有第一次生效（probe 实测）——游戏渲染如出现象限错乱可参考
5. 引擎 pango 文本栈为自建 musl 版——文字渲染异常时查

## 真机联调战报快照（2026-09-30 午间）

引擎初始化推进链（CSGO_TRACE stderr 打点，2026-09-30 真机实测）：
PreInit → ResourceRoot ✓（env 正确、多候选探测生效）→ D3D9 device ✓ → RESZ/INTZ ✓
→ studiorender 全部材质 ✓ → vguimatsurface localize/white-mat/fullscreen-buffer/cursors ✓
→ **fonts 块全过 ✓（lang-check→language→helper→interfaces）** → **vgui2 Init ✓（BaseClass::Init done）**
→ 【当前卡点：vgui2 之后 100% CPU 打转（SDL_main 线程，纯计算循环）】

关键工具进展：iterate.sh 已修为 WSL 全链一键（互操作直调 Windows Git Bash + hdc.exe，117-146 秒/轮）；
engine 构建失败不再被 `|| true` 吞掉。**新打点**：AppSystemGroup per-system InitSys[i] enter/done
（launcher_client+engine_client 双副本都覆盖，含第二层 CModAppSystemGroup）+ SourceAppGroup Main enter。

vgui2 之后的代码路径：Main enter → CEngineAPI::Run → RunListenServer → ModInit（重 CPU：着色器预缓存）
→ CModAppSystemGroup.Run（加载 client_panorama/server/matchmaking）→ 其 InitSystems → 引擎帧循环。
InitSys 序列最后一个"enter 无 done"的索引即打转系统。

教训记录：WSL 树里 appframework 同时存在大写 AppSystemGroup.cpp（真编译）与小写 appsystemgroup.cpp
（wsl-sync 残留不进构建）——打点必须打大写文件；v8 是 1.4MB 零返回桩但只影响 Panorama JS，
引擎 vscript 用真编译 Squirrel。

**等待用户解锁屏幕**（哨兵循环 aa start 中），解锁后自动出 InitSys 定位序列。
高危预告：CModAppSystemGroup 加载 client_panorama（Panorama UI + v8 桩）。
