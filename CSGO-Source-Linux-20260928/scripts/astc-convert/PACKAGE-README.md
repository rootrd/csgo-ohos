# Source 移动端纹理覆盖包

本包由原始 VTF 离线转换而来，供未来原生 Vulkan 加载器接入。当前 D3D9 / DXVK
游戏路径不能直接加载它。范围是 `distribution.json` 中指定的 VPK 与筛选条件；
它不包含完整游戏，也不包含地图 BSP 内嵌纹理、其他 VPK 或散装资源。

## 校验与内容

解包后，在本文件所在目录运行：

```sh
sha256sum --check --quiet SHA256SUMS
```

命令退出码为 0 且没有输出表示包内所有文件哈希一致。

- `materials/**/*.ktx2`：纹理数据；通常是 ASTC，HDR、有符号数据和体积纹理使用
  保留数值/维度的无压缩格式。根据 KTX2 的 `vkFormat` 选择上传格式。
- `materials/**/*.ktx2.json`：每张纹理的原始路径、格式、尺寸、来源与产物 SHA-256、
  编码策略等信息；实际路径以 `manifest.json` 为准。
- `manifest.json`：完整纹理清单与转换设置。
- `source-inventory.json`：转换前的 VTF 清单，可核对覆盖范围和原始数据哈希。
- `validation.json`：逐张 Khronos KTX 格式校验、子资源检查和覆盖检查结果。
- `distribution.json`：本包范围、计数、字节数、工具版本与打包参数。
- `quality/`（若提供）：原 VTF 与 ASTC 的对比图、数值和可离线打开的
  `comparison.html`；`production.json` 记录选中策略的每个样本与本包所有 mip
  压缩数据逐字节匹配的结果。样本结果不能视为全库或游戏内的画质验收。
- `SHA256SUMS`：上述所有文件的 SHA-256，包括说明和画质报告。

## 加载约定

按 `manifest.json` 的原 `.vtf` 路径查找对应 KTX2。全部 mip 保留；动画帧对应
array layer，立方体面沿用 Source 顺序，体积纹理保留 mip 深度和所有切片。
`source.vtf.*` 元数据保留 VTF header、辅助资源、sampler/LOD 标志、动画信息和旧版球面。
元数据中的原始文件偏移不能作为 KTX2 数据偏移使用。

当前默认保存原数值并使用 UNORM；sRGB 采样由材质决定。游戏以 `-astcpack <目录>`
加载本包（需 GPU 支持 ASTC LDR）。

ASTC 是有损编码。4×4 对已有 DXT 样本通常很接近原图，6×6 更容易损失细节；
未压缩法线图即使用 4×4 也可能出现可见损失。本包仍需原生加载器、真实材质/地图
和目标 Android GPU 的运行验收。
