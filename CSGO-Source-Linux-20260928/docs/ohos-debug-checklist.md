# CS:GO 鸿蒙真机联调检查清单

> 创建：2026-09-28。阶段6 真机联调准备文档。
> 用法：按顺序逐项验证，每项标注通过/失败/待测。

## 1. Vulkan 上屏链路

### 1.1 SDL3 → Vulkan Surface 创建
- [ ] SDL3 OHOS 后端 XComponent surface-created 回调触发
- [ ] `SDL_Vulkan_CreateSurface()` → `vkCreateSurfaceOHOS()` 成功
- [ ] Vulkan instance 创建成功（`vkCreateInstance`）
- [ ] Vulkan physical device 枚举到 Maleoon GPU

### 1.2 DXVK D3D9 → Vulkan 翻译
- [ ] `Direct3DCreate9()` 成功（d3d9.so 加载）
- [ ] `DXVK_WSI_DRIVER=SDL3` 环境变量生效
- [ ] D3D9 device 创建成功（`IDirect3D9::CreateDevice`）
- [ ] Swapchain 创建成功（DXVK 内部 Vulkan swapchain）
- [ ] 首帧 Present 成功（`FIRST_FRAME_PRESENTED` 日志）

### 1.3 引擎渲染管线
- [ ] MaterialSystem CONNECT 阶段成功
- [ ] ShaderAPIDX9 初始化成功
- [ ] 首个世界帧渲染成功
- [ ] HUD/Panorama 2D 层显示

## 2. Maleoon 专项

### 2.1 Quirk 自动检测
- [ ] DXVK 检测到设备名含 "Maleoon"（`dxvk_shader.cpp:82`）
- [ ] Cube Dref 坐标补齐生效（`padCubeDrefCoordinates=true`）
- [ ] CubeArray Dref 模拟生效（`emulateCubeArrayDref=true`）
- [ ] 单样本 A2C 禁用生效

### 2.2 dxvk.conf 配置
- [ ] `d3d9.maxFrameLatency=1` 生效（输入延迟最小化）
- [ ] `d3d9.kisakEvictManagedOnce=True` 生效（省 ~400MB PSS）
- [ ] `dxvk.kisakFreeEmptyChunks=True` 生效
- [ ] `dxvk.kisakChunkSizeMB=32` 生效

### 2.3 已知风险（SourceOH 经验）
- [ ] 上屏率验证（SourceOH 遗留 ~1-5fps 问题，DXVK 路线可能不同）
- [ ] vgui 2D 层显示验证（D3DCOLOR 顶点属性 UB4 翻译路径）
- [ ] BC1-BC7 纹理压缩格式支持

## 3. 引擎启动链路

### 3.1 SDL3 dlopen libmain.so
- [ ] `libmain.so` 存在于 HAP libs/arm64-v8a/
- [ ] `SDL_main` 符号导出（`#include <SDL3/SDL_main.h>` → `#define main SDL_main`）
- [ ] SDL3 OHOS 后端 `SDL_OpenHarmonyMainSurfaceCreated()` 触发
- [ ] `dlopen("libmain.so")` 成功
- [ ] `dlsym("SDL_main")` 成功

### 3.2 引擎模块加载
- [ ] `liblauncher_client.so` dlopen 成功
- [ ] `LauncherMain` 符号找到
- [ ] `libtier0_client.so` 加载
- [ ] `libvstdlib_client.so` 加载
- [ ] `libfilesystem_stdio_client.so` 加载

### 3.3 资源路径
- [ ] 游戏根目录 `/data/app/el2/100/base/com.csgosource.ohos/files/csgo` 可访问
- [ ] `csgo/gameinfo.txt` 存在
- [ ] `csgo/pak01_dir.vpk` 存在
- [ ] overlay 资源（触控 UI + dxvk.conf）正确部署

## 4. 触控输入

### 4.1 SDL3 XComponent 触控回调
- [ ] `DispatchTouchEvent` 回调注册成功
- [ ] 单指触摸 → SDL touch event
- [ ] 多指触摸 → SDL multi-touch events
- [ ] 触控坐标正确映射到游戏窗口

### 4.2 Panorama 触控 UI
- [ ] csno_touch_hud.js 加载成功
- [ ] 虚拟摇杆响应
- [ ] 开火/瞄准按钮响应（bind j/k）
- [ ] 检视按钮响应（bind f）

## 5. 音频

- [ ] SDL3 OHAudio 后端初始化
- [ ] 音频设备打开成功
- [ ] 游戏音效播放

## 6. 稳定性

- [ ] 前后台切换不崩溃
- [ ] Surface 重建不崩溃
- [ ] 10 分钟持续运行无崩溃
- [ ] 崩溃日志可通过 hilog 收集

## 诊断命令

```bash
# 设备检查
scripts/diagnose-ohos.sh check

# 安装 HAP
scripts/diagnose-ohos.sh install

# 启动应用
scripts/diagnose-ohos.sh run

# 收集日志
scripts/diagnose-ohos.sh logs

# Vulkan 探测
scripts/diagnose-ohos.sh probe

# 崩溃日志
scripts/diagnose-ohos.sh crash

# 全流程
scripts/diagnose-ohos.sh all
```

## 关键日志标记

| 标记 | 含义 | 来源 |
|---|---|---|
| `XComponent loaded` | ArkTS XComponent 创建 | Index.ets |
| `game env prepared` | NAPI entry 模块环境准备 | EntryAbility.ets |
| `Calling SDL_main!` | SDL3 dlopen libmain.so 成功 | SDL_sysmain_runapp.c |
| `ENGINE_LOAD: liblauncher_client.so` | 引擎启动 | engine_startup.cpp |
| `ENGINE_ENTRY: calling Source LauncherMain` | 引擎入口调用 | engine_startup.cpp |
| `FIRST_FRAME_PRESENTED` | 首帧渲染成功 | android_main.cpp |
| `SURFACE_READY` | Vulkan surface 就绪 | android_main.cpp |
| `STARTUP_FAILED` | 启动失败 | android_main.cpp |