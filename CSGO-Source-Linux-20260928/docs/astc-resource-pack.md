# 离线 ASTC 资源包验收记录

2026-09-24。目标是提供离线转换工具和一套供后续分发使用的纹理资源，
并核对转换方向与相对原 VTF 的画质损失。开发、生成与测试使用 Distrobox `dev`。

## 范围与格式

本次覆盖 `runtime/csgo-2019/csgo/pak01` 的全部 **16,305 张 VTF**。
地图 BSP 内嵌纹理、其他 VPK、散装资源以及非 VTF 图片不在本次范围内。
离线 ASTC 适合移动 GPU，可将编码成本留在开发机。游戏经 D3D9 / DXVK 路径加载，
见下文“游戏内加载”。

| 项目 | 数量 / 结果 |
| --- | ---: |
| VTF 输入 / KTX2 输出 | 16,305 / 16,305 |
| ASTC 4×4，medium | 9,279 |
| ASTC 6×6，medium | 6,941 |
| 无压缩半浮点 HDR | 31 |
| 无压缩有符号 UV | 2 |
| RGBA8 体积纹理 | 52 |
| 包含动画的纹理 | 35 |
| 立方体纹理 | 40 |
| mip 层合计 | 156,297 |
| 帧 × 面 × mip 切片合计 | 162,992 |
| 转换失败 / 校验错误 | 0 / 0 |

所有 mip、动画帧、立方体面、体积切片及 VTF 辅助资源均保留。
正常颜色图默认 6×6；NORMAL/SSBUMP、alpha 标志和 UI 等优先使用 4×4。
规则依据 flags 和路径，表面上是颜色图的纹理也可能因 alpha 标志被选为 4×4。
HDR、有符号纹理和体积纹理使用保留数值或维度的无压缩格式。

## 体积

统计使用十进制 GB，比较的是选中纹理本身，不是整个游戏安装目录。

| 资源 | 字节 |
| --- | ---: |
| 原 VTF 合计 | 9,284,245,314 |
| KTX2 合计 | 9,101,712,988 |
| KTX2 相对 VTF 减少 | 182,532,326（1.966%） |

4×4 的主体码率与 BC3 相同、为 BC1 的两倍，6×6 对 BC1 仅省约 11.1%。
体积纹理解码为 RGBA8 后从约 114 MB 增至 250 MB。因此，这套偏重保真的策略
主要解决 GPU 格式兼容问题，不能期待仅凭转换就大幅缩小下载体积。
分发归档还使用包级 Zstandard 压缩，准确大小见压缩包旁的 `.json` 收据。
这不等同于相对“同样压缩后的原 VTF 包”的体积优势。

## 画质对比

对比基准为**原 VTF 解码后的 mip 0**，保留原尺寸、帧与面；不是美术制作阶段的
无压缩源图。BC 源解码同时与 Pillow 独立解码器交叉检查，8 个样本的最大像素差均为 0。
ASTC 用 Arm 官方 CLI 解码。已有完整尺寸滑块对比页和放大裁剪图：

- [交互对比页](../runtime/astc/quality/comparison.html)
- [2 倍最近邻裁剪图](../runtime/astc/quality/crops.png)
- [完整数值](../runtime/astc/quality/quality.json)

RGB PSNR 越高表示误差越小；此处数字只刻画选中样本的额外压缩损失。

| 样本 | 4×4 RGB PSNR | 6×6 RGB PSNR | 正式包使用 |
| --- | ---: | ---: | --- |
| Dust II 砖墙 / BC1 | 55.44 dB | 35.21 dB | 6×6 |
| Dust II 地面 / BC1 | 59.65 dB | 41.73 dB | 4×4 |
| 签名贴纸 / BC3 | 53.55 dB | 38.98 dB | 4×4 |
| 棕榈树叶 / BC3 | 43.99 dB | 32.44 dB | 4×4 |
| 泥砖法线 / BGR888 | 29.85 dB | 24.11 dB | 4×4 |
| 砖墙法线 / BC1 | 56.41 dB | 30.00 dB | 4×4 |
| Dust II 菜单缩略图 / BC3 | 57.37 dB | 40.02 dB | 4×4 |
| 金属图 / BC3 | 58.63 dB | 46.22 dB | 4×4 |

放大图中，4×4 对已有 DXT 样本通常很接近原图；6×6 的砖墙细节、树叶边缘与
法线高频细节更容易变模糊。树叶 alpha 的 PSNR 从 4×4 的 37.45 dB 降到
6×6 的 29.73 dB，透明边缘需要保守选择。

未压缩法线更敏感：泥砖法线的平均方向误差为 4×4 **4.39°**、6×6 **9.59°**，
95 分位为 10.95° / 22.27°。原本为 BC1 的砖墙法线平均误差为 0.16° / 4.66°。
角度统计假定这些样本的 RGB 表示 XYZ，并在计算前归一化向量；不能套用于其他
通道打包方式。即使选择 4×4，也不能把未压缩法线的损失当成不可见。

打包时核对了每个样本的来源 SHA-256，且其正式选中尺寸下的**所有 mip 压缩数据**
与正式包逐字节相同，结果保存在归档内 `quality/production.json`。
这证明对比对应本次产物；它不代替全库逐张评估或游戏内光照、运动和真机画质验收。

## 完整性与交付

资源目录是 `runtime/astc/csgo-2019-mobile/`。分发文件位于 `runtime/astc/dist/`：

- `csgo-2019-mobile-astc-v1.tar.zst`：纹理、旁清单、源盘点、说明和画质报告。
- `csgo-2019-mobile-astc-v1.tar.zst.sha256`：压缩包 SHA-256。
- `csgo-2019-mobile-astc-v1.tar.zst.json`：体积、文件数量、参数和归档回读结果。

Khronos KTX-Software 4.4.2 已逐张检查全部 16,305 个 KTX2，同时核对来源覆盖、
SHA-256、格式、尺寸、帧/面/切片、mip 长度及元数据。唯一允许的 warning 是
7010（应用自定义 `source.vtf.*` 元数据），共 142,005 条；其他 warning 和所有 error
都不允许通过。之后再次检查原始包的 32,612 项 SHA-256，全部一致。

当前回归入口共 **16 组测试通过**：11 组转换格式/解码/续跑测试，以及 5 组打包测试，
后者覆盖内容损坏、过期校验、缺漏、额外文件、符号链接、并发转换、已有输出保护、
压缩流截断、画质数据关联和固定归档元数据的字节可复现性。
上次会话修复 libktx writer 元数据小泄漏后，11 组转换测试也通过了 ASan/UBSan。

包内 `SHA256SUMS` 同时保护纹理、源清单、校验结果、使用说明和画质报告。
打包过程中逐文件计算哈希，压缩完成后流式解压并再次检查每个成员，成功后才发布归档。

## 复现与后续接入

构建、转换、格式校验、画质生成和打包命令见
[工具说明](../scripts/astc-convert/README.md)。打包入口自动进入 `dev`：

```sh
bash scripts/build-astc-convert.sh package \
  --pack runtime/astc/csgo-2019-mobile \
  --inventory runtime/astc/inventory/manifest.json \
  --quality runtime/astc/quality \
  --out runtime/astc/dist/csgo-2019-mobile-astc-v1.tar.zst \
  --threads 8

distrobox enter -T -n dev -- bash -c \
  'cd runtime/astc/dist && sha256sum --check csgo-2019-mobile-astc-v1.tar.zst.sha256'
```

已有压缩包不会被覆盖；重打包请换一个输出文件名。相同内容、脚本与 zstd 参数/版本
产生相同归档字节。解包后，在包根目录执行 `sha256sum --check --quiet SHA256SUMS`。

来源清单中记录的实际生产二进制已保存在
`runtime/astc/provenance/vtf2astc-producer`，其 SHA-256 为
`9aba55684c0605e3d72dedd840f13a4876498c201e7693de5127f3e1fd05740a`。
清单与生产二进制一起保存用于追溯。重新构建后工具哈希变化会让 `--resume` 重新编码；
只复验或重打包时使用相应入口即可。

## 游戏内加载

- 启动参数 `-astcpack <目录>` 把包挂到 `ASTC` 搜索路径；Android 上 `<资源根>/astc`
  存在即自动传入。`CSGO_ASTC_PACK=runtime/astc/csgo-2019-mobile bash scripts/build-android.sh sync`
  会只推送 `.ktx2`。
- `CTexture` 读 `.vtf` 前先查同名 `.ktx2`；`vtf` 库的 `ConvertKTX2ToVTF` 用
  `source.vtf.header`/`source.vtf.resource.*` 和 KTX2 层还原 VTF 文件映像，
  之后走原有 VTF 流程：跳 mip、动画帧、立方体面、sheet/LOD/CRC、sRGB 采样状态都不变。
  包未覆盖的纹理（地图内嵌、其他 VPK）照常读 `.vtf`。
- 新增 `IMAGE_FORMAT_ASTC_4X4/6X6`，对应自定义 FourCC `AS44`/`AS66`；in-tree DXVK 补丁
  （`android/patches/dxvk-android.patch`）把它们映射到 `VK_FORMAT_ASTC_*_UNORM/SRGB_BLOCK`，
  仅在设备启用 `textureCompressionASTC_LDR` 时报告可用。ASTC 没有 CPU 编解码与替代格式：
  设备不支持、`mat_compressedtextures 0`、包文件损坏或与头部不一致都直接 `Error()`。
  `vtf2astc --srgb` 产生的 sRGB 块会被拒绝，sRGB 必须由材质采样状态决定。
- 主机回归：全部 16,305 个 KTX2 经 `ConvertKTX2ToVTF` + `CVTFTexture::Unserialize`
  还原，所有子资源与 KTX2 层逐字节一致，辅助资源与缩略图一致，跳 1/2 级 mip 的截断读取正确。
