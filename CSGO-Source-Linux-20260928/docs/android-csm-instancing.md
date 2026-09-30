# Android CSM 实例化试验与 GPU 开销

后续已定位并修复基础视锥被误判无效的问题，默认路径约 75.25→99.70 FPS。
最新根因、对照数据和下一步目标见 [CSM 基础视锥剔除修复](android-csm-culling.md)。
下文为这次剔除修复之前的实例化和紧凑顶点试验，勿跨轮直接相减计算收益。

2026-09-26。设备为 PJZ110 / Adreno 830，测试地图 `de_inferno`，0 bot，CT 机位：

```text
setpos 2449.14 2010.22 192.09
setang 0 160 0
```

窗口 3168×1440，正常测试的 `mat_viewportscale=0.75`，3D 视口为 2376×1080。
屏幕 120 Hz，`fps_max=0`。389 MHz 是诊断时的 GPU 固定频率，不是 30 fps 限制。
下面的满频结果全部确认 GPU 为 1100 MHz。最后一轮性能采样共 31 秒；加载地图和截图另计。

## 实现及当前状态

`r_csm_instancing` 默认 **0**，是可切换的 Android/DXVK 原型。
在现有 `DrawInstancedPrims` / `RenderPassForInstances` 的末端，把相邻的兼容网格实例合成硬件实例化 draw。
继续使用原顶点、索引、逐物体剔除结果、模型精度、投影矩阵和深度状态。
每个实例的模型矩阵每帧写入实例缓冲，没有预先烘焙阴影或世界坐标顶点。

仅接受现有 `__DepthWrite000` / `__DepthWrite010` 材质，以及无骨骼、无形变的刚体网格。
镂空、树木摇摆、骨骼模型、特殊材质及不兼容实例保留原绘制路径。
符合条件的刚体实体也使用其当帧矩阵和当帧绘制列表；移动或删除实例不依赖缓存失效。

新增顶点着色器把原来的模型矩阵常量换成实例输入，保留模型变换和视图投影的两级点积。
原材质的像素着色器、裁剪和深度偏移继续使用。绘制结束恢复原 D3D9 shader、顶点声明、
索引缓冲、顶点流和实例频率；设备资源释放时销毁新增资源。

主要代码：

- [csm_instancing.cpp](../src/materialsystem/shaderapidx9/csm_instancing.cpp)
- [csm_instancing_vs.hlsl](../src/materialsystem/shaderapidx9/csm_instancing_vs.hlsl)
- [meshdx8.cpp](../src/materialsystem/shaderapidx9/meshdx8.cpp)

着色器字节码随源码保存，常规构建不需要额外 shader 编译器。修改 HLSL 后，使用
`python3 scripts/build-csm-instance-shader.py` 在 `dev` 中通过 `vkd3d-compiler` 更新 `.inc`。

## 已测收益

FPS 来自未开启 GPU 计时的 SurfaceFlinger 呈现记录；GPU 时间使用下面介绍的帧/CSM 边界采样。

| 对照 | 原路径 | 实例化 |
| --- | ---: | ---: |
| 389 MHz，两组 A/B 的平均 FPS | 31.08 | 34.33 |
| 1100 MHz，短测 FPS | 76.23 | 83.29 |
| 1100 MHz，同一冻结状态下 CSM GPU 时间 | 5.01 ms | 3.68 ms |
| 同一冻结状态的模型阴影 draw 数 | 4161 | 1104 |
| 同一冻结状态的模型阴影三角形数 | 2,872,295 | 2,872,295 |

每帧减少 **3057 次模型阴影 draw**，约 73%；正常满频下整帧节省约 **1.1 ms**，FPS 提高约 9%。
模型 draw 统计不包括世界几何的独立绘制。保留的镂空、形变和其他绘制仍会产生 draw。

原始记录：

- [低频重复 A/B](../runtime/android/csm-instancing-20260926/trial-1/results.json)
- [满频 A/B](../runtime/android/csm-instancing-20260926/trial-quick-1/results.json)
- [最终 GPU 分解与冻结帧采样](../runtime/android/csm-instancing-20260926/cost-breakdown-final/results.json)

第一轮的最后一次 GPU 采样被 Android `Force stopping com.csgosource.android` 中断。
已经完成的四段 FPS 和前三段 GPU 采样单独保存；该轮未标记为完整通过。

## 剩余开销

修正计时后，满频基准与末尾基准的整帧 GPU 时间分别为 12.03 / 12.01 ms，
CSM 为 3.68 / 3.60 ms，其余绘制为 8.35 / 8.41 ms。

以下删减仅用于诊断，测试完成后均恢复。不同项存在交互，不能直接把节省量相加。

| 诊断设置（实例化开启） | 整帧 GPU | CSM GPU | 非 CSM GPU |
| --- | ---: | ---: | ---: |
| 原画面，75% 渲染比例 | 12.03 ms | 3.68 ms | 8.35 ms |
| 阴影 LOD 2 | 11.98 ms | 3.63 ms | 8.35 ms |
| 50% 渲染比例 | 10.03 ms | 3.55 ms | 6.47 ms |
| 关闭静态道具阴影 | 9.44 ms | 0.21 ms | 9.23 ms |
| 关闭静态道具绘制 | 6.44 ms | 0.19 ms | 6.24 ms |
| 关闭后处理 | 11.43 ms | 3.56 ms | 7.87 ms |
| 恢复原画面 | 12.01 ms | 3.60 ms | 8.41 ms |

可以支持的结论：

- 实例化后，约七成 GPU 时间落在非 CSM 绘制，继续只优化阴影提交不足以解决剩余开销。
- 主画面像素数量减少时，非 CSM 开销下降约 1.9 ms，而几何提交量不变。
  主场景像素着色/纹理访问/覆盖量值得继续分析；这个实验尚不能区分 ALU、纹理吞吐和带宽。
- 在关闭静态道具阴影的对照下，继续关闭道具本体约省 3.0 ms，说明主画面的道具绘制也有明显开销。
- CSM 的剩余开销仍主要来自静态道具。它们在阴影中提交约 282 万个三角形，
  而非 CSM 模型绘制约 42 万个三角形。实例化保留了这些几何处理工作。
- 后处理约影响 0.6 ms，不是当前最大项。

LOD 实验这次接到了真正使用的 `CModelRenderSystem::ComputeModelLODs` 路径：
`r_csm_profile_lod` 默认 -1，只有诊断才覆盖阴影 LOD，不影响主画面模型。
本场景 LOD 2 仅把三角形数从 2,872,295 变为 2,871,834，减少 **461 个（0.016%）**。
这次操作并没有实质减少几何量，因此帧率不变依然不能用来排除几何处理瓶颈。

旧抓帧的资源检查确认 107 张 ASTC 4×4、119 张 ASTC 6×6 采样图像。
这些是抓帧中创建的图像数量，不是完整资源包或显存驻留量。
数据保存在 [capture-textures.json](../runtime/android/csm-instancing-20260926/capture-textures.json)。

## 计时方式

`r_csm_profile 45` 测量 45 个渲染帧，并输出：

- `gpu_ms`：CSM 开始到结束的 GPU 时间；
- `render_ms`：ShaderAPI `BeginFrame` 到 `EndFrame` 的 GPU 时间；
- `noncsm_ms`：两者之差；
- 模型 draw、三角形数、可合批数量和实际省掉的 draw。

查询跨帧异步读取，不等待 GPU，也不通过强制提交驱动队列来划分区间。
Android DXVK 的 timestamp 路径先结束当前 tile render pass，再记录时间戳，
仍把命令留在原提交批次内，避免把 CPU 供给间隙计入分段时间。
计时仍是插桩测试，FPS 对照时关闭它，单独采集呈现时间。

早期 `cost-breakdown-1` 使用强制提交划分边界，分段结果受干扰，**不作为最终分段耗时依据**。
RenderDoc 的逐 draw `EventGPUDuration` 求和也不等于整段 GPU 时间；
`scripts/renderdoc-gpu-profile.py` 已增加明确说明和近乎恒定计数器的提示。

## 画面验证及复现

在冻结游戏状态、相同相机下按“关、关、开、开、关”重新渲染并截图。
同时获取两种路径的非零 CSM 采样和完整三角形统计，确认不是比较未更新的旧帧。
五张 3168×1440 RGBA 图像逐像素相同：**变化像素 0，最大通道误差 0**。

- [像素比较记录](../runtime/android/csm-instancing-20260926/cost-breakdown-final/image-comparison.json)
- [原路径截图](../runtime/android/csm-instancing-20260926/cost-breakdown-final/off-a.png)
- [实例化截图](../runtime/android/csm-instancing-20260926/cost-breakdown-final/on-a.png)

已验证范围是这台手机、这个机位的最终画面和短时性能。
其他地图、运动中的级联边界及阴影 atlas 原始深度的逐位对比尚未覆盖，所以开关保持默认关闭。

构建完成后，测试脚本自动进入 `dev`：

```sh
python3 scripts/test-android-csm.py --launch \
  --output runtime/android/csm-test --gpu-mhz 1100 \
  --seconds 2 --profile-frames 45 --diagnose --warmup 2 --settle 0.5 --background-after
```

执行入口为：

```sh
distrobox enter -T -n dev -- python3 scripts/test-android-csm.py --launch \
  --output runtime/android/csm-test --gpu-mhz 1100 \
  --seconds 2 --profile-frames 45 --diagnose --warmup 2 --settle 0.5 --background-after
```

GPU 限频、刷新率及诊断设置会恢复，`--background-after` 在结束时把游戏送到后台。
在游戏控制台使用 `r_csm_instancing 1` 开启原型，`r_csm_instancing 0` 回到原路径。

## 后续试验：紧凑阴影顶点（2026-09-26）

已实现并验证，但**没有测出值得开启的帧率收益**。`r_csm_compact_vertices` 保持默认 0。
这组试验使用同一手机、机位、2376×1080 视口、1100 MHz GPU 和不限制 FPS 的设置，
在实例化已经开启的基础上，按 `0,1,2,1,2,0` 交替测试三种布局：

| 顶点布局 | 两次 FPS 平均 | CSM GPU 平均 |
| --- | ---: | ---: |
| 0：原顶点、原索引 | 83.37 | 3.654 ms |
| 1：12 字节 float3、原索引 | 83.45 | 3.592 ms |
| 2：float3，合并逐位相同位置并重映射索引 | 83.48 | 3.598 ms |

性能采样共 **27.93 秒**，每个设置采集 2 秒未插桩 FPS，再单独测 45 帧 GPU 时间。
最多只有约 0.06 ms 的 CSM 差值，FPS 差不到 0.2%，不足以确认实用收益。
整帧计时的首尾基准为 12.30 / 11.99 ms，其中非 CSM 部分也在变化；
不能把首轮整帧时间与优化轮次的差额全归功于紧凑顶点。

这次真实量到的数据：

- 可优化顶点全部是 **32 字节步长**，模式 1 改为 12 字节，仍保留 32 位浮点坐标。
- 每帧覆盖 3271 个实例、214 个实例化 draw、**2,149,289 个三角形**。
- 按每次实例/子网格引用的不同顶点累加，为 2,597,152 个；逐位相同位置合并后为
  1,294,741 个，减少 **50.15%**。这是索引引用统计，不是硬件顶点着色器调用计数。
- 两个优化档的实例数、三角形数及 draw 数相同，所有三角形顺序、绕序和退化三角形都保留。
- 两档一起预热后，CPU 数据副本容量约 **32.17 MiB**，新增 GPU 缓冲有效载荷约 **4.42 MiB**。
  这里没有计算容器管理结构和驱动分配粒度；GPU 数字同时包含两档缓存。

因此，可以降低“这部分阴影的顶点步长/重复位置是主要剩余瓶颈”的优先级。
三角形数量、光栅化与深度写入、剩余不兼容材质的绘制并没有减少，仍不能排除这些开销。
后续应先定位约 8.4 ms 的非 CSM 部分。

实现入口是 [csm_geometry.cpp](../src/materialsystem/shaderapidx9/csm_geometry.cpp)。
原缓冲使用 WRITEONLY，所以在模型上传的 Unlock 之前保存位置和索引，不回读 GPU 缓冲。
原缓冲的局部更新会使相关 GPU 缓存失效，未写入区域不会被使用；原缓冲销毁时释放相关缓存。
动态缓冲继续走原路径。缓存只含模型空间几何，逐帧姿态矩阵、材质选择和剔除继续由原路径决定。

测试包含原始逐 draw、实例化、紧凑顶点、位置去重的九张冻结状态截图。
每种路径仍重新绘制非零 CSM 工作，九张截图**逐像素相同**，变化像素及最大通道误差均为 0。
RGBA SHA256 为 `b4deae6039988f37dccbd0957e010ee400e172a08f42fd06f210217f8b2c71a9`。
仍只覆盖当前机位；未扩展为其他地图、运动级联和阴影 atlas 原始深度的全量验收。

CPU 的 ASan/UBSan 测试验证了逐索引位置等价、带偏移的子网格、部分上传失效、
16 位索引上界，以及有符号零、相邻浮点值和不同 NaN 位模式不会被误合并。
Release APK 的构建、16 KiB 对齐、签名和动态库依赖检查通过。此轮测试 APK build ID 为
`6085ef5f46c84b27959f`。

- [原始性能记录](../runtime/android/csm-compact-20260926/trial-1/results.json)
- [统计结果](../runtime/android/csm-compact-20260926/trial-1/analysis.json)
- [像素对照](../runtime/android/csm-compact-20260926/trial-1/image-comparison.json)
- [CPU 边界测试](../src/materialsystem/shaderapidx9/tests/csm_geometry_test.cpp)

复现时必须在加载地图之前开启紧凑数据捕获，测试脚本会自动处理：

```sh
distrobox enter -T -n dev -- python3 scripts/test-android-csm.py --launch \
  --output runtime/android/csm-compact-test --gpu-mhz 1100 \
  --seconds 2 --profile-frames 45 --compact-cases 0,1,2,1,2,0 \
  --warmup 2 --settle 0.5 --background-after
```

手动测试时，在加载地图之前设置 `r_csm_compact_vertices 2`，之后用
`r_csm_instancing 1` 配合 `r_csm_compact_vertices 0/1/2` 做对照。
中途才开启时，没有捕获到上传数据的网格会保留原路径。
