# 原生 Vulkan 后端计划

2026-09-24 建立，2026-09-26 更新。目标：Android 以 Vulkan 1.1 为兼容基础，在现有 Vulkan 1.3
Adreno 830 上先验证；通过材质合批、硬件实例化和按能力启用的间接绘制降低 CPU 开销。

当前执行顺序按最新要求调整：按较大功能批次推进框架，统一做 Linux 验收；
Android 打包和真机验收暂缓，继续保留为后续门槛。

Linux 游戏窗口也改用 SDL3，现有画面对照路径采用 native DXVK D3D9。
构建、依赖和独立运行目录见 [Linux 构建说明](linux-build.md)。
第二阶段仍以原生 ShaderAPI 的真实材质、地图、基础阴影和 UI 验收为完成标准，
完成前不进入第三阶段。Linux 已能加载两个原生模块并运行 Dust II；当前优先支持
Source 整数 HDR 的光照、曝光和烘焙数据路径，显示输出仍是 SDR，未实现 HDR10 输出。

## 范围与原则

- 新后端服务于 Source 的实际渲染需求，不实现完整的 D3D9 兼容层。
- Vulkan 基础代码位于 `src/materialsystem/shaderapivulkan/`，通过 SDL3
  共用窗口与 Vulkan loader，可在 Linux 独立验证，再由 Android 启动器调用。
- 基础路径使用 Vulkan 1.1、传统 RenderPass、普通 descriptor set、binary semaphore
  与 fence。dynamic rendering、synchronization2、timeline semaphore、bindless 均非前提。
- 可选能力必须分别查询。`multiDrawIndirect`、`shaderDrawParameters`、
  `drawIndirectFirstInstance`、descriptor indexing 的支持不能从版本号推断。
- HLSL 使用固定版本 DXC 离线生成面向 Vulkan 1.1 的 SPIR-V；保留着色器名称和变体语义，
  明确常量布局。离线 SPIR-V 不能代替运行时图形管线预热与缓存。
- 开发、依赖准备、生成器和测试全部在 Distrobox `dev` 中执行。
- 每个阶段均需有独立可复现的验收。窗口清屏不代表 Source 材质或地图已接通。

## 阶段 1：Vulkan 基础接入（Linux 已验收，Android 待验收）

交付一个可供下一阶段复用的 Vulkan 设备、资源与呈现模块，以及 Android / Linux
共用的诊断入口。现有游戏入口继续提供 DXVK 画面对照。

### 实现

- [x] 通过 SDL3 获取 Vulkan loader、实例扩展与 Surface，明确请求 Vulkan 1.1。
- [x] 选择满足基础要求的物理设备、graphics / present 队列；记录实际支持能力及启用能力。
- [x] 交换链、图像视图、RenderPass 与 framebuffer；FIFO 呈现和实际像素尺寸。
- [x] 每帧复用 command pool / command buffer / fence；按交换链图像管理呈现 semaphore。
- [x] 正常帧路径不调用 `vkDeviceWaitIdle`；重建、退出和显式诊断回读允许等待。
- [x] 使用固定版本 VMA 管理 buffer / image 分配、上传、回读及非一致内存缓存操作。
- [x] 资源释放与 GPU 完成同步，部分初始化失败可安全清理。
- [x] resize、最小化、Surface 丢失、前后台恢复的实现；重建呈现资源并保留设备和普通资源。
- [x] `VK_SUBOPTIMAL_KHR` 仍视为有效呈现，同像素尺寸事件不会重复申请重建。
- [x] 独立诊断图案和 GPU 像素回读；诊断失败返回错误并保存明确日志。
- [x] Debug APK 增加 `vulkan-probe` 动作，Linux 提供独立构建 / 运行入口。

### 验收

- [x] Linux 构建与真实 GPU 运行，开启 Vulkan validation，包含资源回读与窗口重建。
- [ ] Android ARM64 Debug / Release 构建；APK 签名、依赖、Build ID 和 16 KiB 对齐检查。
- [ ] Adreno 830 原生 Vulkan 诊断通过，使用的 API / 必需设备功能限制在基础路径。
- [ ] 真机前后台恢复后设备与上传资源继续有效；持续呈现不发生无故交换链重建。
- [x] 保留构建命令、测试结果和未覆盖设备范围。

第一阶段不实现 `IShaderAPI` 全部接口、不加载 `.vcs`、不迁移材质，也不将诊断图案
宣称为游戏场景渲染。诊断主要使用 render pass 清屏、附件区域清屏和 transfer 命令，
无需将临时着色器编译路线引入正式材质工具链。

## 阶段 2：Source 接口与首批真实材质

### 已完成的绘制底层框架

- [x] `UploadBatch` 合并 buffer / image 上传，复用暂存页，一批只提交一次；
  `UploadTicket` 用 fence 保持资源寿命，正常使用无需全设备等待。
- [x] `FrameArena` 按完成 fence 的帧槽复用动态顶点、索引、uniform / storage 数据页，
  处理对齐与容量上限，供批量对象数据使用。
- [x] 普通 descriptor layout / set、固定数组和动态缓冲区偏移；按帧复用 descriptor pool，
  保留引用资源，拒绝过期帧、已重置集合、缺失绑定和越界范围。
- [x] 单色附件加可选深度的离屏目标，传统 RenderPass、显式布局和 pass 顺序检查。
- [x] 顶点与实例流、16/32 位索引、动态 viewport / scissor、push constants、深度与混合状态；
  `drawIndexed(..., instanceCount)` 直接录制硬件实例化，不在 CPU 循环模拟实例。
- [x] 按着色器、布局、兼容附件格式、顶点布局及固定状态缓存图形管线；
  兼容的交换链重建继续复用管线。驱动缓存以设备、驱动版本、UUID 和数据校验值检查有效性并原子保存。
- [x] 固定 DXC `v1.8.2505.1` 及下载 SHA-256；从明确的名称、入口和宏定义清单生成
  `vs_6_0` / `ps_6_0`、`vulkan1.1` SPIR-V，运行 `spirv-val` 并记录源码/产物哈希。
- [x] 加载时检查 SPIR-V 版本与入口，反射普通 descriptor 的 set / binding / 类型 / 固定数组数量，
  创建管线前核对布局与阶段；基础路径拒绝运行时 descriptor 数组。

以上已通过同一 Linux 综合绘制验收。`framework.hlsl` 仍是框架验收资产；
真实材质的模块和地图验收单独列在下文。

### 已完成的 Source 数据桥接与材质快照

- [x] `sourceVertexLayout` 直接调用现有 `ComputeVertexDesc`，复用 Source 顶点 ABI、
  骨骼权重/索引偏移、UV 维数和 PC padding；通过语义映射 Vulkan attribute。
  支持未压缩交错顶点、骨骼和显式绑定的三路烘焙颜色流；压缩法线/切线仍未支持。
- [x] `prepareSourceTexture` 转换常用字节格式，支持部分浮点/有符号格式直传；
  DXT1/3/5 优先使用设备支持的 BC1/2/3 原生上传，无 BC 能力时使用 CPU RGBA8 解码。
  检查输入 pitch、数据长度和溢出，单 mip 输出限制为 64 MiB。
- [x] `SourceConstants` 保留 VS 256、PS 224 个 float4，以及各 16 个 int4 / bool 寄存器；
  HLSL 显式匹配布局，bool 用 uint4 打包。未变化的寄存器组每帧只上传一次，
  同帧更改使用新的不可变动态缓冲片段。
- [x] `SourceShadow` 实现真正的 `ShaderShadow010` / `IShaderShadow` 虚函数接口；
  支持深度、颜色/Alpha 写入、剔除、普通/独立 Alpha 混合、纹理开关、sRGB 状态、
  顶点用途和着色器静态索引。快照不可变、去重且有容量上限。
- [x] 保留 `EnableBlendingForceOpaque` 的排序语义；所选纹理视图和颜色附件必须匹配
  材质的 sRGB 状态；实际网格必须包含材质所需的顶点语义。
- [x] Alpha Test 使用共享 HLSL helper，保留 Source 的 8 位参考值截断规则；
  变体必须明确声明并实现 Alpha Test，调用者为每次 draw 传入快照中的 Alpha 参数。
- [x] 离线生成 `source-variants.tsv`；运行时按名称、阶段、静态/动态索引查找并缓存
  Shader module。缺失变体直接失败，不回退到索引 0；索引限定为有符号 32 位非负范围。
- [x] 上述数据桥接、Source 虚函数调用、快照复用和 Alpha Test 已纳入同一次 Linux 综合验收。

`csgo_vulkan_source` 是框架与游戏模块共用的静态库；`shaderapivulkan_client.so`
提供引擎加载入口，`stdshader_vulkan_client.so` 提供首批真实材质。
快照已保存雾参数、depth bias 和多边形模式；线框/点模式依赖单独查询并启用的
`fillModeNonSolid`。一般雾着色和 Alpha-to-coverage 仍未实现。

### 已完成的设备、网格和 ShaderAPI 核心（Linux 已验收）

- [x] `SourceDevice` 实现真实 `ShaderDeviceMgr001` / `ShaderDevice001` 虚函数接口；
  对已选定的 Context / SDL 窗口提供适配器信息、显示模式、模式变化通知和 `SetMode` 工厂。
  该工厂还可查询 `ShaderShadow010`，以及安装 `SourceAPI` 后的 `ShaderApi029` / `ShaderDynamic001`。
- [x] 静态/动态顶点和索引缓冲，`Lock` / `Unlock` / `Modify`、16/32 位索引和格式转换；
  CPU 预算计入数据和保留的锁定暂存区。结束动态格式转换后仍核对真实顶点格式，
  未初始化的动态索引缓冲允许首次选择 32 位格式。
- [x] 静态 `IMesh` 及设备所有的动态 `IMesh`；真实 Source 顶点 ABI、索引范围检查、
  primitive list 和 modulation。静态更新替换 GPU 分配，动态更新生成新的帧内片段，
  已录制的绘制不受后续修改/删除影响。
- [x] `ShaderApi029` 的快照、常量、纹理和绘制核心。使用真实的有符号 short 快照句柄，
  去重并限制容量，清理后不重用旧句柄；支持分类查询、顶点用途合并和按名称拒绝未迁移接口。
- [x] `BeginPass` / `RenderPass`、`IMesh` draw sink 与 `BindVertexBuffer` / `BindIndexBuffer` / `Draw`
  接入 Vulkan 管线、普通 descriptor 和寄存器缓冲；同帧复用相同资源的 descriptor set。
  `SetViewports`、`SetScissorRect`、颜色/深度清屏与深度、Alpha Test、透明混合共同验收。
- [x] 材质回调可接收 `IMaterial` 身份和网格请求，再选择 snapshot / 常量并录制 pass。
  游戏模块已接到真实 `IMaterialInternal::DrawMesh`；独立诊断仍使用受控的材质身份。
- [x] 有界的 2D/cube 纹理句柄表、完整 mip 链、`TexImage2D` / 带 pitch 的 `TexSubImage2D`、
  min/mag/mip 过滤、wrap、标准纹理和名称/尺寸查询；创建多帧纹理的批次失败会原子回滚。
  不完整 mip、过期句柄、越界、预算超限或不支持的创建标志明确失败。
- [x] RGBA8 纹理通过 Vulkan 1.1 mutable-format image 共用线性和 sRGB 采样视图，
  根据绑定标志选择，并核对快照中的 sRGB 语义。替代视图限定采样用途，不继承 storage 写入用途。
- [x] 纹理与静态网格合并上传；支持帧间和同帧更新，更新时替换 GPU image。
  `ReleaseResources` / `ReacquireResources` 保留 CPU 数据并重建上传；删除句柄会解除后续绑定，
  已录制 descriptor 保留原图像至 GPU 完成。
- [x] 新增 60 帧 GPU 综合验收，与已有绘制框架集中构建。首轮修复无 D3D 类型的消费者
  包含 `imageformat.h` 时的声明依赖；保留共享文件中的 ASTC 修改。
- [x] 上传只检查发生变化的纹理和静态缓冲；按帧回收上传 ticket，保留锁定检查和失败重试。
  `IMesh` 的顶点/索引基类指针通过别名表查找，避免每次绘制线性搜索全部网格。

`source_api.hlsl` 规定寄存器、纹理和 push constants 的显式绑定；顶点语义由调用者登记，
名称/静态/动态索引仍通过离线清单查找。`api_fixture.hlsl` 是接口验收资产；
`material.hlsl`、`twotexture.hlsl` 等提供首批真实材质，Panorama 复用现有 shader 源码。

当前 `ShaderApi029` 已接通矩阵、骨骼、环境/局部光、所需材质命令缓冲、
颜色/深度纹理目标、backbuffer 深度/stencil、局部纹理锁定、目标复制和 Panorama。
快速裁剪通过近裁剪投影实现，支持摄像机空间变换 override。
精确遮挡查询按 render pass 分段并求和，普通结果轮询不等待；显式 flush 才允许提交前缀或等待旧帧。
Source 材质实例接口仍逐对象提交；底层 `DrawEncoder` 已有硬件实例化，二者不能混同。
cube 数据可上传，但环境反射材质尚未完整接通；volume、自动 mip、MRT 和 MSAA 尚未实现。

### 首批真实材质与第二阶段剩余门槛

- [x] 游戏模块工厂、硬件能力查询、`IShaderUtil`、材质调用、渲染线程互斥和离线 SPIR-V 查找。
- [x] `UnlitGeneric`、基础 `LightmappedGeneric` / `WorldVertexTransition`、
  `VertexLitGeneric`、`DepthWrite`、Sky、Modulate、DecalModulate 和调试/遮挡材质。
- [x] Source 整数 HDR：线性 RGBA16 lightmap、scale 16、曝光和 sRGB 输出；
  加载 Dust II 的 `sp_hdr_*.vhv`，修复三路 BGRA 烘焙颜色上传及 CPU 后备写入。
- [x] Panorama / HUD、stencil/scissor 和写入 override；材质的 `SetDefaultState`
  不再清空上下文设置。SDL3 模式切换后同步实际窗口、后台缓冲及引擎尺寸。
- [x] `UnlitTwoTexture` 独立 UV 变换、颜色/骨骼变体及 HDR 曝光，真实天空云层已验收。
  crosshair/foam 分支已有实现但未做独立场景验收；cloak refraction 仍显式回退线框。
- [x] SplineRope、基础 bloom/downsample/blur、Engine_Post 和屏幕清屏/亮度统计材质。
- [x] Linux Dust II 地图主体、静态道具、武器、手臂、HUD、移动和窗口生命周期基础验收。
- [ ] 基础动态阴影及其真实地图回读/画面对照，不能以已有 `DepthWrite` 代替阴影验收。
- [ ] 首批材质在相同位置、设置下与 DXVK 对照并完成差异归类；
  `Character` 目前显式回退 `VertexLitGeneric`，未实现完整涂装、布料和高光。
- [ ] 达到上述第二阶段门槛后再进入第三阶段。CSM、flashlight、复杂粒子、水面、
  一般雾着色、完整环境反射和 WeaponDecal 等仍属未完成覆盖，不因基础地图可运行而标记完成。

## 阶段 3：功能覆盖与移动兼容

- 覆盖 CSM、粒子、水面、贴花、后处理、离屏目标、动态纹理和设备恢复。
- 完成材质变体盘点与管线预热，缓存按驱动和资源版本失效。
- 查询压缩纹理与深度格式；为不支持 BC 的设备提供适合的资源格式 / 缓存方案。
- 增加旧 Adreno 与 Mali 验收。新驱动以 1.1 API 运行不能代替旧设备验收。

## 阶段 4：减少 CPU 绘制成本

- 先测 CPU 逐对象处理、材质状态、命令录制、GPU pass 和提交等待的时间。
- 利用已有 BSP / PVS 与材质、光照贴图批次，缓存静态绘制信息，保持剔除粒度。
- 将相同网格和渲染状态的道具改成真正的硬件实例化。
- 对象、材质、骨骼数据按变化更新，避免每对象重复绑定、复制和分配。
- 支持的设备启用 multi-draw indirect；明确批次内管线、资源与几何绑定限制。
- 在确定有收益的场景中增加 GPU 剔除与间接参数生成，避免同步读回可见性。
- 透明和 UI 保持顺序、裁剪及混合语义；不以减少 draw 数量换取大量过绘。

验收采用固定画质 / 分辨率 / 场景路线，同时测不限帧性能与固定帧率 CPU 成本。
记录 CPU / GPU 帧时间和长时间运行后的温度、频率；不以 GPU 利用率单独证明优化。

## 工作量参考

目前按新增约 2万～3.5万行、实质修改约 5千～1.5万行手写代码估算；
不计第三方库、自动生成变体或 SPIR-V。完整 GPU 驱动渲染另列后续工作。
代码量是规模参考，实际工期取决于材质覆盖、驱动差异与回归。

## 当前验证记录

环境为 Distrobox `dev`，AMD Radeon 780M / RADV PHOENIX，Mesa 26.1.6、SDL 3.4.14。
实例明确请求 Vulkan 1.1，设备报告 Vulkan 1.4.354；Khronos validation 与
synchronization validation 均启用。当前按能力启用非实心多边形、BC 和精确遮挡查询；
descriptor indexing、间接绘制等仍未启用。

### 2026-09-26：HDR、查询、状态与上传回归

| 检查 | 实际结果 |
| --- | --- |
| 绘制框架 | `framework-hdr-perf/`：90 帧、450 draw，2 次 resize 回调；上传录制失败后的分配清理/重试通过，未变化缓冲不重新扫描；0 validation error / 0 live allocation。 |
| Source API | `api-hdr-perf/`：60 帧、780 draw；纹理更新、资源重获、删除后已录制绘制正确。仅检查 5 份纹理和 6 份静态缓冲，与实际上传数相同。 |
| HDR / 双纹理 | 线性 RGBA16 lightmap × 16 × 0.5 曝光，经 sRGB 写入得到 `{188,137,99,255}`；两张 sRGB 纹理独立变换后得到 `{61,64,255,255}`。 |
| 查询与 UI | 精确查询跨两 pass 合计 4608 样本，复用句柄不返回旧数据；显式 prefix flush 为 128 样本。Panorama 两种三角形绕序、stencil/scissor 交集和写入 override 通过。 |
| BC 后备 | `api-hdr-perf-no-bc/` 显式禁用 BC，CPU 解码后的相同材质/像素、HDR 和查询回归全部通过。两次 API 回归均为 0 validation error / 0 live allocation。 |
| 真实地图的此前验收 | `game-hdr/`：Dust II、静态道具、天空、手臂/武器和 HUD；水平移动 40.817 单位，1280×720 → 1024×768，最小化恢复及正常退出通过。亮度查询 27 次开始、21 次取得结果，0 强制等待 / 0 prefix flush；0 validation error / 0 live allocation。此记录早于待上传队列优化。 |

图像和日志位于 `runtime/vulkan/validation/` 对应目录。以上上传次数说明重复扫描已移除，
不构成 FPS 提升测量；真实游戏启用验证层的运行也不能直接用来比较发布版性能。
新队列优化后的游戏全流程还在复核模式切换耗时，未以此前游戏结果替代本次验收。

### 2026-09-24～25 的底层历史记录

以下历史批次关闭了可选设备功能，当时只有框架/API 变体；当前完整离线清单为 2053 个变体。

基础模块先前通过 `bash scripts/build-vulkan-probe.sh test`。2026-09-25 将设备/API、
纹理和动态网格一起构建，原绘制框架与新增 API 场景均通过。收尾的视图用途和动态
格式校验修正仅增量编译并重跑 API 场景，包含清屏和非全屏 viewport；
没有重复基础生命周期或 Android 验收。

| 检查 | 实际结果 |
| --- | --- |
| 基础模块，X11/XWayland | 377 帧；resize、最小化恢复和主动 Surface 重建后，原 buffer / texture 数据保持正确；颜色与 D32 深度共 8192 像素回读通过。 |
| 绘制框架，Wayland | 90 帧、450 次 draw；索引硬件实例化、2×2 纹理采样、非零动态 uniform 偏移、push constants、深度遮挡、透明混合与 scissor 回读通过。Alpha Test 同时验证通过/丢弃片元的颜色和深度。 |
| Source 数据布局 | 实际 Source 顶点 stride 24，BGRA 顶点颜色；带骨骼字段的 stride 64/偏移检查通过。BGRA/I8/A8、DXT1/3/5 CPU 转换及边界检查通过，RGBA 纹理上传采样通过。 |
| Source 寄存器 | GPU 读取 VS c255、PS c223、双方 i15/b15 正确；VS 4416 字节、PS 3904 字节；90 帧共 180 次寄存器组上传。 |
| Source 快照与变体 | 调用真实 `ShaderShadow010`，5 个不可变快照；清单扩展为 5 个变体，原框架只加载其中 2 个 Shader module；VS 静态/动态索引 7/2、PS 3/5；透明/force-opaque 分类和快照清理后存活检查通过。 |
| 设备、网格与复用 | 原框架 450 次 draw 均经过 `IMesh`。初始顶点、索引、纹理共一次提交，加上修改和资源重建共 3 次；静态 buffer 上传 5 次，动态片段 450 次。3 个管线、92 次管线命中；4 个 descriptor pool，动态页共 25216 字节且不持续增长。 |
| 窗口与缓存 | 87 个稳定帧无设备级等待；运行中尺寸改变和 Surface 替换后仍复用同一批管线；本次驱动缓存写出并重新加载 6380 字节。 |
| ShaderAPI 核心 | 从 `SetMode` 工厂取得真实 `ShaderApi029` / `ShaderDynamic001`；60 帧、780 次 draw，其中 720 次经 `IMesh`，60 次经独立 32 位索引缓冲。材质回调协议执行 60 次；short 快照清理后旧句柄被拒绝。 |
| API 数据与描述符 | VS/PS c255/c223、i15/b15 经真实虚函数设置并由 GPU 读取；同帧修改使用独立片段，共 1440 次寄存器组上传。分配 300 个 descriptor set，复用 480 次；双帧动态页稳定在 262144 字节。 |
| 纹理生命周期 | 2D mip 与带 pitch 的局部更新、线性/sRGB 共用图像、显式 mip / NOMIP 采样均通过。包括资源重新获取共 5 次 image、8 次 mip 上传，与静态 buffer 共 3 次提交；删除前已录制的像素仍正确，纹理 CPU shadow 最终为 0。 |
| API 画面与边界 | 清屏、非全屏 viewport、scissor、Alpha Test 颜色/深度、透明混合和遮挡通过；同帧动态网格先红后绿再改蓝，已录制的红/绿图形保持正确。纹理批次原子回滚、预算、首次 32 位 cast、结束 cast 后的格式误配、sRGB view 的 storage 用途限制均通过。 |
| 错误与释放 | 缺失变体、不支持 Alpha Test 的变体、sRGB 附件不匹配、缺失顶点语义、未启用的纹理阶段、布局不匹配、池预算超限、过期 descriptor 均被拒绝；`validation_errors=0`、`live_allocations=0`。 |

这是功能与生命周期验证，不是 Source 游戏性能对比。未覆盖旧 Adreno/Mali、分离的
graphics/present 队列、非一致内存专用设备、真实 GPU 丢失或 16 KiB 页 Android 设备。
Android 的完整 APK 构建、签名/对齐及 Adreno 前后台验收按当前要求暂缓，未标记通过。
两项绘制诊断均为 `validation_errors=0`、`live_allocations=0`。游戏接入和首批材质
的进度见上文；基础动态阴影、材质差异和完整合批性能仍未完成。

### 可复现入口

以下脚本自动进入 Distrobox `dev`，无需在 SteamOS 宿主机安装开发环境。

```sh
# 构建游戏模块和材质，再运行独立的原生地图验收。
bash scripts/build-vulkan-module.sh
python3 scripts/test-linux-renderer.py --shaderapi shaderapivulkan_client.so \
  --validation --output runtime/vulkan/validation/game-native

# 一次构建并检查基础模块、绘制框架和 Source API；首次会校验固定版 DXC。
bash scripts/build-vulkan-probe.sh test

# 统一运行绘制框架和 Source API，避免重复基础生命周期验收。
bash scripts/build-vulkan-probe.sh framework-test

# 仅修改 Source API/纹理适配层时可只运行新增场景。
bash scripts/build-vulkan-probe.sh api-test

# 普通运行使用 SDL 默认窗口系统；test 默认使用 X11/XWayland 做自动最小化恢复。
bash scripts/build-vulkan-probe.sh run --validation --seconds 15

BUILD_CONFIG=debug bash scripts/build-android.sh engine
BUILD_CONFIG=debug bash scripts/build-android.sh native
BUILD_CONFIG=release bash scripts/build-android.sh engine
BUILD_CONFIG=release bash scripts/build-android.sh native
BUILD_CONFIG=debug bash scripts/build-android.sh install

# 只打开图案，或一次完成约 35 秒的真机诊断及两次前后台恢复。
BUILD_CONFIG=debug bash scripts/build-android.sh vulkan-probe 15
python3 scripts/test-vulkan-android.py
```

Linux 保存 `runtime/vulkan/debug/self-test/vulkan-probe.log`、
`runtime/vulkan/debug/framework-test/graphics-probe.log` 与 `runtime/vulkan/debug/api-test/api-probe.log`，
以及 GPU 回读 PPM/PNG 和驱动缓存。
基础批日志在 `runtime/vulkan/validation/framework-batch.log`，数据桥接及材质批记录分别在
`source-bridge-batch.log`、`material-batch.log`；设备批和新 API 批分别在
`device-batch.log`、`api-batch.log`，保留编译修复前后的记录，末尾为最终成功结果。
这些是此前脚本批次的产物。2026-09-26 的增量回归目录见上表，包含更多材质 GPU 回读；
游戏脚本另存截图、模块哈希、RSS 峰值、模式切换耗时及原生上传/查询统计。
自动非均匀画面检查只证明有图像，HUD 和材质仍需人工看图。DXVK 对照保留在
`runtime/linux-sdl3/validation/game/`，原生测试使用独立目录。
着色器产物、完整编译清单及运行时变体索引在 `runtime/vulkan/shaders/`。
上述 Android 命令保留供后续验收使用。
Android 脚本保存 `runtime/vulkan/android/self-test/` 中的 native/logcat 日志、前后截图、
GPU 回读图像和 `result.json`，并核对安装构建编号、同一 PID、设备未重建及原上传数据。
Android 原生探针使用 Debug 私有目录，独立于游戏资源和共享存储权限。
缺层时显式请求 validation 会失败；真机可通过 `vulkan-probe 15 --validation` 启用，
前提是 Debug APK 已打包对应的 Khronos validation layer。

自动最小化恢复使用 X11，是因为 Wayland 合成器可拒绝无用户激活令牌的程序恢复请求。
普通 Wayland 运行仍走原生 SDL 窗口事件。完整 APK 仍包含 DXVK 游戏路径及 Vulkan 1.3
manifest 门槛；Linux 原生模块和诊断限制在 Vulkan 1.1 基础路径，不能据此宣称 Android 整包或旧机已验收。
