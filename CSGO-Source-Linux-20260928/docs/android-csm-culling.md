# Android CSM 基础视锥剔除修复

2026-09-26。修复默认开启，Release APK 已构建并在 PJZ110 / Adreno 830 上验收。
在 `de_inferno` CT 机位，默认绘制路径约 **75.25 → 99.70 FPS（+32.5%）**，
GPU 帧时间约 **13.12 → 10.08 ms**。这是同一 APK 内恢复旧行为与开启修复的交替测试。

## 原因和实现

`CVolumeCuller::IsValid()` 只检查补充的 inclusion / exclusion volume，漏掉了基础视锥。
当补充包含体为空、当前级联又没有排除体时，基础视锥虽然已经设置，
`CConcurrentViewBuilder::GetBuildViewVolumeCuller()` 仍返回空指针。
世界几何和模型列表因此跳过该视图的 CPU 剔除，大量视锥外几何交给 GPU 后才被裁掉。

修复前的管线统计确认，一个阴影批次输入约 **251 万个图元**，裁剪后只输出约 **1.8 万个**。
这比仅压缩顶点步长或合并 draw 更接近这部分浪费的根因。

修复是在 [volumeculler.h](../src/public/mathlib/volumeculler.h) 的有效性判定中加入
`m_bHasBaseFrustum`，继续使用原来的六平面包围盒检查。阴影仍每帧更新，保留原来的
投影、分辨率、材质、三角形精度、级联范围和所有与基础视锥相交的物体。

Android 的 `r_csm_base_frustum_culling` 默认 **1**，设为 **0** 可复现旧有效性检查，
用于同一 APK 内的对照；它是作弊诊断开关，不是画质档位。
`r_csm_instancing` 和 `r_csm_compact_vertices` 仍保持此前的默认值 0。

## 实测

条件：GPU 1100 MHz、120 Hz 屏幕、`fps_max=0`、`mat_vsync=0`、
`mat_viewportscale=0.75`（2376×1080），0 bot，位置 `2449.14 2010.22 192.09`，角度 `0 160 0`。
FPS 与 GPU 计时分开采集；每组顺序为旧、新、新、旧。

| 默认路径，实例化关闭 | 旧剔除 | 修复后 |
| --- | ---: | ---: |
| FPS，两次平均 | 75.25 | 99.70 |
| 两次 FPS | 76.37 / 74.13 | 99.43 / 99.97 |
| 整帧 GPU | 13.117 ms | 10.080 ms |
| CSM GPU | 4.950 ms | 1.839 ms |
| 非 CSM GPU | 8.167 ms | 8.241 ms |
| 模型阴影 draw | 4170 | 482 |
| 模型阴影三角形 | 2,885,903 | 434,190 |

模型阴影三角形减少约 **85%**；这部分物体完全位于该级联的基础视锥之外。
世界几何还走独立路径，不包括在以上模型计数中。

已经开启实例化的单独一组对照为 **92.55 → 108.64 FPS（+17.4%）**，
整帧 GPU 为 **10.645 → 9.129 ms**。不同轮次的调度、温度和场景状态存在变化，
不能把两组数字或先前 83 FPS 的历史基准直接相减，声称都是本次修复的收益。

基线诊断、实例化 A/B、默认路径 A/B 的性能采样合计 **48.23 秒**，加载地图、截图另计。
每轮结束恢复 GPU 频率限制、屏幕刷新设置，并把游戏送到后台。

## 验证和原始记录

- 回归测试先在旧代码上失败，明确报出基础视锥被判为无效；修复后 Linux ASan/UBSan
  和手机 ARM64 均通过，覆盖六个面的相交边界、完全在外的物体、复制到视图缓存、
  清空状态，以及独立 inclusion / exclusion volume。
- CT 冻结状态的五张最终画面逐像素相同，最大通道差 0；两种路径均确认重新绘制了 CSM。
- 用户随后确认各视角画面正常、帧率提高，要求停止补充截图，继续寻找优化点。
  补充机位截图中混入的通知及过渡帧不作为逐像素验收结果。
- Release APK 签名、16 KiB 对齐和动态库依赖检查通过。测试 build ID：`049f4af8ec9dd4f19bac`。

原始数据：

- [默认路径 A/B](../runtime/android/perf-culling-20260926/default-ab/results.json)
- [实例化路径 A/B](../runtime/android/perf-culling-20260926/culling-ab/results.json)
- [冻结帧比较](../runtime/android/perf-culling-20260926/culling-ab/image-comparison.json)
- [修复前管线统计](../runtime/android/perf-culling-20260926/baseline/results.json)
- [回归失败记录](../runtime/android/perf-culling-20260926/test-before.log)
- [Linux / ARM64 回归通过记录](../runtime/android/perf-culling-20260926/test-after.log)

复现，不重复截图：

```sh
distrobox enter -T -n dev -- bash scripts/test-csm-culling.sh --android
distrobox enter -T -n dev -- python3 scripts/test-android-csm.py --launch \
  --output runtime/android/csm-culling-check --gpu-mhz 1100 \
  --seconds 2 --profile-frames 30 --culling-cases 0,1,1,0 --culling-instancing 0 \
  --warmup 2 --settle 0.5 --skip-images --background-after
```

## 后续目标

后续提交策略试验、主场景状态核查和道具排序原型见 [主场景开销与后续试验](android-main-rendering.md)。

保留本次剔除修复，以主场景为下一重点。修复后的分段记录中，主场景约 5.5–6.4 ms，
约 1009 次 draw、1210 万次片元着色调用；3D 视口只有约 257 万像素。
需要区分过度绘制、片元辅助调用、深度测试和昂贵材质，不能直接把调用数比例等同于可消除的开销。

同时核查 DXVK 的隐式提交及批次间约 1 ms 的间隔。这些间隔可能包括驱动延后执行的工作，
不能直接全算成 GPU 空闲。单独清深度的批次约 0.02 ms，暂不优先优化。
