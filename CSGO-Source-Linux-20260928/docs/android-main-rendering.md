# Android 主场景开销与后续试验

2026-09-26，承接 [CSM 基础视锥修复](android-csm-culling.md)。基础视锥修复默认开启；
本页的两个后续原型默认关闭。按用户要求停止补充截图，后续以短时真机帧时间和绘制统计判断收益。

## 隐式提交：未测出稳定收益

DXVK D3D9 在切换渲染目标、`EndScene` 等位置发出提交提示。Android 原型通过
`debug.csgo.dxvk_defer_hints=1` 暂缓 strong / weak hint，仍保留真正的同步、
内存压力处理、显式 flush 和 Present。

同 APK、de_inferno CT、1100 MHz、2376×1080，实例化关闭，基础视锥修复开启；
按旧、新、新、旧测试，FPS 测量与 GPU 插桩分开，采样共 22.72 秒。

| 两次平均 | 原提交策略 | 暂缓提示 |
| --- | ---: | ---: |
| FPS | 107.89 | 108.36 |
| 两次 FPS | 106.66 / 109.12 | 108.04 / 108.69 |
| 整帧 GPU | 9.260 ms | 9.206 ms |
| CSM GPU 区间 | 1.599 ms | 0.433 ms |
| 非 CSM GPU 区间 | 7.661 ms | 8.773 ms |
| render pass 数 | 10 | 9 |

整帧差约 0.05 ms，FPS 差约 0.44%，小于首尾基准的波动。CSM 区间减少约 1.17 ms，
几乎全部被后段抵消，因此不能声称消除了约 1 ms 的实际工作，也不能把 pass 之间的间隔
直接当成 GPU 空闲。这一项保持关闭。

测试 APK build ID 为 `f832440abb882fed729a`。

- [原始数据及管线计数](../runtime/android/perf-main-20260926/flush-ab/results.json)
- [汇总](../runtime/android/perf-main-20260926/flush-ab/analysis.json)

复现：

```sh
distrobox enter -T -n dev -- python3 scripts/test-android-csm.py --launch \
  --output runtime/android/dxvk-flush-check --gpu-mhz 1100 \
  --seconds 2 --profile-frames 30 --defer-flush-cases 0,1,1,0 --pass-profile 30 \
  --warmup 2 --settle 0.4 --skip-images --background-after
```

测试脚本恢复原 Android 属性、GPU 频率限制和刷新率设置。

## 主场景：核查深度状态和实际排序位置

离线分析已有 `inferno-ct.rdc` 的 Vulkan 命令和 SPIR-V，不使用 RenderDoc 的逐 draw
回放耗时推算帧率。此抓帧早于基础视锥修复，下面仅用它检查主场景的结构和状态。

主场景 1007 次 draw 的状态如下：

| 状态 | draw 数 |
| --- | ---: |
| 开启深度测试、写深度、LESS_OR_EQUAL，无 alpha test | 805 |
| 同上，有 alpha test | 117 |
| 开启深度测试、不写深度 | 78 |
| 不做深度测试 | 7 |

这 1007 次均为单采样，片元程序均没有 `DepthReplacing` 输出。
所以不是误开多重采样，也没有发现“不透明绘制整体关闭深度测试”这一类简单错误。
这些状态不能单独证明硬件 early-Z 的实际效率。

新的整段采样中，主场景约 5.5–6.4 ms，片元调用约 1210 万次，视口约 257 万像素。
下一项优先验证能否通过调整不透明物体绘制顺序减少被遮挡像素的着色。

实际顺序由 `CStudioRender::BuildSortedRenderList(MeshRenderData2_t*)` 再次按材质、
顶点布局、光照、网格等排序。只改上层 `CModelRenderSystem` 的模型列表顺序会被这一步覆盖，
因此原型放在真正提交网格之前的 StudioRender 路径。

- [离线分析脚本](../runtime/android/perf-main-20260926/analyze-frame.py)
- [完整状态及着色器统计](../runtime/android/perf-main-20260926/frame-analysis.json)

## 道具深度排序：未测出稳定收益

`r_model_depth_sort` 默认 0：

- 0：原顺序。
- 1：保留 `DrawMeshRenderData` 原有的合批范围，在范围内部从近到远绘制。
- 2：在连续的适用网格范围内按 128 个世界单位的深度区间排序；同一区间保留原材质顺序，
  用来测量更强的遮挡优化是否值得增加材质切换。

只处理不透明路径中的单骨骼刚体网格，跳过 stencil、ignore-Z、贴花材质和材质强制覆盖。
原来的模型精度、着色器、纹理、实例数据和三角形全部保留；阴影与半透明绘制保持原排序。
相同排序键保持原顺序。`r_model_depth_sort_stats` 可请求数次列表统计，验证试验确实命中网格。

原型通过 Release 引擎和 APK 构建。补测按 `0,1,2,2,1,0` 交替采样，共 30.59 秒：

| 两次平均 | 原顺序 | 合批范围内排序 | 深度区间排序 |
| --- | ---: | ---: | ---: |
| FPS | 109.69 | 109.69 | 109.29 |
| 整帧 GPU | 9.109 ms | 9.244 ms | 9.296 ms |
| 主场景片元调用 | 12,119,234 | 12,098,006 | 12,110,501 |

非 CSM 模型 draw 均约 672、三角形均约 421,180。片元调用量变化不到 0.2%，
没有形成稳定收益，排序保持默认关闭。
原始记录见 [排序 A/B](../runtime/android/perf-main-20260926/sort-ab-3/results.json)
和 [汇总](../runtime/android/perf-main-20260926/sort-ab-3/analysis.json)。

最初一次因游戏被从任务列表关闭而未开始采样；另一轮因诊断使用 `DevMsg`、
开发日志级别为 0 而没有收到命中记录。这些不作为有效结果。
脚本现仅在读取排序统计时临时开启开发日志，并恢复原 FPS、垂直同步、渲染比例和输入设置。

复现：

```sh
distrobox enter -T -n dev -- python3 scripts/test-android-csm.py --launch \
  --output runtime/android/model-sort-check --gpu-mhz 1100 \
  --seconds 1.5 --profile-frames 20 --model-sort-cases 0,1,2,2,1,0 --pass-profile 20 \
  --warmup 2 --settle 0.4 --skip-images --background-after
```

验收关注同轮整帧时间、主场景片元调用量，以及没有减少模型/三角形的统计。
如果排序没有收益，下一步再评估深度预绘制或具体材质程序的等价优化；不根据单个分段计时猜测收益。

## 着色器调用统计与启动布局修复

新增 `debug.csgo.dxvk_shader_stats=1`，配合已有 `debug.csgo.dxvk_passes` 请求，
在现有 render pass 内按片元 shader key 统计 draw、VS、输入图元和 FS 调用量。
普通运行默认关闭。`--shader-profile --counts-only` 可只采管线计数，保持系统管理 GPU/屏幕频率，
不把诊断插桩耗时用于 FPS 对照。此细分统计尚未获得完整有效的真机数据。

一次新 APK 启动退出已查明为触屏菜单 `TouchButton` snippet 的布局错误：
原模板有四个并列根节点，首节点还带 `id`，违反 Panorama 的单一、无 ID 根面板规则。
为它增加 `<Panel>` 根节点后，已重新打包装机并正常进入 de_inferno，9 个机器人正常运行。
`android-build-tools.py` 的打包阶段现校验该约束，旧错误模板会在装机之前被拒绝。

- [启动错误和调用栈](../runtime/android/perf-shaders-20260926/crash-menu/error.txt)
- [修复后的构建检查](../runtime/android/perf-shaders-20260926/crash-menu/build-final-native.log)

性能脚本也会在游戏退回启动器时提前停止，并在退出时清除一次性 GPU 采样请求，
避免它们在下一次启动时自动重跑。
