# csgo-ohos — CS:GO 2019 引擎 HarmonyOS（鸿蒙）移植

将 CS:GO 2019 引擎源码（kisak-strike / CSNO Android 同源版本）移植到 HarmonyOS，
目标真机 **Huawei Pura 80 Ultra（Maleoon 920 GPU）** 可玩。

```
┌─────────────────────────────────────────────────────────┐
│  ArkTS 启动页（Launcher/Game 两态）                        │
│   · 资源检查 / 本地导入 19GB 资源包 / URL 下载接口          │
│   · XComponent + SDL3 → dlopen libmain.so 拉起引擎         │
├─────────────────────────────────────────────────────────┤
│  引擎层（Source 2019，aarch64-linux-ohos 交叉编译）         │
│   · launcher → appframework → engine 26 模块全量自编        │
│   · Panorama UI（DEVELOPMENT_ONLY 散装文件模式）            │
│   · vscript（Squirrel 真编译；v8 为桩，仅影响 Panorama JS）  │
├─────────────────────────────────────────────────────────┤
│  图形栈：D3D9 → DXVK（legacy 分支）→ Vulkan 1.3 → Maleoon 920 │
│   · Maleoon quirk：同 render pass 多次 vkCmdClearAttachments │
│     只有第一次生效（probe 实测）                             │
├─────────────────────────────────────────────────────────┤
│  兼容层：sse2neon（SSE2→NEON）、musl 文本栈                  │
│   （pango/cairo/fontconfig/glib 自建）、SDL3 OHOS 后端       │
└─────────────────────────────────────────────────────────┘
```

## 当前状态（2026-09-30）

**引擎初始化推进至 vgui2 之后**。完整状态、已攻克关卡清单与当前卡点见 [STATUS.md](STATUS.md)，
联调战报与调试方法见 [docs/PORTING-PLAN.md](docs/PORTING-PLAN.md)，
全部踩坑与知识点见 [docs/KNOWLEDGE-BASE.md](docs/KNOWLEDGE-BASE.md)。

| 里程碑 | 状态 |
|---|---|
| WSL 交叉编译全链（26 模块 + libmain，90 库入 HAP） | ✅ |
| Vulkan probe 上屏（2132 帧 ~120FPS，四色图案真机可见） | ✅ |
| 19GB 游戏资源导入（Steam depot 731 完整版） | ✅ |
| 引擎初始化：D3D9 device / RESZ/INTZ / studiorender 材质 / vguimatsurface / fonts / vgui2 | ✅ |
| vgui2 之后 → engine 主循环 → 主菜单 | 🔬 定位中（InitSys 二分打点已装机） |
| 主菜单 → 加载地图 → 可玩 | ⏳ 未开始 |

## 仓库结构

```
├── CSGO-Source-Linux-20260928/   # 源码树（Valve CS:GO 2019 Linux 源码 + 本移植的修改）
│   ├── android/native/           # libmain 入口层：生命周期/D3D9 窗口/资源根解析/probe
│   ├── ohos/                     # boot（种子资源根）+ overlay（CSNO 0.2.3 同步资源）
│   ├── scripts/                  # Windows 侧构建：napi / 资源装配 / hvigor 打包
│   └── src/                      # 引擎源码（不含 src/lib 预编译库，构建需自备，见下）
├── hap/                          # HarmonyOS HAP 工程（ArkTS 启动页 + NAPI 桥 + XComponent）
├── scripts/                      # WSL 侧驱动：交叉编译 / stage / 一键迭代 iterate.sh
├── deps/                         # dxvk-ohos-legacy（DXVK 移植）、SDL（OHOS 后端补丁）
└── docs/                         # 战报 / 知识库
```

## 构建链路

前提：WSL2（Ubuntu 24.04）+ OHOS SDK 26.0.0（Linux 版 command-line-tools）+
DevEco Studio（Windows 侧，hdc/hvigor/JBR）+ 真机（开发者模式，hdc 连接）。

```bash
# 1. 同步 E:→WSL 源码补丁（修改过的引擎文件列表维护在脚本内）
bash scripts/wsl-sync.sh

# 2. 交叉编译引擎（vpc → foundation → engine-deps → text-stack → engine 26 模块）
BUILD_JOBS=5 bash scripts/build-ohos-engine.sh engine
BUILD_JOBS=5 bash scripts/build-ohos-engine.sh native   # libmain.so
BUILD_JOBS=5 bash scripts/build-ohos-engine.sh stage    # 产物回传 E: 并组装 90 库

# 3. Windows 侧打包（napi + 资源 + hvigor 签名，versionCode 自动+1）
cd CSGO-Source-Linux-20260928 && bash scripts/build-ohos.sh package

# 4. 装机/启动/日志（或直接用一键迭代）
hdc install -r hap/entry/build/default/outputs/default/entry-default-signed.hap
hdc shell "aa start -a EntryAbility -b com.csgosource.ohos"

# 一键迭代（WSL 内，增量编译→打包→装机→启动→抓日志，约 2 分钟/轮）
BUILD_JOBS=5 bash scripts/iterate.sh engine
```

**src/lib 预编译库**（约 500MB，Valve 原始 source drop 附带，本仓库未收录）：
`libprotobuf.a`、`libcryptopp.a`、`tier1_client.a` 等 androidarm64 静态库，
与基础源码树同源获取后放入 `CSGO-Source-Linux-20260928/src/lib/`。

**游戏资源**：通过启动页导入（本地 zip），内容为 Steam depot
`730 / depot 731 / manifest 8472803367147551147`（CSNO 锁定的 kisak-strike 2019 版本），
需自备 Steam 账号用 [DepotDownloader](https://github.com/steamre/depotdownloader) 下载。

## 免责声明

本项目用于平台移植技术研究与学习。Counter-Strike、Source Engine 及相关资产
版权归 Valve Corporation 所有，本仓库不包含、也不分发任何游戏资产（vpk/模型/贴图）。
构建与运行需要用户自备合法获取的源码与资源。

## License

本仓库中原创的移植代码（android/native 兼容层、hap 工程、scripts、ohos 配置）
以 MIT 许可发布。`CSGO-Source-Linux-20260928/src` 下的引擎源码遵循其原始许可条款，
`deps/dxvk-ohos-legacy` 遵循 DXVK 的 LGPL-2.1 许可。
