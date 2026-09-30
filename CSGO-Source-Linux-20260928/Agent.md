# 项目约定

## 目标

- Clang / Linux 离线基线已通过；当前推进 Android ARM64 移植，保持 Linux 可运行。
- Android 启动器使用 SDL3，渲染采用 native DXVK / D3D9 → Vulkan；资源根目录固定为 `/storage/emulated/0/Games/CSGO`。

## 环境

- SteamOS 宿主不可变；构建、依赖安装、生成器、测试和 `gh` 统一在 `distrobox enter -n dev -- …` 中运行。已有脚本可自行进入容器。
- NDK：`/home/deck/Code/Toolchains/android-ndk`（r30）；SDK：`/home/deck/Code/Toolchains/android-sdk`。以实际安装版本为准。
- 保留容器名：`dev` 为 Arch 主环境；`dev-ubuntu-22-04` 为 glibc 2.35 基线；`dev-debian-12` / `dev-fedora-43` 分别用于 deb / rpm 验收。

## 常用入口

- [构建](scripts/build-linux.sh)：`bash scripts/build-linux.sh`。
- [离线运行](scripts/run-linux.sh)：`bash scripts/run-linux.sh +map de_dust2`。运行目录与构建产物分别在忽略的 `runtime/`、`game/`；游戏运行时不要覆盖运行库。
- [控制台回归](scripts/test-netconsole.py)：用法见 [Linux 构建与运行](docs/linux-build.md)。
- [Android 入口](scripts/build-android.sh)：`engine / native / install / run / console / diagnose / symbolize`；`BUILD_CONFIG=debug|release` 隔离产物到 `runtime/android/<配置>/`。Debug 支持 `run +map de_dust2` 和 `console 'status'`。当前默认启动真实引擎，客户端/服务端已链接，真机地图尚在验收。
- [Android 可行性与计划](docs/android-port-plan.md)：依赖、加载路径及分阶段验收的唯一计划入口。

## 保持简洁

优先更新现有文档和脚本。内容失效、被替代或不再使用时，删除对应文件，并同步删除本文件及其他文档中的链接；不要积累重复指南、临时脚本和逐轮工作日志。临时产物放忽略目录或 `/tmp`。本文件只保留目标、环境与入口。
