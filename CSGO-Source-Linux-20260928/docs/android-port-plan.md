# Android ARM64 移植研究与计划

2026-09-25。Linux 基线通过，Android 已接入游戏内移动菜单、设置和原生多指触控，真机回归进行中。

结论：可以推进原生移植验证。SDL3 启动器、外置资源和 NDK 工具链有明确路线；
完整可玩版仍取决于 **VPhysics、渲染后端、Panorama** 三项验证。
GTAV 的成功经验能减少 Android 平台工作，但不能证明 CS:GO 的二进制依赖可用。
按用户指定，Android 首期渲染路线确定为 **native DXVK D3D9 → Vulkan，SDL3 管理窗口**。

Linux 完整 Clang 构建及离线运行基线已通过：Dust II 连接、场景/HUD、移动、
武器与伤害、返回控制台及正常退出已验证；用户确认换弹正常。
网络控制台的认证、分段/批量命令及断开重连回归通过。发行版大厅/社交面板仍不在离线基线范围内。

Android 已完成 SDL3 / DXVK D3D9 的 ARM64 构建、APK 打包与安装，以及完整资源同步。
真机为 OnePlus PJZ110，Android 16 / API 36、Adreno 830，系统 Vulkan 1.3.284，支持 BC 纹理。
图形入口已验证原版 VPK 的 87,548 条目录、VTF 的 CRC 和 BC1 上传、Dust II BSP 的 64 个 lump 范围；
SM2 顶点/像素着色器、深度遮挡、透明混合和 GPU 像素回读通过。
**当前 APK 默认调用真实 `LauncherMain`；Debug 真机已载入 Dust II 并连接本地服务端，
游戏内菜单、触屏操作与完整玩法尚未完成验收。**
引擎、matchmaking、`client_panorama`、`server` 均已完成 ARM64 Release 链接，`engine`
动作一次构建全部模块并写入 receipt。真机已通过 D3D9 设备创建和 matchmaking 加载；
VScript ARM64 虚成员指针修复后，client/server 加载也已通过；共享系统依赖中的
`scenefilecache` 已补入构建、receipt 与 APK 必需模块清单。
链接成功不等于进入地图，运行验收以真机日志为准。
同一进程内切后台、恢复 Surface 并再次校验像素、返回键退出均已通过；实测设备使用 4 KiB 页，
16 KiB 当前完成 ELF/APK 静态检查，仍需对应页大小的设备验证。
权限拒绝时禁止启动、资源缺失提示、恢复权限/资源后可启动已通过。图形探针曾验证同进程重开；
真实引擎改为独立 `:game` 进程，退出后创建新进程，避免复用 Source 全局状态。

## 范围与工具链

- 首阶段只做 `arm64-v8a`、本地离线游戏；启动器采用 SDL3。
- 游戏资源根目录固定为 `/storage/emulated/0/Games/CSGO`。
- 构建、生成器、依赖安装和测试统一在 Distrobox `dev` 中进行。
- NDK：`/home/deck/Code/Toolchains/android-ndk`，指向 r30，版本 `30.0.16248370`，内置 Clang 21。
- SDK：`/home/deck/Code/Toolchains/android-sdk`；已安装平台 `android-37.2`、Build Tools `37.0.0`、adb `37.0.1`。
- `dev` 已有 JDK 25、CMake 4.4.2、Ninja 1.13.2、Meson；最低 API 30，target SDK 36，compile SDK 37.2。
- SDL3 固定 `release-3.4.16` / `fa2c02bb6e21974a89ea9824bc53c9932abe5f9c`；Java 支持类和 native 库使用同一源码。
- 使用现有 SDK 的 aapt2、javac、d8、zipalign、apksigner 打包，无 Gradle 依赖；NDK libc++ 在所有模块间共享。

## 当前构建与真机入口

唯一脚本 [build-android.sh](../scripts/build-android.sh) 会自动进入 `dev`：

```sh
BUILD_CONFIG=release bash scripts/build-android.sh engine  # 当前已接通的引擎核心模块
BUILD_CONFIG=release bash scripts/build-android.sh native  # 入口增量编译、打包、验证
BUILD_CONFIG=release bash scripts/build-android.sh install
bash scripts/build-android.sh run
bash scripts/build-android.sh diagnose
bash scripts/build-android.sh symbolize libengine_client.so 0x647d4f
BUILD_CONFIG=debug bash scripts/build-android.sh engine
BUILD_CONFIG=debug bash scripts/build-android.sh native
BUILD_CONFIG=debug bash scripts/build-android.sh install
BUILD_CONFIG=debug bash scripts/build-android.sh run +map de_dust2
BUILD_CONFIG=debug bash scripts/build-android.sh console 'status' 'getpos'
BUILD_CONFIG=debug bash scripts/build-android.sh console   # 交互终端，Ctrl-D 断开
```

`BUILD_CONFIG` 只接受 `release` / `debug`，默认 release；产物分别在
`runtime/android/<配置>/`、`game/bin/androidarm64/<配置>/`，游戏专属模块在
`game/csgo/bin/androidarm64/<配置>/`。Release 保留独立调试符号，关闭 Android
`debuggable`、D3D HUD 和自动控制台文件日志；Debug 开启断言、控制台日志和 HUD。
两种配置均可通过 `run +map de_dust2` 等正常 argv 启动，便于在 Release 上测性能。
脚本通过 JSON 传递参数，保留空格、引号与逗号。
`run` 自动设置随机控制台密码和 ADB 端口转发，`console` 可批量发命令、
读取响应或进入交互终端；不依赖触摸控制台。设备端仅监听 loopback，USB / 已连接的
无线 ADB 使用相同命令，多设备通过 `ANDROID_SERIAL` 选择。会话信息保存在忽略的
`runtime/android/<配置>/netconsole.json`；重启使用 `run`，连接尚未就绪时最多等待 30 秒。
图形探针仅 Debug 可用：`BUILD_CONFIG=debug bash scripts/build-android.sh probe 15`。
两种配置暂用同一开发签名，Release 不代表可发布版本。

`build` 会构建已版本化的依赖配方和引擎，再打包；**全新 checkout 尚不能完整重建**，
V8/字体栈仍依赖现有构建树，待办见下文。`engine` 成功后记录源码指纹和模块哈希；
打包校验记录，明确选择模块及递归 ELF 依赖，拒绝混入 game/bin 的历史库。
编译期间变更源码需重新执行 `engine`，不能为旧库手工补写成功记录。
`verify` 校验 APK 签名、对齐、AArch64、16 KiB 段、Build ID 和依赖完整性。
符号在 `runtime/android/<配置>/symbols/`，APK 内 `assets/build-info.json` 记录每库来源与哈希。
引擎 receipt 使用引擎源码指纹；报错中的构建编号另包含 Android native/Java、打包脚本和配置，
修改启动器也会更新编号。打包同时核对 native 库内的编号，防止只更新 CMake 配置却复用旧库。
独立的 `src/materialsystem/shaderapivulkan/` 由 Android CMake 构建，因此只纳入 APK 指纹，
不使 Source `engine` 的构建记录失效；共享公开头文件仍纳入引擎指纹。

设备日志在 `Games/CSGO/logs/`：`launcher.log`、`stdio.log` 及各自上一轮日志、
`error.txt`、DXVK 日志。`diagnose` 收集日志、按游戏 PID 筛选 logcat 和系统退出原因，
并单独保存 `crash_dump64` 输出的本次游戏进程回溯到 `crash-logcat.txt`，避免 PID 过滤漏掉 tombstone。
Debug 另外开启 `-condebug -conclearlog`，每次启动重建 `csgo/console.log`，
`diagnose` 同时拉取它，保留 JS 报错和地图加载输出；启动参数写入日志时隐藏控制台密码。
设备日志与本地符号构建编号不一致时写入 `symbol-mismatch.txt`，不附上错误版本的构建信息；
`Sys_Error` / `Error` 保存构建编号和模块相对 PC，可用 `symbolize` 对应符号定位。
真正的 native crash 使用 Android tombstone / ApplicationExitInfo，不安装自行处理信号的崩溃拦截器。

多设备时设置 `ANDROID_SERIAL`。资源已同步，不要再次跑容量检查或全量同步。
只有资源改变才用 `sync`；它不删除手机文件，默认源 `runtime/csgo-2019/`，
可用 `CSGO_RESOURCE_SOURCE` 指定。APK 升级独立于资源。

回归：`test-foundation --compare-linux` 已验证手机/Linux 两次文件系统初始化、白名单开启、
异步读写和 SIMD 一致性。`android/tests/parsifal_test.c` 使用 Source 的公开 Parsifal 头，
与 Expat 适配器不同包含路径共同编译，ASan/UBSan 检查流式 EOF、命名空间、CDATA、实体、
错误文档和回调中止；已通过。`test-physics` 在手机上通过公开接口验证碰撞缩放、包围盒、
重心、体积、无单位阻力覆盖比例、描述字节复制和独立释放。所有构建和测试均在 `dev`。
`test-vscript` 使用真实 VScript 公共头，已在 Linux/ARM64 验证静态注册、首个及后续虚表槽的
动态派发、非虚/const/普通函数及空指针往返，覆盖此前 client 静态初始化阶段的 SIGTRAP。
`test-gcsdk` 在 Linux（AddressSanitizer）和手机上验证消息序列化往返、同一数据包重新绑定、
消息池清理、超长/缺失数据拒绝，以及文本/二进制缓冲区内的 JSON 转义；已通过。
同一测试还覆盖空 `LanSearch` 的精确二进制格式、嵌套空 KeyValues 与同级字段，
以及中文宽字符串按 16 位线格式往返；Linux ASan 与手机均通过。

## 游戏内菜单、触控与加载耗时

### 实现与入口

Android 的离线入口使用自有 Panorama 页面，提供开始游戏、设置、暂停、返回主菜单和
退出确认。页面属于游戏客户端的 `CGameUI`，随游戏画面绘制；启动器只处理权限、资源和进程启动。
原版主菜单依赖本树缺失的 `MyPersonaAPI`、`LobbyAPI`、`PartyListAPI` 等业务接口，
此前 `-nosteam` 因而跳过原菜单并打开控制台。新页面复用已有控件、字体和 HUD，绕开这些大厅依赖。

主要入口是 [mobile_gameui.cpp](../src/game/client/cstrike15/mobile/mobile_gameui.cpp)，
资源位于 [mobile_ui](../android/app/src/main/assets/mobile_ui/)，随 APK 更新并解包到应用私有目录。
不需要修改外置 `panorama/code.pbin`。`-nomobileui` 可在 Debug 中回到原离线控制台路径。
Panorama 的 native DXVK 构建须启用 `PANDX_DRAW`，否则页面布局完成后仍可能黑屏；
Android 使用 Surface 请求的横屏尺寸，避免桌面模式枚举失败后退回 640×480 并拉伸。

28 枚动作与交互 SVG 使用统一的 64×64 坐标、3px 主线宽及安全留白；人物动作采用实心轮廓，
开火使用弹药、瞄准使用准星，轻击/重击、上抛/轻抛、开门/关门和鸡跟随/停止跟随分别表达方向或状态。
这些专用图标经过 64px 与 32px 渲染检查；装备剪影继续从 CS 原版资源按真实武器读取。
所有普通图标和文字使用白色，按钮底框使用中性灰；动作反馈也沿用这套颜色。
触屏切枪控件的已装备图标、名称、弹药数字和边框统一使用原版金色 `#EAD18A`，
未选中装备保持浅灰白。武器名称与弹药分成两行，避免长名称与数量重叠。
SVG 只固定纹理高度，宽度按素材比例推导，然后在按钮内等比缩放，避免长枪被压成正方形。
Panorama 的 SVG 子集不继承 `<g>` 的填充和描边，因此本地图标把绘制属性写在每个图元上，
避免浏览器预览正常、游戏内却变成黑块或丢失线条。

| 功能 | 当前行为 |
| --- | --- |
| 主页风格 | 复用原版 Dust II 场景图、CS:GO 标志及 `csgostyles.css` 的颜色定义；半透明深灰面板、白色文字、绿色开始按钮和顶部导航。局内菜单透出当前游戏画面，触屏点击区域保持足够大小。 |
| 开始游戏 | 扫描本地 BSP 地图，提供休闲、竞技和死亡竞赛；可选择机器人数量、难度及 T/CT 队伍。进图使用原版 Panorama 加载画面；失败信息仍回到移动菜单。 |
| 设置 | 音量、菜单音乐、帧率、纹理质量、纹理过滤、阴影、普通视角与开镜独立触屏灵敏度、反转视角、按钮透明度，以及性能数据（`net_graph`，显示帧率与延迟）。应用时保存；返回放弃未应用的草稿，恢复默认也先写入草稿。 |
| 游戏语言 | 默认跟随系统；中文系统使用简体中文，其余系统语言暂用英文。启动器和游戏内设置均可选跟随系统、简体中文、English；选择后立即保存，重启游戏生效。 |
| 触摸 | 左下摇杆、左右开火、右侧滑动视角与跳蹲换弹，底部独立武器栏及投掷物按钮。玩家持有的武器才显示，使用原版武器剪影，显示弹药或数量；当前装备的图标、名称和弹药统一金色。电击枪和匕首、各类投掷物分别选择。蹲下和静步点按切换，开镜点按，投掷/轻抛及近战攻击按住。 |
| 情境交互 | 根据服务端当前可用目标显示拾取、开门/关门、鸡跟随/停止跟随、救援或拆弹。携带 C4、按住安装和按住拆除共用一个位置；安装先选择 C4，再产生攻击输入，松手取消。购买有独立按钮，仅在允许购买时显示。 |
| 后坐力 | 开火通过武器的后坐力表调用 `KickBack`，同时产生弹道偏移、镜头震动和枪模抖动；客户端预测和服务端使用同一实现，松开开火后沿原有衰减逻辑回正。 |
| 购买 | `buymenu` 发送 Panorama 打开/关闭事件；余额和购买区域变化同步到菜单。普通模式显示真实价格，死亡竞赛免费物品显示 `$0`，悬停详情价格一致。保留原有扣款、购买时间、区域和游戏模式限制。 |
| 布局编辑 | 按实际游戏大小展示全部按钮，拖动位置并调整大小和透明度；保存、取消、恢复默认。坐标相对安全区域，适配手机宽屏与平板；布局版本 3 自动升级旧版默认位置，保留手动布局并为新增按钮补默认位置。 |
| 焦点与生命周期 | 菜单和原版弹窗取得焦点时释放游戏输入；局内选边和购买菜单打开时，触控改为界面点击并隐藏虚拟按键，关闭后恢复游戏操作；取消触摸、失焦、切后台、改变尺寸和队列溢出均重置触控，避免持续移动或开火。列表按 `touch-scroll` 类单独启用拖动滚动，保留聊天的文字选择行为。 |

配置保存到 `Games/CSGO/csgo/local/cfg/mobile_ui.vdf`，使用临时文件加重命名更新。
设置同时应用到真实 CVar，执行 `mat_savechanges; host_writeconfig`；布局独立于原版键鼠绑定。
语言单独保存到同目录的 `language.txt`（`auto` / `schinese` / `english`），不存在或无效时视为 `auto`。
启动器按系统语言解析 `auto`，通过 `-language` 传给引擎；显式传入的 `-language` 优先。
引擎、VGUI、本地化资源路径和移动菜单读取同一次启动的语言；移动菜单的静态文字、动态选项及
触屏提示使用随 APK 分发的 `menu_english.txt`。语言选择不属于滑块草稿，返回设置页不会撤销它。
`mobileui_language` 可查询或保存下一次启动的语言；`-devcvars` 下的 `mobileui_buy_prices`
输出实际购买面板标签和玩家余额，支持不截图的语言与价格验证。
启动参数修改会重新分配 `CommandLine()` 的字符串；`CEngineAPI` 现在先持有启动目录的独立副本，
避免 `-nosteam` 补写 `-insecure` 后继续使用已释放的游戏目录，导致随机的 `gameinfo.txt is missing`。
普通视角灵敏度为 0.25～3.00×，开镜灵敏度为 0.05～3.00×，两项均以 0.05× 调整，
默认分别为 1.00× / 0.50×。开镜值独立使用，不与普通值相乘；仅在存活玩家实际开镜时切换。
原有 `sensitivity` 配置保留为普通视角，旧配置缺少 `scoped_sensitivity` 时沿用原值，
应用保存后两项独立持久化。陀螺仪继续使用单独的陀螺仪灵敏度。
“3D 渲染比例”只控制 `mat_viewportscale`（50%～100%，默认 75%）：Source 先按比例绘制
场景、武器和后处理，再放大到原视口，最后按原生分辨率绘制 HUD、菜单和触摸按键。死亡定格在这次放大之后抓取，并按未缩放视口回放，避免击杀画面停在左上角。
窗口、Surface、后缓冲和触摸坐标不随该设置改变，应用比例无需重置设备或重新加载纹理。
Android 上该 CVar 不标记为 cheat，避免进入地图时被重置；桌面平台保留原标记。
Android 通过 `IMobileInputSource` 向客户端传递原始多指事件；桌面 launcher 的接口布局保持兼容。
触屏 HUD 的两路输入是 [mobile_hud.h](../src/game/client/cstrike15/mobile/mobile_hud.h) 中的
`PlayerState` 和 `InteractionState`。前者读取本地预测的存活、装备、弹药和动作状态，后者由
[cs_mobile_interaction.cpp](../src/game/server/cstrike15/cs_mobile_interaction.cpp) 按原有使用规则查询，
每 50ms 更新一次，只有开启 `cl_mobile_context` 的玩家接收自己的目标句柄和交互类型。
客户端与服务端需要配套更新；普通键鼠客户端不启用这项查询。查询本身不触发使用或拾取失败事件。
按钮隐藏、禁用或绑定目标变化时，取消对应手指并等待抬起；死亡、换队、切后台和打开菜单会释放输入。
隐藏按钮不占触摸区域。武器选择使用实际实体句柄，避免 `slot3` 轮换到电击枪或 `slot4` 轮换错投掷物。
HUD 的 `equipped` 表示实际持有在手的装备，`active` 表示开镜、蹲下或安装/拆除等动作状态；
切枪成功后才转移金色，按住、暂时无法切换以及 C4 从选择变为安装时均保留正确的装备标识。
布局编辑的选择框独立于局内装备选中状态。`mobileui_status` 输出各按钮的实体、弹药、
选中状态、最终图标路径及触摸区域，供真机输入检查使用。
原版武器 SVG 位于 `materials/panorama/images/icons/equipment/`，通过 Panorama 的 `{images}` 读取，
按真实物品定义选择剪影；只有移动、跳蹲等动作和门/鸡等交互使用随 APK 提供的专用矢量图标。
APK 打包时生成 UI 资源清单，启动时连同图标子目录一起原子解包。
中文字体由现有 Valve 解码器解密 `.vfont`，缓存为应用私有 TTF，再注册到 Pango/fontconfig；
缓存键包含文件名、大小和修改时间。触控按钮在位置、大小或透明度改变时更新样式；窗口或 Surface 尺寸变化后会连续重放若干帧，避免 Panorama 重建样式后按键叠回默认流式布局。竖屏、方形和过窄的安全区不会改写已有布局。

### 回归入口

```sh
BUILD_CONFIG=release bash scripts/build-android.sh test-mobile
```

该入口在 Linux 上以 ASan/UBSan、在手机 ARM64 上运行相同的触摸与 HUD 模型测试，覆盖三指组合、
按键归属、短按、取消、重复事件、安全区域、独立道具、数量变化、交互目标序列号、安装/拆除、
观战和 64 位按钮掩码；检查菜单 JS/XML 与图标资源，并准备真实 Android 多指事件注入器。
本地存在游戏 VPK 时，还检查引用的原版武器图标和主页场景图确实存在。
测试手势的 JSON 送到 `/data/local/tmp/gesture.json` 后可执行：

```sh
adb shell 'CLASSPATH=/data/local/tmp/csgo-touch.dex app_process /system/bin com.csgosource.tests.TouchInput /data/local/tmp/gesture.json'
```

手势由 `down/move/up/cancel`、手指 `id`、屏幕像素 `x/y` 和毫秒 `wait` 组成，必须释放全部手指。
只在游戏位于前台时注入。布局、设置保存和真实进图还需通过设备交互验收，不能以模型测试代替。

[触屏装备检查](../scripts/test-android-touch.py) 使用真实 Android 触摸事件，验证独立切枪、
投掷物数量和耗尽隐藏、按住开火时用第二根手指切枪，以及蹲下和菜单切换时的输入释放。
保存每次切换的 HUD 状态与截图，供检查原版图标比例及图标/名称/弹药的金色反馈。
先运行上面的 `test-mobile` 准备注入器并安装相应 APK，再执行：

```sh
python3 scripts/test-android-touch.py --run --config release --timeout 240
```

无 `--run` 时只打印流程。测试使用本地 Dust II 对局并在结束时停止本次游戏进程。

[射击与购买菜单检查](../scripts/test-android-gameplay.py) 默认只打印测试流程，不连接设备。
显式加 `--run` 才启动已安装的 APK，核对构建编号，在 Dust II 检查连射后坐力、回正及
购买菜单打开/关闭，并保存数据和截图；成功、失败或超时都会停止本次启动的游戏。
默认总测试时限为 120 秒，最后停止应用与移除端口转发另有最多 8 秒的清理时限。
脚本不安装 APK；枪模画面、菜单布局和真实触屏点按仍需结合截图及真机检查。

```sh
python3 scripts/test-android-gameplay.py
# 手机空闲且已安装对应构建时才执行：
python3 scripts/test-android-gameplay.py --run --timeout 120
```

### 已测到的加载热点

2026-09-24 在 PJZ110 上检查 Debug 构建 `0df0ae2ec4eab2212ee5`：
一次启动到控制台可回应约 **17.9 秒**；另一次从控制台执行 `map de_dust2`，
加载状态到游戏中状态约 **93.2 秒**，`status` 确认本地 loopback 连接。
进图前约 45 秒以 simpleperf 的 `cpu-clock:u` / 400 Hz 采样，记录 22,346 个样本、无丢样；
**`MD5Transform` 占 66.06% 的 CPU 样本**。这是该阶段的 CPU 占比，不是总加载时间占比，
也不是 Release 的性能结论。第二次进图复测遇到 APK 被另一轮安装替换，跨进程耗时已作废。

调用栈包含同步的 `CFileTracker2::RecordFileRead → TrackedFile_t::ProcessFileRead → MD5Update`，
以及后台 `ThreadedProcessMD5Requests`。原因和优化顺序：

1. [sys_dll2.cpp](../src/engine/sys_dll2.cpp) 的客户端初始化按 `gameinfo.txt` 的多人类型启用
   whitelist 文件追踪，未区分 `-nosteam`；读取资源时仍在计算 pure-server 所需的 MD5。
   VPK 读缓存另有后台块校验，[basefilesystem.cpp](../src/filesystem/basefilesystem.cpp) 在挂载
   VPK 时直接注册 tracker。离线策略需要同时审查两条路径；只改 `sv_pure 0` 或关闭
   whitelist 追踪不能自动去掉所有块校验。保留必要的损坏检测与联网校验语义。
2. Debug 使用 `-O0`，Release 使用 `-O2`。先对同一源码的优化构建做性能验收，再决定
   离线追踪、重复校验和缓存策略；不要用 Debug 的耗时推断最终体验。
3. 之前一轮日志中，`dt_encode.cpp:306` 的字段范围断言有 379,308 次，两个位缓冲区对齐
   断言合计 207,116 次。`launcher.log` 约 40 MB，`stdio.log` 约 100 MB；native 日志逐条
   `fflush`，stdout/stderr 无缓冲，`-condebug` 还会写控制台文件。需定位断言来源并控制重复
   输出；`Invalid blend mode (0)` 在该轮只有 48 次，不是日志膨胀的主要来源。
4. DXVK 已读取 539 条有效 state-cache 记录，不能归因为完全没有着色器缓存；同一阶段
   驱动编译库也有 CPU 开销，后续应区分 state cache 与驱动管线编译，再测冷启动/热启动。

另外，当前 Pango/fontconfig 库没有导出 `*_ft2_new_face_substitute`，运行日志也出现对应
断言及字体回退。新菜单应先修好字体栈；日志中 `FcConfigAppFontAddDir` 的几毫秒仅测量
最后的 fontconfig 调用，不包含前面的全部 vfont 读取/解码，不能当作字体初始化总耗时。

采样、符号缓存和原始日志保存在忽略目录 `runtime/android/debug/ui-investigation/`；
上面的耗时是修改前 Debug 基线。当前 Android `-nosteam` 已关闭逐次资源读取的 whitelist 追踪，
保留 VPK 块校验；Debug 可用 `-trackfilehashes` 显式恢复追踪。Release 对比须单独记录，
不能将编译优化与文件追踪策略的收益混为一谈。

### 帧率采样

`scripts/android-perf.py` 自动进入 `dev`，按配置选择手机 Surface，统计本轮新呈现的帧。
采样时若 Surface/进程改变或 SurfaceFlinger 的历史记录被覆盖，直接判定该轮无效。
GPU 占用率与频率作为运行条件记录，不能换算成 GPU 时间戳测得的帧耗时。
`--output` 保存构建编号、场景、原始呈现时间戳和温度/频率样本，供同场景对照：

```sh
python3 scripts/android-perf.py --config release --seconds 15 \
  --cmd 'mat_viewportscale 0.75' --output runtime/android/release/perf-75.json
```

比较渲染比例时保持原生窗口、相同相机/画质/机器人数量，分别测 100%、75%、50%，
最后再测一次 100% 检查温度和频率漂移。`--map de_dust2` 可另外测启动和进图时间。

## 借鉴 GTAV 的启动与加载

参考其当前实际游戏入口，而不是仅创建 NativeActivity 的旧 bootstrap：

| 参考文件 | 采用的设计 |
| --- | --- |
| [GameMenuActivity.java](../../GTAV-Source-Linux/android/app/src/main/java/com/gtavsource/android/GameMenuActivity.java) | 先处理文件权限，实际检查目录读写，再进入游戏；资源问题在加载 native 模块前给出明确提示。 |
| [GTAVActivity.java](../../GTAV-Source-Linux/android/app/src/main/java/com/gtavsource/android/GTAVActivity.java) | SDL Activity 管理 Surface、游戏线程及前后台切换；暂停时释放按键、触摸和震动状态。它目前加载 SDL2，不能直接充当 SDL3 的 Java 层。 |
| [android_entry.cpp](../../GTAV-Source-Linux/src/dev_ng/game/Core/android_entry.cpp) | native 入口集中设置资源根目录、配置、日志和缓存，再调用原有引擎入口；不重复建立 SDL 生命周期。 |
| [build-gtav-android.sh](../../GTAV-Source-Linux/scripts/build-gtav-android.sh) | 在 dev 内构建/打包，检查 ARM64 ELF、依赖、入口符号、16 KiB 对齐和签名；保留未剥离符号供崩溃定位。 |
| [AndroidDxvkGplall.cmake](../../GTAV-Source-Linux/src/dev_ng/cmake/AndroidDxvkGplall.cmake) | 固定 DXVK 提交及必要 Android 补丁。当前 GTA 配置启用 D3D11/SDL2、关闭 D3D9/SDL3；CSGO 需要自己的 D3D9/SDL3 构建配置。 |

CSGO 当前顺序：权限与资源检查 → 同版本 SDL3 Java/native 层 →
`SDL_main` → Android 平台初始化 → 复用 `LauncherMain` / 引擎启动流程。
启动参数集中生成，包含 `-game csgo -nosteam -insecure -novid`，开发阶段保留控制台。
SDL3 可使用固定版本的官方 Android 工程或 AAR；只选一种集成方式，Java/JNI 与 native 版本必须一致。

资源目录沿用现有运行树的结构：

```text
/storage/emulated/0/Games/CSGO/
  csgo/                  gameinfo.txt、VPK、maps、materials、scripts 等
    local/               本地个人配置，USRLOCALCSGO 指向这里
  platform/              配套公共资源
  cache/                 可重建缓存
  logs/                  当前及上一轮日志
```

这个路径用于游戏数据。原生 `.so` 随 APK 的 `lib/arm64-v8a/` 安装，由系统加载器加载，
不能把 PC 的 `bin/linux64` 原样复制到共享存储后执行。Android 共享存储的执行限制和
linker namespace 需要把模块路径与资源路径分开；现有 `Sys_LoadModule` 搜索规则也要适配。
APK 升级只更新程序，不重新复制整套资源。

API 30+ 的固定共享目录访问沿用 GTAV 的“所有文件访问权限 + 实际读写检查”路线。
只授予媒体权限不足以访问 VPK/BSP；SAF 的 `content://` 也不能直接传给现有 `fopen`。
权限拒绝或文件缺失时停在启动器；日志在共享目录不可写时回退到应用私有目录。

## 依赖与主要难点

主机上的 VPC、protoc、Clang 等构建工具继续运行 x86_64 版本。
需要重编译的是进入 APK 的目标 `.so` / `.a`；普通 VPK、BSP、MDL、VTF 数据不因 CPU
架构变化而整体转换，但仍须验证文件格式、结构体布局和物理碰撞数据。

| 项目与已发现的证据 | 处理路线与验证重点 |
| --- | --- |
| **VPhysics**：已导入 Kisak 实现和 source-physics IVP/Havana，使用本树接口完成 ARM64 Debug/Release 编译；接口为 `VPhysics031`。 | 优先评估可重建的 Source 物理实现。候选 nillerusr 分支也声明 031，但版本字符串相同不保证虚表/结构体相同。逐项比对本项目头文件，验证 `VCollideLoad`、BSP/MDL 碰撞、玩家控制器、投掷物和布娃娃。无法兼容时评估 Jolt 适配层；物理行为变化必须与 Linux 对照。 |
| **渲染，最高优先级**：`shaderapidx9 → ToGL → 桌面 OpenGL`，Panorama 还链接 X11/GL；纹理使用 DXT/S3TC。 | 采用 native DXVK D3D9 + SDL3/Vulkan，保留上层 D3D9 渲染和着色器资源，替换 ToGL 平台连接。先验证 D3D9 ABI、ToGL 特有调用、着色器、纹理和 Android WSI；具体边界见下节。 |
| **Panorama**：已编译 V8 5.8.283 ARM64、Panorama 和 Pango 文本模块；V8 仍依赖旧 NDK 构建树，UI 尚未运行验收。 | 获取并固定对应源码，用 NDK 重建 V8/ICU 和无 X11 的字体/绘制栈；验证 JS 绑定、JIT、中文字体与线程模型。不能只换成新版 V8 二进制。必要时另行评估精简 HUD，但它属于功能取舍，不能假定关闭 Panorama 就能保留现有界面。 |
| **Steam / GC / SDR**：当前 Android 核心及 matchmaking 已启用 `NO_STEAM`，ELF 不依赖桌面 Steam/SDR 库。 | Android 编译期隔离相关服务和回调，保留 UDP/loopback、本地配置、基础物品；使用明确的离线接口，不依赖桌面 Steam 库。最终检查整个 ELF 依赖链。 |
| **SSE、汇编与线程**：`ssemath.h` 大量使用 `_mm_*`，构建存在 `-march=nocona`、`-mtune=core2` 等假设。 | 增加 ARM64 平台分支，先以标量参考或经过验证的 SSE→NEON 层恢复行为，再优化热点。核对 NaN、舍入、近似倒数/平方根、非规格化数；计时使用 Android 单调时钟，原子操作明确内存顺序。借鉴 GTAV 的数值对照方法，不能直接套用其 RAGE 数学实现。 |
| **C++ ABI / Bionic / 分配器**：Linux 使用 glibc 和旧 libstdc++ ABI。 | Android 所有目标统一 NDK/Bionic/libc++；跨模块共用一份 `libc++_shared.so`，保持对象与内存的创建/销毁契约。处理 glibc 专有符号、路径包装、pthread 和文件偏移差异；按实际页大小工作。 |
| **可重编译基础库**：树中有 Protobuf 2.5、Crypto++、OpenSSL、zlib、libpng、curl 等源码。 | 逐库固定版本并交叉编译，先处理旧版本的 ARM64/现代 Clang 兼容点。protoc 是主机工具，生成代码与目标 Protobuf 版本要配套；只纳入实际运行需要的库。 |
| **视频、空间音效等额外二进制**：Android 已隔离 `libvideo.so`；Steam Audio beta20 ARM64 官方库已接入并按配套 API 调整调用。 | 首个可玩版本编译期裁剪片头/网页视频及可选空间音效，保留基础混音与 SDL3 Android 音频；后续按需要重建或替换。仅使用 `-novid` 或关闭某个设置，不能消除动态链接依赖。 |
| **SDL2 → SDL3 与移动生命周期**：当前引擎和 UI 使用 SDL2。 | 修改窗口、事件、鼠标/手柄、音频接口和 native handle 获取方式；以 SDL3 Android 生命周期为准。先验证手柄/鼠标键盘，再做最小移动/视角/射击触控；前后台切换必须释放输入并恢复 Surface、音频和渲染状态。 |

物理接口实查：nillerusr/source-engine 的 `ed8209cc35c61fbd8ddff8480962a01c981eef2f` 缺少本树的
碰撞解析重载、球体半径修改、预测接口等，且 `surfacedata_t` 布局不同，不能直接替换。
[Kisak-Strike 的物理实现](https://github.com/SwagSoftware/Kisak-Strike/tree/4c2fdc31432b4f5b911546c8c0d499a9cff68a85/vphysics)
更接近 CS:GO，但仍缺 `DuplicateAndScale` / `GetMaterialIndexDataOps`，碰撞集合 ID 类型和表面数据布局也有差异；
其预测、替代重力、部分休眠/延迟销毁方法仍为占位实现。可参考其接口适配与 nillerusr 的 ARM 支持，
本树已补充接口并编译，但预测、packed collision、BSP/MDL 物理行为仍需验收，不能以导出 `VPhysics031` 代替兼容验收。

## 审查后的待办与依赖来源

本次审查已替换临时手写 XML 解析器：Parsifal 输入回调的 `BIS_EOF=1` 允许最后一块
仍有数据，`XMLParser_Parse` 成功返回 1，不能与回调的 `XML_OK=0` 混用。现由 Expat
验证 XML，仅适配布局/SVG 实际使用的 SAX 接口；不声称实现完整 Parsifal API。
同时修复 SDL3 rumble 成功判断、手柄 mapping 释放、字体匹配空指针、鼠标小数增量，
并保留 Linux SDL2 的类型接口。

已经定位并修复的启动问题：Android 使用 Surface 呈现，不切桌面独占显示模式；
文件系统 Shutdown 后重置白名单状态并允许哈希线程重启；`Sys_Error` 写入错误记录，
不弹桌面式阻塞对话框、不人为解引用空指针；Source 的 `RUN_OK=3` 在入口转换为进程成功码，
`ModInit` 失败不再伪装成正常退出。Release 与 Debug 共用这些必要错误记录。
已修复禁用视频时 Panorama 播放器指针未初始化，以及空 `LanSearch` 序列化解引用
空子节点的两次实际崩溃。native DXVK 在设备创建/重置成功后通知 VGUI 渲染尺寸，
SDL 窗口坐标按实际 backbuffer 缩放，鼠标回置执行反向转换。
Android 字体模块改用随包 Pango/GLib 的目标头文件；旧的 Windows `glibconfig.h`
会在 ARM64 把 `GType` 等指针宽度类型声明为 32 位，现有编译期检查会拒绝混用。

真机画面问题的结论：
- **阳光下条纹/摩尔纹**：CSTRIKE15 把 SM3 显卡的 dxlevel 上限定为 95，`SetupHardwareCaps`
  原先在所有 POSIX 平台按 95 强制降级 caps（为 ToGL 设计），DXVK 伪装的 AMD 因此被当成
  非 DX10 的 ATI，CSM 走 DF16 + ATI Fetch4 着色器；DXVK 只在点采样时启用 Fetch4，
  实际双线性采样返回单个深度，逐分量比较得到按 texel 小数位置变化的条纹。现在仅 ToGL
  降级，DXVK 与 Windows 一样保留真实 caps（D16 + 硬件 PCF）。Adreno 830 的 depth bias
  单位、比较精度、斜率偏移均已用独立 Vulkan 探针实测与规范一致。
- **彩色拉伸三角形**：非渲染线程创建的网格需要 `HandleLateCreation`，DXVK 与 ToGL 同样启用。
- **切后台**：Activity 停止时 SDL 先置最小化，引擎按桌面失焦停用，后台只模拟不绘制（CPU 约 5%）。
  DXVK 下 Vulkan 设备和资源在 Surface 销毁后仍然有效，`CheckDeviceLost` 不再因最小化释放资源
  （原先释放全部纹理，恢复需约 10 秒），现只重建交换链，恢复首帧约 0.5 秒。从竖屏桌面返回时
  Surface 会短暂为竖屏尺寸，DXVK 在 SUBOPTIMAL 时比较 Surface 尺寸，变化即重建交换链。
- **CSM 渲染顺序**：`cl_csm_before_main_view`（Android 默认 1）在主视图首个 pass 前绘制阴影深度，
  天空盒与世界共用一个 tile pass；GPU 锁 389 MHz 实测每帧约快 0.5 ms。
- **CSM 实例化原型**：`r_csm_instancing` 默认 0。de_inferno CT 机位每帧减少 3057 次模型阴影 draw，
  保留全部三角形；满频短测约 76.2→83.3 fps，冻结状态的最终画面逐像素相同。
  `r_csm_profile` 通过实际 GPU 时间戳测量帧/CSM 区间；开启实例化后约 3.6 ms 在 CSM、8.4 ms 在其余绘制。
  方法、原始记录、验证范围及诊断开关见 [CSM 实例化与 GPU 开销](android-csm-instancing.md)。
- **CSM 基础视锥剔除修复**：`CVolumeCuller::IsValid()` 漏判基础视锥，导致部分级联跳过 CPU 剔除。
  修复默认开启，同 APK、1100 MHz、de_inferno CT 机位的默认路径约 75.25→99.70 fps，
  模型阴影三角形约 289 万→43 万。见 [根因与实测](android-csm-culling.md)；后续重点转向主场景。
- **主场景后续试验**：暂缓 DXVK 隐式提交没有稳定的整帧收益，保持关闭。
  已确认主场景为单采样、普通不透明材质的深度状态正常；底层道具深度排序原型已构建，
  六轮 A/B 约 109–110 fps，片元调用变化不足 0.2%，默认关闭。见 [记录与复现](android-main-rendering.md)。
- **焦点**：截图浮层、通知栏、小窗只改变焦点，Surface 仍在；Android 上初始化后失焦只清空
  按键状态，不停用应用/鼠标，`IsIconic` 只在最小化、隐藏或没有 native window 时成立。

RenderDoc（仅 Debug，手机端回放，避免 GPU 差异）：Android 16 不允许从单独 APK 加载
调试层，需把层打进游戏 APK：

```sh
CSGO_ANDROID_VK_LAYER=$(bash scripts/renderdoc-android.sh layer) \
  BUILD_CONFIG=debug bash scripts/build-android.sh native   # 层与手机上 RenderDoc 版本一致
BUILD_CONFIG=debug bash scripts/build-android.sh install
bash scripts/renderdoc-android.sh start +map de_dust2
bash scripts/renderdoc-android.sh capture runtime/android/debug/captures/frame.rdc
bash scripts/renderdoc-android.sh replay runtime/android/debug/captures/frame.rdc analysis.py
bash scripts/renderdoc-android.sh off   # 装回无层 APK 前必须关闭，否则 vkCreateInstance 失败
```

`replay` 在手机上打开抓帧并执行 qrenderdoc Python 脚本（可用 `controller`、`log`）；
DXVK 使用 dynamic rendering，动作的输出目标为空，需用 `GetUsage` 和管线状态定位。

以下属于较大工作，不能用“能链接”代替完成：

| 项目 | 后续方案与验收 |
| --- | --- |
| 客户端/服务端 | 已链接，Debug 真机已载入 Dust II 并建立 loopback 连接。Steam 好友/头像/工坊/统计在 `NO_STEAM` 下编译期隔离，本地身份与 universe 统一取 `ClientSteamContext()`。本地物品、HUD 与完整玩法仍需验收。 |
| GCSDK | 桌面只有预编译 `gcsdk_client.a`，无源码。Android 由 `src/gcsdk/gcsdk.vpc` 从源码构建客户端子集：protobuf 消息/池、共享对象与缓存、job 类型注册、emit 组、`CSteamID` 字符串、`EmitJSONString`。**没有 job 调度器/协程和 GC 传输**，`BYieldingWaitOneFrame` 断言并返回失败；若以后要连 GC，需要补完整 `CJobMgr`。 |
| VPhysics | 继续验证 compact surface 的节点/三角形偏移、packed collision、碰撞序列化与释放；验证缩放、质量惯量、BSP/MDL、预测、玩家控制器、投掷物和布娃娃。导入实现中的空方法逐项对照真实调用。当前范围检查不等于完整二进制格式验证。 |
| V8 构建 | 固定 V8 5.8.283 及 DEPS，保存 GN 参数，整理旧 NDK/host clang/Python 2 的可重建工具链；只在 `dev` 工作，不通过伪造 `libtinfo.so.5` 符号链接或修改用户 HOME 解决 ABI。现有 `v8` 动作只支持已有构建树增量构建。 |
| 字体/UI 栈 | 把已有 Meson 构建树的交叉配置、subproject 版本、patch 和依赖顺序纳入正式配方。Pango/fontconfig 的 `*_ft2_new_face_substitute` 是 vfont 解密入口，不能直接换成普通发行版库或删掉。核对头文件/库 ABI、字体路径、中文布局及 V8 JS/JIT。 |
| 菜单与触控 | 离线代码跳过主菜单，发行版脚本依赖缺失的大厅接口；优先复用 Panorama 编写移动端页面，补原生多指输入与布局保存。具体范围见本文“游戏内菜单、触控与加载耗时”。 |
| protoc | `protobuf_builder.vpc` 原先只给 `$LINUX` 生成 `.pb.cc/.pb.h`，Android 实际复用了 Linux 构建留下的生成文件；已改为 Android 同样调用主机 protoc。 |
| 构建复现 | 当前 SDL/DXVK、sse2neon、protobuf、Crypto++、curl/TLS、JPEG、FreeType、libpng、Parsifal adapter、Steam Audio 有脚本；V8/UI/OpenSSL 等仍有预先构建输入。收齐配方后在干净目录验收两种配置，补齐新依赖来源和随包许可文本。 |
| 生命周期 | 小窗切回后后缓冲变为 640x480、比例错误，尚未复测。短暂的错误窗口尺寸不再把触控布局钳成一堆。 |
| 默认画质 | 资源同步排除了 `bin/`，手机缺少 `bin/dxsupport.cfg`，没有按显卡的推荐配置；`config.cfg`/`video.txt` 曾出现 0 字节。 |
| 加载与 Debug 断言 | 进图 CPU 采样的主要热点为资源 MD5，需审查离线文件追踪与 VPK 块校验，并用优化构建复测。`dt_encode.cpp` 字段范围、`bitbuf.cpp`/`newbitbuf.cpp` 对齐与渲染线程断言须定位来源，控制重复日志。 |

固定来源（均位于忽略的 `runtime/android/deps/`，不要仅依赖临时目录）：

| 依赖 | 修订 |
| --- | --- |
| Kisak vphysics | `4c2fdc31432b4f5b911546c8c0d499a9cff68a85` |
| source-physics IVP/Havana | `47533475e01cbff05fbc3bbe8b4edc485f292cea` |
| minimp3（Linux/Android 的 MP3 解码模块 `vaudio_minimp3`，CC0） | `ea99364f61c14656440e8d77e9c233ccf3124633` |
| V8 / build | `eda659cc5e307f20ac1ad542ba12ab32eaf4c7ef` / `c7c2db69cd571523ce728c4d3dceedbd1896b519`；16 KiB 链接补丁见 `android/patches/v8-build-android.patch` |
| GLib / Pango | `d40f72e98e4734ba826ba9a278814530720ba760` / `25c27f452294f84044d5cc9f23b637193c7b4421` |
| fontconfig / HarfBuzz | `7861a719616b4b132b9cac089c6c64f47832edb1` / `894a1f72ee93a1fd8dc1d9218cb3fd8f048be29a` |
| Cairo / Pixman | `200441e6855854eb4dbf338e44d67b00ababe07f` / `37216a32839f59e8dcaa4c3951b3fcfc3f07852c` |
| FriBidi / Expat | `b54871c339dabb7434718da3fed2fa63320997e5` / `fa75b96546c069d17b8f80d91e0f4ef0cde3790d` |
| OpenSSL | `c523121f902fde2929909dc7f76b13ceb4961efe` |
| Steam Audio | 官方 `steamaudio_api_2.0-beta.20.zip`，SHA256 `284b7d3b9a5ee744951c9138835c342a93bb2b08a43a253f54956ab6ff70fdd6`；与 beta20 头/API 配套，ARM64 库 64 KiB 对齐 |

上述 UI 仓库的 dirty 状态主要来自 Meson 下载的未跟踪 subprojects/锁文件，未发现
fontconfig/HarfBuzz 根仓库已跟踪源码有未保存修改；定制功能来自固定的 fork 修订。
V8 build 仓库确有链接参数修改，现已保存为补丁。

## DXVK 主路线

目标链路：CSGO / Panorama 的 D3D9 调用 → ARM64 native DXVK → Vulkan → SDL3 Android Surface。
这能利用已有的 D3D9 状态管理及 shader bytecode，也能复用 GTA 已处理过的 Android
Vulkan 加载、Surface 重建、呈现等待与缓存问题，省去另建 ToGL/GLES 后端的工作。

- **依赖构建**：参考 GTA 固定的 `Digger1955/dxvk-gplall` 提交
  `6a0ea561f9add008899680e6c313aa21c151e03e`，先评估其 D3D9 与 SDL3 组合；
  目标配置启用 `enable_d3d9`、`native_sdl3`，关闭 `native_sdl2`。
  GTA 现成的 D3D11 `.so` 不包含所需 D3D9 模块。只迁入当前需要的 Android 补丁，
  RAGE 专用互操作和性能实验逐项判断，避免整套复制。
- **引擎接入**：在 `shaderapidx9` 的 D3D9 设备创建与类型边界接入 native 接口，
  核对 COM 调用约定、结构体、窗口句柄与扩展能力；隔离 `gGL`、ToGL 扩展字段和
  GL 专用资源操作。需要独立的渲染选择，不能仅把 `libtogl` 的名字换成 DXVK。
- **Panorama**：[ipanoramaui.h](../src/public/panorama/source2/ipanoramaui.h) 已暴露
  `GetD3Device` / `SetD3Device`，[source2surface.cpp](../src/panorama/source2/renderer/source2surface.cpp)
  也包含 D3D9 路径，优先让 HUD 与场景共享同一设备。Linux 独立 OpenGL/X11 分支仍需裁剪或适配。
- **SDL3 / Vulkan**：采用 SDL3 原生 WSI，整个进程使用同一 SDL3 实例和同一 Vulkan loader；
  GTA 的 SDL2 句柄查询及 JNI 代码需要对应改写。验证切后台、锁屏、Surface 销毁/重建、
  尺寸变化及 Present 恢复，最初使用系统 Vulkan 驱动。
- **已遇到的平台差异**：显式设置 `DXVK_WSI_DRIVER=SDL3`；Android 持续返回 `SUBOPTIMAL` 时保持有效交换链，
  D3D9 的帧时序也按有效图像处理，避免每帧重建。SDL3 的应用生命周期事件只发给 event watcher，
  不进入 `SDL_PollEvent` 队列；恢复时依据生命周期代数和 native window 重建图形资源。
  启动脚本使用与桌面图标一致的 MAIN/LAUNCHER Intent，避免恢复时新建一个启动器遮住游戏 Activity。
- **纹理与着色器**：保留并验证 D3D9 shader bytecode → SPIR-V 路径；BC/DXT 为发行格式，
  GPU 缺少格式支持时直接报错，不做 CPU 解码回退。可选离线 ASTC 包经 DXVK 加载，见
  [ASTC 资源包](astc-resource-pack.md#游戏内加载)。检查深度、sRGB、透明混合、光照和 HUD 合成。

首个渲染探针应独立于完整游戏：SDL3 窗口、DXVK D3D9 设备、一个真实 shader 和材质、
深度/混合与前后台恢复。通过后再接入完整地图，后续性能工作以实际帧时间和内存数据为准。
DXVK 解决图形 API 转换，VPhysics、ARM64 数学/线程、V8 和 Steam 编译期隔离仍是独立任务。

## 构建组织与推进顺序

VPC 存在 `ANDROIDARM64` 条件，但现有 Android 生成器面向旧 Visual Studio/Ant 工程，
Linux make 基础规则也仍带 x86 和 Steam Runtime 假设。建议保持 Linux 构建稳定，
Android 使用 NDK + CMake/Ninja 入口，并复用 VPC 的文件清单/依赖信息；先用小模块验证
是否扩展其导出，避免人工维护两套庞大的源码列表。Android 输出必须与 Linux 的 `game/` 隔离。

| 阶段 | 完成条件 |
| --- | --- |
| 0. Linux 基线 | 已通过；保留当前离线运行作为对照。 |
| 1. 阻塞依赖验证 | 确认物理候选的完整接口与资源兼容性；验证 V8/字体构建路线和渲染接入探针。任何一项无可用路线时先处理该项，避免直接迁移全部模块。 |
| 2. SDL3 启动壳 | 已通过：真机安装、权限拒绝/授予、资源检查、真实 D3D9 画面、Surface 恢复、正常退出与同进程重开。 |
| 3. ARM64 基础层 | tier0/tier1/mathlib/filesystem 等可交叉编译；真机读取同一 VPK/BSP，验证大小写兼容、偏移、SIMD 和原子操作。 |
| 4. 引擎模块与图形 | 模块工厂加载成功；全部 native 依赖为 Android AArch64，统一 libc++，没有 glibc/X11/Steam 二进制残留；真实材质、光照、模型与 HUD 正确显示。 |
| 5. 本地可玩版本 | Dust II 本地服务端与客户端连接、选队、移动、开火、换弹、投掷物/死亡及退出通过；再验证触控和较长时间运行。 |
| 6. 移动优化 | 基于真机 CPU/GPU、内存和温度数据确定画质/帧率目标；按需优化 NEON、纹理格式、流式加载和着色器缓存。 |

APK 阶段统一检查 ELF `PT_LOAD` 至少 16 KiB 对齐、打包对齐、导出入口、签名和 Build ID；
运行时还要核对 `mmap` 等代码的页大小假设。仅有 ARM64 ELF 或能打开启动器不算游戏移植成功。

## 参考与维护

- [SDL3 Android 官方说明](https://github.com/libsdl-org/SDL/blob/main/docs/README-android.md)：SDL3 Java/native 配套、CMake/AAR、存储路径及生命周期。
- [Source 物理候选](https://github.com/nillerusr/source-engine/tree/master/vphysics) 与 [IVP/Havana 源码](https://github.com/nillerusr/source-physics)：已核对其公开接口也声明 `VPhysics031`，已完成本树接口编译适配，运行行为验收待完成。
- [VPhysics-Jolt](https://github.com/misyltoad/VPhysics-Jolt)：备选。项目说明明确区分 SDK2013/Alien Swarm 与 CS:GO 接口，且部分物理特性仍缺失，不能视为现成替换库。
- 本项目依据：[物理接口](../src/public/vphysics_interface.h)、[Panorama 依赖](../src/panorama/panorama.vpc)、[V8 版本](../src/thirdparty/v8/include/v8-version.h)、[ToGL 纹理格式](../src/togl/cglmtex.cpp)、[平台构建规则](../src/devtools/makefile_base_posix.mak)。

本文件是 Android 唯一计划入口，不为每个阶段另建重复报告。进入实现后按实际需要合并
构建/安装/运行入口；临时探针放忽略目录。计划被正式指南替代时删除本文件，并同步更新
[Agent.md](../Agent.md) 及其他引用，不保留失效文档或脚本。
