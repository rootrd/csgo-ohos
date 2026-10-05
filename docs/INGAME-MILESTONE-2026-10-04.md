# 里程碑：de_dust2 真机进图渲染成功（2026-10-04 17:23）

## 结果
- 进程稳定存活（10+ 分钟），主循环持续出帧（HostStateFrame state=4 连续 353+ 帧）
- 第一人称视角渲染：世界几何 + 纹理 + 天空 + 光影 + **手枪 viewmodel + 准星**（玩家已出生）
- 资源占用稳定在 ~2GB（images 1263MB + buffers 692MB），远离系统杀死阈值

## 从"黑屏挂死"到"进图可玩"的完整根因链（按修复顺序）

1. **HUD 骨架布局遮蔽真实布局**（10-02/03）：`ShouldUseLoosePanoramaResources` 在
   OHOS 恒真 → 散装骨架 XML 遮蔽 code.pbin 真实布局 → 面板类型不符 → 野 vtable 崩溃。
   修复：`materialize_pbin_loose.py` 全量展开 + `sanitize_hud_xml.py` 消毒未知类型。

2. **CircularProgressBar 未链接**：静态库成员无人引用不拉入 → 工厂未注册 → compass.xml
   解析崩。修复：`csgo_compass.cpp` 加 `__attribute__((used))` 引用。

3. **vDSO/flock 假象**（撤销）：快照里 0x59cd659xxx 帧 = 信号跳板垃圾帧，非故障现场。

4. **g_once_cond 无限等待（挂死真根因）**：主线程 futex(x8=98) 等待 glib `g_once_cond`
   （x0=g_once_cond+8，x2=期望值，无超时）。根因 = **`uitextlayoutpango.cpp` 以无版本名
   `dlopen("libpango-1.0.so")`，而引擎其余组件按 DT_NEEDED soname `libpango-1.0.so.0`
   引用；HAP 内两个文件名是两份独立文件（同内容）→ musl 按名字去重失败 → **进程加载了
   两份 pango** → 第二份 GType 注册失败（'cannot register existing PangoFontMap'）→
   g_once location 永久报废 → 任何后续进入者无限等待。**与当年 SDL3 双实例同源**。
   修复：dlopen 改用 soname（`libpango-1.0.so.0` / `libpangoft2-1.0.so.0`）。
   （附带保留的 pango 预热代码在 Initialize 里提前完成各类一次性初始化。）

5. **`pango_font_map_load_font` 崩溃**：上一步修复后暴露——fontmap 对象头被踩成
   {12,93}；逐调用探针定位到 `pango_font_map_list_families` 一路的连锁反应，随 soname
   修复一并消失（对象从未被真正踩坏，是双库类型系统错乱的表现）。

6. **`V_MakeAbsolutePath: _getcwd failed` 致命**：加载匾 JS SetImage(相对路径
   de_dust2_radar.dds) → `CFileResource::Set` 的基目录 = `GetApplicationInstallPath()`
   → **SOURCE2_PANORAMA 分支直接 return ""** → 空基目录 → 落入 getcwd 分支且缓冲区按 0
   长度计算（19 字节）→ ERANGE/截断 → `V_AppendSlash` 溢出 Plat_FatalError 杀进程。
   修复：`uiengine.cpp` 该分支返回游戏根（CSGO_OHOS_GAME_ROOT 环境变量），并在
   `strtools.cpp` V_MakeAbsolutePath 加 OHOS 防死回退（绝不致死）。

7. **HUD NULL 崩溃**：进图时 `OnLevelLoadingStarted → CCSGO_Hud::GetInstance()->ReloadLayout()`
   实例为空（本 run HUD 窗口未建成）→ UnloadLayout 空指针。修复：判空跳过 + 打点。

8. **音效 PreloadSounds NULL 崩溃**：服务端进图 precache 时代码对 NULL 参数
   `ShouldPreload()` 解引用。修复：判空 continue + 打点（OHOS 防御）。

9. **内存墙（LowMemoryKill / GPU_LEAK_KILL）**：加载峰值 6.5GB（GL 侧 4.9GB），被
   OHOS memmgrservice 判 GPU 泄漏/低内存杀进程。逐层定位（自加 DXVK 资源足迹打印
   `G9_DXVK_RESOURCES`）：**Maleoon（Mali 系）不支持 BC/DXT 采样，引擎把 DXT 纹理解码
   成 RGBA8 上传（8 倍膨胀）**；1440 张纹理 3.9GB、1.26 万个小 buffer 1GB。
   修复：DXVK `d3d9_caps.h` 在 `__OHOS__` 分支把 `MaxTextureDimension` 从 16384 降到
   **512** → 引擎加载时自动降采 → 资源稳定在 ~2GB，系统不再杀。
   （长期正解见下文"待办"——按 0.3.0 参考包做移动端压缩纹理。）

## 当前已知限制（均已记录为后续项）
- **纹理质量**：512 上限导致画面偏糊（临时手段，换来稳定运行）；正解=A次代掌机压缩纹理
  路径（ASTC/ETC2，参考 CS:NO 0.3.0：把不透明 DXT1 转 ASTC 4×4，显存 -75%）
- **HUD 未创建**：本 run `CCSGO_Hud` 从未构造（ctor 打点 0 次），进图无 HUD 覆盖层；
  守卫避免了崩溃，HUD 缺位原因待查（layout/window 创建链）
- **声音被禁**（`-nosound`，内存止血时加）；音效脚本 5315 条缺失（scripts/game_sounds_*.txt）
- 顶部 10px 花屏带（DXVK/CSS 老问题，待查）
- 画面规格：2848×1276 全屏渲染

## 关键工程产物（本轮新增）
- `scripts/once-map.sh`（once-location→调用点映射表）、`sym-*.sh` 符号化工具
- libmain 超级看门狗：全线程 SIGUSR2 快照 + glib g_once_init_list 内窥（`=== THREAD SNAP`）
- DXVK 资源足迹打印：`G9_DXVK_RESOURCES imagesMB/images/largeMB/large/buffersMB/buffers`
- 构建纪律：**资源改动写 `CSGO-Source-Linux-20260928/ohos/{boot,overlay}`**（打包会
  rm -rf rawfile 后从 boot+overlay 重建）；libmain 必须手动强拷 hap libs（stage 不可靠）
