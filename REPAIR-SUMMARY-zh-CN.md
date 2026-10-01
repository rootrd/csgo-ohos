# 修复说明与验证边界

源码提交：a5e5fc6c4570d10cfe19edb744ff3c446a97dbf5

## 问题原因与修复方式

### 1. Vulkan 创建设备成功，仍不能证明 D3D9 可正确渲染

原来的回退会清空核心特性或丢弃特性链；即使 `vkCreateDevice` 返回成功，DXVK 的着色器、资源、查询及采样路径仍会使用未启用的能力。32 位诊断 mask 也不能正确表达更高位的必需特性。

修复：按实际 D3D9/DXVK 调用路径建立必需特性集合，只移除已审计为可选的能力；mask 改为严格解析的 64 位请求子集，并在第一次创建设备前应用。每次重试同步重建扩展版本、特性链和实际 Vulkan 请求；成功后立即停止重试，保存真实启用位并清除栈上链指针。缺少必需特性时按名称报错，不再用零特性设备冒充可用渲染器。默认配置不带诊断 mask。

### 2. Panorama 链接到了 V8 符号桩，无法执行 JavaScript

返回零的导出函数可以满足链接，但不能创建 platform、isolate 或 context；后续进入 V8 scope 会使用无效对象。原初始化标记也先于实际成功被置位。

修复：从校验和固定的官方 V8 5.8.283 源码构建 OHOS/musl 真实实现，核对现有头文件，并统一三份 `.cr.so` 的 SONAME/DT_NEEDED。Panorama 在使用对象前检查版本、platform、ICU、初始化、isolate、模板和 context，正确处理 POSIX ICU 路径，并仅在全局初始化成功后记录完成。stage 拒绝旧零返回桩；增加真实运行时探针和初始化失败测试。

### 3. Android Steam Audio 二进制不能作为 OHOS 实现使用

现有 Android/bionic 音频库不能证明兼容 OHOS/musl；伪造函数会使 HRTF 路径进入不完整状态。

修复：仅在 `__OHOS__` 下禁用 HRTF/Steam Audio 遮挡及线程路径，固定相关配置开关；游戏和语音保留原始 wave data 的所有权并走普通混音器，立体声直通正确按双声道帧数拷贝。最终引擎验证没有 `ipl*` 导入或导出。其他平台路径保留。

### 4. 构建依赖缺失且原流程绑定 WSL/Windows

恢复构建需要源码依赖、正确的 protobuf 编译器及官方 OHOS 工具链；仅复用 Android 库或旧 stage 目录不能保证目标 ABI 和依赖闭包。

修复：增加固定版本/哈希的依赖与主机工具恢复、原生 Linux DXVK/V8/引擎构建和隔离的未签名 HAP 打包。修正 Linux 大小写文件引用、支持 `PROTOC` 覆盖、补齐 GLib netlink 检测所需头文件、修正当前 SDK 下启动页状态圆点组件编译问题。stage 强制核对所有模块与真实运行时，再校验最终 HAP 内实际 ELF/API/依赖闭包；NAPI 模块增加 16 KiB 链接对齐。未触碰正常项目的签名材料。

## 已完成验证

- 官方 OHOS SDK native 26.0.0.105 / API 26 / Clang 15.0.4；Hvigor 6.26.8。
- 全量 26 个引擎模块、libmain、NAPI、DXVK、SDL、真实 V8 及源码依赖构建通过。
- 完整 release 未签名 HAP 打包通过；独立复核 ZIP CRC、全部 92 个 AArch64 ELF、所需导出 API、DT_NEEDED 闭包及音频库符号。
- DXVK 主机 ASan/UBSan 回归和相关编译单元语法检查通过。
- Panorama 初始化保护及音频回退主机测试通过。
- 真实 V8 ARM64 模拟器功能探针通过；OHOS ARM64 探针已交叉链接，未在真机执行。

构建记录：`docs/build-evidence/native-linux-2026-10-01.json`

测试包：`csgo-ohos-0.1.0-1000062-arm64-unsigned.hap`

- 字节数：142751955
- SHA-256：`5d9f21722f9f837ba620d9b94c9742166e26fbe9254c45ddd2a76383b379b0a2`

本源码包不包含 HAP、SDK 缓存、外部下载依赖或完整游戏资源包；保留仓库已有的启动资源、依赖源码和许可证。签名配置已改为无签名版本。

## 待真机测试及已知限制

当前修复等待实际设备测试。编译和静态审计通过尚不能证明游戏可玩。

- HAP 未签名，需要测试方自己的授权签名配置后才能正常安装。
- V8 5.8 仍需要运行时可执行内存；HarmonyOS 应用沙箱是否允许当前分配方式尚未验证。未添加权限或降低系统安全设置。
- 当前 V8 配置不包含 Intl；Steam Audio/HRTF 空间音频已关闭。
- Maleoon/其他 GPU 驱动若不能提供渲染所需 Vulkan 特性，将明确失败；尚未验证真实 D3D9 着色器执行、像素读回或画面正确性。
- SDK 的 libc++_shared.so 为 4 KiB ELF 段对齐，整个包的 16 KiB 设备兼容性尚未确立。
- 仍需检查实际设备 API 可用性、Panorama 资源与菜单、输入/返回/重入、地图加载、音频及游戏流程。

复现与验收说明：

- `docs/NATIVE-LINUX-BUILD.md`
- `docs/PANORAMA-RUNTIME.md`
- `deps/dxvk-ohos-legacy/tests/vulkan/README.md`
