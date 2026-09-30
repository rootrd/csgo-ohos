# Linux / SDL3 / Clang 构建

当前验证环境为 Distrobox `dev`（Arch Linux）、x86_64、Clang 22.1.8。
宿主机 SteamOS 不需要安装编译环境。源码树依赖本地已有的第三方预编译库；
它们被 `.gitignore` 排除，单独克隆 Git 仓库并不足以完成链接。

2026-09-22 离线基线：完整构建、Dust II 本地连接、场景/HUD、移动、枪械显示与伤害、
返回控制台及正常退出已通过；用户确认换弹正常。控制台协议回归通过。
原版大厅/社交面板缺失仍会产生部分 Panorama JS 警告，不属于当前离线入口的功能范围。

## 构建

在仓库根目录执行：

```sh
bash scripts/build-linux.sh
```

脚本会进入 `dev`，构建只启用 SDL3 的 native DXVK D3D9，随后用 Clang 编译 VPC，
重新生成 CSGO Linux 64 位工程，再以 4 个并发任务编译 Release。
Linux 现在复用 Android 的 SDL3 窗口、输入和设备接口，使用 Panorama。
也可以指定配置、并发数或项目：

```sh
BUILD_JOBS=4 BUILD_CONFIG=debug bash scripts/build-linux.sh
bash scripts/build-linux.sh Client_Panorama_CSGO Server_CSGO
```

需要 `clang`、`make`、`meson`、`ninja`、`pkg-config`、`binutils`、Perl，以及 FreeType、Fontconfig、
SDL3（至少 3.2）、Vulkan 和 X11 的开发文件。树内的旧版 `protoc` 是 32 位 ELF，
`dev` 还需要相应的 32 位 glibc / libstdc++ 运行库。

`scripts/build-linux-deps.sh` 将 DXVK 固定到
`6a0ea561f9add008899680e6c313aa21c151e03e`，复用项目的 D3D9/SDL3 补丁，
安装到 `runtime/linux-sdl3/install/`。这个 checkout 与 Android 依赖目录独立。
SDL3 版本、DXVK 提交与补丁哈希写入 `runtime/linux-sdl3/dependencies.json`。
旧版闭源 Scaleform 依赖 SDL2/ToGL，不再属于 Linux SDL3 客户端构建。

已有的第三方 C++ 库使用旧版 libstdc++ ABI，因此 Linux 构建保留
`_GLIBCXX_USE_CXX11_ABI=0`。Clang 在这里使用 libstdc++。

所有持久修改应放在 VPC 源文件或 `src/devtools/makefile_base_posix.mak` 中。
`*.mak` 是生成文件，手工修改会在重新生成时丢失。CSGO 游戏条件必须在
`projects.vgc` 的项目入口中启用，否则即使传入 `/csgo`，仍会混入其他游戏的源码。

编译设置缓存记录编译器和编译参数；切换编译器或参数会自动重编译。
VPC 的 Clang 对象位于单独的 `src/utils/vpc/obj/Linux/clang-release/`。

## 输出与运行资源

- 启动程序：`game/csgo_linux64`
- 引擎与公共模块：`game/bin/linux64/`
- 游戏模块：`game/csgo/bin/linux64/`

运行程序还需要匹配版本的 `gameinfo.txt`、VPK、地图、字体、shader 和运行库。
源码树没有完整的物理引擎实现，还需要配套的 `vphysics_client.so`。
单纯生成可执行文件并不代表游戏运行验收已经通过。

用户指定的 Steam 历史资源版本：

| 应用 | Depot | 内容 | Manifest |
| --- | --- | --- | --- |
| 730 / 740 | 731 | 公共资源，2019-03-11 | 8269807393781859603 |
| 730 | 734 | Linux 客户端，2019-03-11 | 2907398298184722507 |
| 740 | 740 | 专服，2019-03-08 | 5632566164910295033 |

SteamCMD 放在忽略的 `runtime/steamcmd/` 中；下载保存在其
`linux32/steamapps/content/` 下，不覆盖 `game/` 的编译输出。
已通过 SteamCMD 匿名下载上述 `731` 和 `740` manifest；`734` 返回
`No subscription`，需要有许可的 Steam 账号。`740` 包也包含 `bin/linux64/`
的客户端运行库，包括物理引擎、tcmalloc、FFmpeg 和 ICU。

`runtime/csgo-2019/` 用于组合这两份官方下载和本项目构建结果，
原始下载保持独立保存。资源中的 `steam.inf` 标识版本为 `1.36.8.1`、
`SourceRevision=4987231`、`Mar 11 2019`。

完成构建后，运行脚本首次将资源复制到 `runtime/linux-sdl3/game/`，随后更新其中的
编译产物并在 `dev` 中启动。后续运行复用这个目录：

```sh
bash scripts/run-linux.sh -windowed -w 1280 -h 720
```

脚本默认添加 `-nosteam -insecure -novid -console`，用于不启动 Steam 客户端的本地开发测试，
并设置运行库路径。离线模式跳过 Steam 客户端、控制器和游戏服务器 API 初始化，
本地服务器使用 LAN 模式；个人配置保存在 `runtime/linux-sdl3/game/csgo/local/`。
也可以通过 `USRLOCALCSGO` 指定配置目录。

运行脚本默认让 OpenAL 使用 SteamOS 的 PulseAudio 兼容接口，避免容器内原生
PipeWire 后端连接失败及退出时的 D-Bus 异常；可用 `ALSOFT_DRIVERS` 覆盖。

这份 partner 源码没有完整的大厅、好友和聊天面板，无法直接加载发行版主菜单。
默认入口是和 Android 相同的离线菜单：选图、设置和触控按键。资源从
`android/app/src/main/assets/mobile_ui/` 拷到 `runtime/linux-sdl3/mobile-ui/`，
不改零售 `code.pbin`。局内鼠标左键当作一根手指，用来点按键和拖动视角；
在设置里关闭“显示触屏操作”后恢复普通鼠标。`-nomobileui` 回到控制台入口。
安卓安装包要等这套界面稳定后再重新打包，这次不会装到手机上。

仍可在控制台输入 `map de_dust2`，或直接通过参数启动地图：

```sh
bash scripts/run-linux.sh -windowed -w 1280 -h 720 +map de_dust2
```

`Esc` 可打开控制台；`jointeam 2` / `jointeam 3` 分别加入 T / CT，
`gameui_hide` 返回游戏；`disconnect` 返回本地入口，`quit` 退出。

`CSGO_USE_STEAM=1` 恢复原有 Steam 初始化路径；该路径不属于本轮离线验收范围。

SDL3 平台与游戏运行验收入口：

```sh
# 实际窗口管理器：启动窗口、UTF-8、鼠标、滚轮、焦点和全屏往返。
bash scripts/test-linux-sdl3.sh

# 启动游戏，加载 Dust II，保存画面，检查键盘移动、尺寸变化、恢复和退出。
python3 scripts/test-linux-renderer.py
```

两个脚本均进入 `dev`。游戏脚本使用 X11/XWayland 和 `xdotool`，需要一个可用的桌面会话；
截图转换使用 Pillow。它在 `runtime/linux-sdl3/validation/game/` 保存日志、
实际加载的模块、PNG 截图和 `result.json`，使用独立的本地配置及随机密码网络控制台。
截图中的地图、模型和 HUD 仍需检查画面。`--shaderapi` 可指定待验证的游戏后端模块；
默认是 `shaderapidx9_client.so`，即 SDL3/native DXVK 对照。

基础武器从本地 `scripts/items/items_game.txt` 加载定义，使用现有的物品属性和
网络同步机制取得模型、弹药等数据，不需要在线库存服务。

直接运行可执行文件时使用 `-nosteam` 启用离线模式。此选项关闭运行时的 Steam
客户端连接；构建产物目前仍链接 Steam API 库，后续 Android 移植还需要替换这些依赖。

现代 glibc 会直接调用 `stat64` / `lstat64`，构建必须保留对应的大小写兼容包装。
否则可能出现文件能打开、元数据查询失败的问题。文件长度也改为通过已打开的
文件描述符读取，读取失败时关闭句柄并返回失败，避免使用未初始化的长度。

网络控制台的回归脚本为 `scripts/test-netconsole.py`，检查密码、分段命令、
批量命令及断开重连。需要在游戏启动参数中显式添加 `-netconport 27991` 和
`-netconpassword <测试密码>`，游戏启动完成后在另一个终端执行：

```sh
distrobox enter -n dev -- python3 scripts/test-netconsole.py --password '<测试密码>'
```

网络控制台会监听所有接口，因此请设置测试密码，并在测试结束后用 `quit` 关闭游戏。

## Vulkan 后端与 Android

此处的 native DXVK 是原生 Vulkan 后端的画面对照路径。直接实现 ShaderAPI 的进度与
第二阶段验收要求见 [Vulkan 计划](vulkan-backend-plan.md)。

原生 Vulkan 的游戏模块和首批材质已接通，单独构建后可运行：

```sh
bash scripts/build-vulkan-module.sh
bash scripts/run-linux.sh -shaderapi shaderapivulkan_client.so \
  -nomobileui -windowed -w 1280 -h 720 +mat_queue_mode 0 +map de_dust2

python3 scripts/test-linux-renderer.py --shaderapi shaderapivulkan_client.so \
  --validation --output runtime/vulkan/validation/game-native
```

模块安装到 `game/bin/linux64/`，运行脚本会同步到独立运行目录。
`SOURCE_VULKAN_SHADERS` 默认指向 `runtime/vulkan/shaders/`；运行中不要重建该目录，
游戏会按需读取 SPIR-V。着色器生成使用固定版本 DXC，当前清单共 2053 个变体。

当前支持 Source 整数 HDR 光照与曝光：Dust II 的 HDR 烘焙道具数据、地图、武器/手臂、
天空和 Panorama HUD 已做实际 GPU 验收。显示输出仍是 SDR；未实现 HDR10 输出。
这不代表第二阶段已经完成：基础动态阴影和同场景材质差异仍待验收，
`Character` 暂用 `VertexLitGeneric`，完整涂装/布料/高光、CSM 和复杂效果尚未覆盖。

游戏测试保存截图、模块 SHA-256、进程 RSS 峰值、加载/模式切换耗时，以及原生模块的
上传和查询统计。自动截图检查只判断画面有内容；画质和 HUD 仍须检查截图。
原生测试要求独立输出目录，保留 `runtime/linux-sdl3/validation/game/` 的 DXVK 对照。
输入法拦截桌面按键时可加 `--skip-keyboard`；结果会单独记录跳过的检查，
渲染、缩放、恢复和退出仍正常验收。
`-vulkan-validation` 启用 Vulkan 及同步验证，性能测量应另用关闭验证的固定场景。

Android 已使用 SDL3/native DXVK，其状态见 [Android 计划](android-port-plan.md)；
本轮 Linux 迁移不包含 APK 构建和 Android 真机回归。
