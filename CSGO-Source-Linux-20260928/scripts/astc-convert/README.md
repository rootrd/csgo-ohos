# 离线 VTF → ASTC / KTX2 工具

从 Source VPK 读取 VTF，生成独立的纹理覆盖包。原始 VPK 不会被修改。
这是供未来原生 Vulkan 加载器使用的资源，不是当前游戏可以直接加载的替换包。

## 方向和范围

离线编码 ASTC 适合 Android：昂贵的编码留在开发机，设备端可直接上传压缩块。
游戏通过 D3D9 / DXVK 路径加载本包，见 [资源包说明](../../docs/astc-resource-pack.md#游戏内加载)。
不建议在 DXVK 管理的纹理中插入一条私有 Vulkan 上传旁路，这会额外引入资源与同步问题。

当前完整转换范围是 `runtime/csgo-2019/csgo/pak01` 中的 16,305 张 VTF，
不是整个游戏安装目录。地图 BSP 内嵌纹理、其他 VPK、散装资源和非 VTF 的 UI 图片
不在这次清单中。模型、声音、材质 VMT 等也不是 ASTC 编码对象。

保留每张图的所有 mip、动画帧、六个立方体面及体积切片。动画帧对应 KTX2 array layer；
立方体面的原始顺序不变。旧 VTF 的第七个球面回退面保存在元数据中。
VTF 头、缩略图、sheet/CRC/LOD 等辅助资源也随 KTX2 保存，游戏加载时据此还原完整 VTF 语义。

## 构建和运行

入口自动进入 Distrobox `dev`。宿主机无需安装开发依赖。

```sh
bash scripts/build-astc-convert.sh build
bash scripts/build-astc-convert.sh test

# 先读取并校验全库，不进行 ASTC 编码。
bash scripts/build-astc-convert.sh run \
  --vpk runtime/csgo-2019/csgo/pak01 \
  --out runtime/astc/inventory --mode inspect

# 本次资源包的设置。
bash scripts/build-astc-convert.sh run \
  --vpk runtime/csgo-2019/csgo/pak01 \
  --out runtime/astc/csgo-2019-mobile \
  --block auto --quality medium --threads 8 --resume

# 单张图；--dump-rgba 可输出首帧、首面、mip 0 的解码基准。
bash scripts/build-astc-convert.sh run \
  --input example.vtf --out runtime/astc/example --block 4x4 --dump-rgba
```

也可以用 `--filter` 按路径子串筛选，`--limit N` 限制选中的纹理数，
或用 `--mode extract` 提取通过 CRC 和布局检查的 VTF。
`--vpk` 同时接受 `pak01` 和 `pak01_dir.vpk`。

依赖固定为 Arm astcenc **5.7.0**、Khronos KTX-Software **4.4.2**、
nlohmann/json **3.12.0**；下载 SHA-256 在构建脚本内。
缓存和构建都在 `runtime/astc/`。当前 KTX 开发包为 Linux x86_64；
astcenc 使用本机 CPU 指令，工具二进制不作为跨机器发行物。
`dev` 中需要 CMake、Ninja、C++ 编译器、OpenSSL/zlib 开发库和 curl；
测试/对比脚本需要 Python、Pillow、NumPy。

## 编码策略

| 输入 | 默认 `--block auto` |
| --- | --- |
| 普通颜色图 | ASTC 6×6，3.56 bit/texel |
| `NORMAL` / `SSBUMP`、带 alpha 标志的图、VGUI、sprite、decal | ASTC 4×4，8 bit/texel |
| 半浮点 HDR / 高精度数据 | 对应的无压缩 KTX2 格式，保留原始数值 |
| 有符号 UV | `R8G8_SNORM`，不错误地改成 UNORM |
| 体积纹理 | 无压缩 KTX2，保留全部切片和 mip 深度；普通颜色数据使用 RGBA8 |

也支持显式 `4x4`、`6x6`、`8x8`；HDR、有符号数据和体积纹理仍保留精度与维度。
支持 `fastest`、`fast`、`medium`、`thorough`、`exhaustive`，分别使用 astcenc 的真实预设。
`--threads` 是并行文件数，每个 worker 复用自己的一线程编码上下文。

没有重新生成 mip，没有 gamma 变换，没有翻转图像，没有重新打包法线通道。
法线保留完整 RGBA；alpha 可能是透明度，也可能是粗糙度/反射等遮罩，不能统一清零或丢弃。
`--srgb` 是针对颜色图的显式覆盖；法线和数据纹理仍保持线性。
默认保存原始数值并使用 UNORM。Source 的 sRGB 读取由材质控制，VTF 标志不能可靠替代它；
未来加载器需按材质选择 UNORM / sRGB 采样视图，并查询对应格式及视图支持。

ASTC 4×4 大约是 BC1 的两倍码率，与 BC3 相同。ASTC 6×6 的主体块数据比 4×4
小约 **55.6%**，不是 30%；对 BC1 仅省约 11.1%。小 mip 的块补齐和 KTX2 元数据
会降低实际节省比例。这些数字不包括包级压缩。

## 完整性与复验

每次 VPK 读取会校验 CRC32，包括 preload 与内嵌数据。VTF 解析检查尺寸、版本、
资源范围、mip 布局及数据长度；还处理 Source 对压缩体积 mip 的深度填充规则。
任何读取、解析、编码或写入失败都会进失败清单，并返回非零退出码。

每张 KTX2 由 libktx 生成 DFD、类型、对齐及 mip 索引，写入临时文件后重新加载并逐字节
核对所有子资源，再原子替换目标文件。旁边的 `.ktx2.json` 记录来源/产物 SHA-256、
来源维度/格式、处理方式及编码设置。全局 `manifest.json` 明确记录选中范围与成功状态。

`--resume` 同时匹配源文件 SHA-256、编码设置、工具二进制 SHA-256 和现有产物 SHA-256；
不以文件存在作为成功。中断后可原命令续跑。变更工具或设置会重新编码。
同一个输出目录有进程锁，避免两个转换同时覆盖文件。

```sh
distrobox enter dev -- python scripts/astc-convert/validate_pack.py \
  --pack runtime/astc/csgo-2019-mobile \
  --inventory runtime/astc/inventory/manifest.json \
  --ktx runtime/astc/tools/KTX-Software-4.4.2-Linux-x86_64/bin/ktx

distrobox enter dev -- python scripts/astc-convert/quality_report.py \
  --build runtime/astc/build --vpk runtime/csgo-2019/csgo/pak01 \
  --out runtime/astc/quality
```

完整校验逐张执行 Khronos `ktx validate`，并独立检查源清单覆盖、文件 SHA-256、
格式、每层尺寸/字节数、帧/面/切片和元数据。成功后生成 `validation.json` 和 `SHA256SUMS`。
唯一允许的官方 warning 是 **7010**：KTX2 允许应用自定义 key，工具用 `source.vtf.*`
保存引擎所需信息。其他 warning 和全部 error 都会导致验收失败。

## 打包与交付

完成上述校验后，可生成包含清单、源盘点、使用说明和画质报告的 `.tar.zst`：

```sh
bash scripts/build-astc-convert.sh package \
  --pack runtime/astc/csgo-2019-mobile \
  --inventory runtime/astc/inventory/manifest.json \
  --quality runtime/astc/quality \
  --out runtime/astc/dist/csgo-2019-mobile-astc-v1.tar.zst \
  --threads 8
```

这个入口自动进入 `dev`，仅需 Python 与 zstd，不重新构建转换器或重新编码纹理。
它持有转换目录锁，检查校验报告与清单是否匹配、来源覆盖是否完整、文件是否缺漏，
并在写入归档时逐文件核对 SHA-256。画质报告是可选的；提供时，每个样本必须与正式包
相应压缩块尺寸的全部 mip 数据一致。

归档按路径排序，使用固定时间戳、权限和 uid/gid。完成压缩后，工具会流式解压并
校验每个归档成员，再原子发布压缩文件。相同输入、脚本与 zstd 版本/参数可重复得到
相同字节；已有同名产物会被拒绝，重打包时选择新输出文件名。
旁边的 `.sha256` 校验压缩包，`.json` 记录成包大小及回读结果。包内 `SHA256SUMS`
覆盖纹理、清单、说明与画质报告，解包后可直接用 `sha256sum --check --quiet SHA256SUMS` 复验。

`bash scripts/build-astc-convert.sh test` 包含 11 组转换回归与 5 组打包完整性测试。
2026-09-24 的全量转换统计、实际交付物和画质结论见
[资源包验收记录](../../docs/astc-resource-pack.md)。

## 元数据约定（schema 1）

- `source.vtf.header`：原 VTF header 和资源目录的原始字节。目录偏移只用于追溯原文件，
  不能当成 KTX2 的偏移使用。
- `source.vtf.resource.XXXXXXXX`：完整 32 位资源类型（含标志）的十六进制键。
  内联资源保存 4 字节值，缩略图保存原始图像字节，其他资源保留长度前缀与 payload。
- `source.vtf.legacy_spheremap`：如有旧第七面，以原 VTF 的小 mip 到大 mip、帧顺序保存其块数据。
- `source.vtf.info`：原尺寸、格式、flags、startFrame、帧/面数等 JSON。
- `source.vtf.path` / `sha256` / `recipe` / `sampling`：路径、来源哈希、转换配置和采样约定。
- `KTXorientation` 为 `rd`（二维）或 `rdi`（三维）；文件保留 Source 的原始像素与面顺序。

未来接入须保留 mip/LOD、sampler flags、动画起始帧和 sheet 数据、cubemap 坐标语义、
法线/遮罩通道和 sRGB 选择。当前离线验证不代替游戏地图、材质、真机 GPU 和帧时间验收。
