#!/usr/bin/env bash
set -euo pipefail

# CS:GO HarmonyOS 构建脚本（Windows 侧：资源装配 + HAP 打包 + 装机）
# 引擎 .so 的交叉编译在 WSL 内进行：scripts/build-ohos-engine.sh
# 用法: build-ohos.sh [build|native|package|install|logs|resources|prebuilt]

# 源码树即本脚本所在仓库
source_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo_dir=$(cd -- "$source_root/.." && pwd)
config=${BUILD_CONFIG:-release}
[[ $config == debug || $config == release ]] || { echo 'BUILD_CONFIG must be debug or release.' >&2; exit 2; }
jobs=${BUILD_JOBS:-6}

# 命令行工具（hvigorw + node，用于无 DevEco 的命令行 HAP 构建）
CLI_TOOLS="${CLI_TOOLS:-E:/ohos-cli/command-line-tools}"
# PackageHap/SignHap 需要 Java（DevEco 自带 JBR）
JAVA_HOME="${JAVA_HOME:-C:/Program Files/Huawei/DevEco Studio/jbr}"

# OHOS SDK 路径（Windows）
OHOS_SDK="${OHOS_SDK:-C:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native}"
OHOS_TOOLCHAIN="$OHOS_SDK/build/cmake/ohos.toolchain.cmake"
OHOS_CMAKE="$OHOS_SDK/build-tools/cmake/bin/cmake.exe"
OHOS_NINJA="$OHOS_SDK/build-tools/cmake/bin/ninja.exe"
OHOS_CLANG="$OHOS_SDK/llvm/bin/clang.exe"
OHOS_CLANGXX="$OHOS_SDK/llvm/bin/clang++.exe"

# 输出目录
out="$source_root/runtime/ohos"
prefix="$out/install"
variant="$out/$config"
mkdir -p "$variant"

# HAP 工程（在源码树外层 E:\csgo\hap）
hap_dir="$repo_dir/hap"
hap_libs="$hap_dir/entry/libs/arm64-v8a"

# 预编译依赖
sdl3_so="$repo_dir/deps/SDL/build.ohos/libSDL3.so.0.5.0"
dxvk_so="$repo_dir/deps/dxvk-ohos-legacy/build.ohos/src/d3d9/d3d9.so"

# 源码树（= 本脚本所在仓库，见文件头 source_root；此别名保留给下游引用）
android_dir="$source_root/android"

die() { echo "[csgo-ohos] $*" >&2; exit 1; }
need() { [[ -f $1 ]] || die "Missing file: $1"; }

# ── 阶段 1: 复制预编译依赖到 HAP libs ──────────────────────────
# 注意：完整的 HAP 库组装（引擎 26 模块 + 文本栈 + v8 桩 + libc++_shared 等 90 个）
# 由 WSL 内的 scripts/build-ohos-engine.sh stage 完成，本函数只做 SDL3/DXVK 兜底。
stage_prebuilt() {
    mkdir -p "$hap_libs"
    echo "[csgo-ohos] Staging prebuilt libraries..."

    # SDL3
    need "$sdl3_so"
    cp -p "$sdl3_so" "$hap_libs/libSDL3.so"
    echo "[csgo-ohos]   libSDL3.so ($(du -h "$sdl3_so" | cut -f1))"

    # DXVK D3D9
    need "$dxvk_so"
    cp -p "$dxvk_so" "$hap_libs/libdxvk_d3d9.so"
    echo "[csgo-ohos]   libdxvk_d3d9.so ($(du -h "$dxvk_so" | cut -f1))"

    # 引擎 .so（需预先在 Linux/WSL 中编译）
    local engine_libs="$source_root/game/bin/androidarm64/$config"
    local source_libs="$source_root/src/lib/public/androidarm64/$config"

    # libcsgo_android.so → libmain.so（SDL3 dlopen 约定）
    if [[ -f "$engine_libs/libcsgo_android.so" ]]; then
        cp -p "$engine_libs/libcsgo_android.so" "$hap_libs/libmain.so"
        echo "[csgo-ohos]   libmain.so (from libcsgo_android.so)"
    else
        echo "[csgo-ohos]   WARNING: libmain.so not found (engine not built yet)"
    fi

    # 引擎依赖 .so
    for so in libtier0_client.so libvstdlib_client.so libfilesystem_stdio_client.so; do
        if [[ -f "$engine_libs/$so" ]]; then
            cp -p "$engine_libs/$so" "$hap_libs/"
            echo "[csgo-ohos]   $so"
        fi
    done

    # Steam Audio
    local phonon="$prefix/lib/libphonon.so"
    if [[ -f "$phonon" ]]; then
        cp -p "$phonon" "$hap_libs/"
        echo "[csgo-ohos]   libphonon.so"
    fi

    # V8
    local v8_dir="$source_root/src/lib/common/androidarm64"
    for so in libv8.cr.so libv8_libbase.cr.so libv8_libplatform.cr.so libicuuc.cr.so libicui18n.cr.so; do
        if [[ -f "$v8_dir/$so" ]]; then
            cp -p "$v8_dir/$so" "$hap_libs/"
            echo "[csgo-ohos]   $so"
        fi
    done

    # Pango/Cairo（Panorama 文本渲染）
    for so in libpango-1.0.so libcairo.so libpixman-1.so libfreetype.so; do
        if [[ -f "$prefix/lib/$so" ]]; then
            cp -p "$prefix/lib/$so" "$hap_libs/"
            echo "[csgo-ohos]   $so"
        fi
    done

    echo "[csgo-ohos] Prebuilt staging complete."
}

# ── 阶段 2: 构建 NAPI entry 模块 ──────────────────────────────
build_napi() {
    echo "[csgo-ohos] Building NAPI entry module..."
    local napi_build="$variant/napi-build"
    mkdir -p "$napi_build"

    "$OHOS_CMAKE" -S "$hap_dir/entry/src/main/cpp" -B "$napi_build" -G Ninja \
        "-DCMAKE_TOOLCHAIN_FILE=$OHOS_TOOLCHAIN" \
        -DOHOS_ARCH=arm64-v8a \
        -DCMAKE_BUILD_TYPE=Release

    "$OHOS_CMAKE" --build "$napi_build" --parallel "$jobs"

    # 不拷入 hap_libs：hvigor externalNativeOptions 会从 src/main/cpp 自编 libentry.so，
    # 两份同名 .so 会触发 ProcessLibs 00306049 Duplicated files
    echo "[csgo-ohos]   libentry.so (built for sanity check only; hvigor packages its own)"
}

# ── 阶段 3: 复制游戏资源到 HAP rawfile ────────────────────────
# 布局：rawfile/csgo/ = Source 游戏根（对应沙箱 files/csgo，引擎 -basedir 此目录），
# 其下必须有 csgo/ 子目录（引擎 -game csgo → <basedir>/csgo/gameinfo.txt）、
# platform/、dxvk.conf（CWD=游戏根被 DXVK 读取）、mobile_ui/（引擎触控 UI 资产）。
# 顺序：boot 先、overlay 后覆盖（与 CSNO 装机顺序一致）。
stage_resources() {
    echo "[csgo-ohos] Staging game resources..."
    local rawfile="$hap_dir/entry/src/main/resources/rawfile"
    rm -rf "$rawfile"

    local overlay="$source_root/ohos/overlay"
    local boot="$source_root/ohos/boot"
    [[ -d "$boot/csgo" ]] || die "Missing $boot/csgo"
    mkdir -p "$rawfile"

    # boot 整体即游戏根：csgo/ platform/ dxvk.conf SAVE/
    cp -R "$boot" "$rawfile/csgo"
    # overlay 覆盖到游戏根的 csgo/ 子目录
    if [[ -d "$overlay/csgo" ]]; then
        cp -R "$overlay/csgo/." "$rawfile/csgo/csgo/"
    fi
    # OHOS：资源包的 panorama/code.pbin 是 CS2 时代布局（CCSGOTabletPanoLayer
    # 等本引擎未注册的类），pbin 优先级又高于散装文件 → HUD 必崩。
    # DEVELOPMENT_ONLY 构建走散装布局，pbin 一律剥离。
    rm -f "$rawfile/csgo/csgo/panorama/code.pbin"
    [[ -f "$overlay/dxvk.conf" ]] && cp -p "$overlay/dxvk.conf" "$rawfile/csgo/dxvk.conf"
    [[ -f "$overlay/steam_appid.txt" ]] && cp -p "$overlay/steam_appid.txt" "$rawfile/csgo/"

    # fontconfig OHOS 专用配置：默认 fonts.conf 会扫 /system/fonts 等系统目录，
    # 沙箱里 FcFontList 打转。只给游戏自带字体目录 + 可写缓存目录。
    local fc_src="$source_root/ohos/boot/fontconfig/fonts.conf"
    if [[ -f "$fc_src" ]]; then
        rm -rf "$rawfile/csgo/fontconfig"
        mkdir -p "$rawfile/csgo/fontconfig"
        cp -p "$fc_src" "$rawfile/csgo/fontconfig/fonts.conf"
        echo "[csgo-ohos]   fontconfig/fonts.conf packaged"
    fi

    # mobile_ui（引擎内建触控 UI 的资产包）：安卓从 APK assets 读，OHOS 由 ArkTS
    # 解包到沙箱 files/csgo/mobile_ui，引擎从 ResourceRoot/mobile_ui 读
    local mobile_ui="$source_root/android/app/src/main/assets/mobile_ui"
    if [[ -d "$mobile_ui" ]]; then
        rm -rf "$rawfile/csgo/mobile_ui"
        cp -R "$mobile_ui" "$rawfile/csgo/mobile_ui"
        # 生成 manifest.txt（与安卓打包一致的相对路径清单，供引擎逐文件提取）
        ( cd "$rawfile/csgo/mobile_ui" && find . -type f ! -name manifest.txt \
            | sed 's|^\./||' | sort > manifest.txt )
        echo "[csgo-ohos]   mobile_ui packaged ($(wc -l < "$rawfile/csgo/mobile_ui/manifest.txt") assets)"
    else
        echo "[csgo-ohos]   WARNING: mobile_ui assets missing - engine touch UI will fail"
    fi

    echo "[csgo-ohos] Resource staging complete ($(find "$rawfile" -type f | wc -l) files)."
}

# ── 阶段 4: HAP 打包（命令行 hvigorw）─────────────────────────
package_hap() {
    echo "[csgo-ohos] Packaging HAP..."
    cd "$hap_dir"

    # OHOS install -r 在 versionCode 不变时会静默跳过 native libs 更新（踩坑实证），
    # 每次打包自动 +1 强制完整覆盖
    BUMP_SCRIPT="$source_root/scripts/bump-versioncode.py"
    if [[ -f "$BUMP_SCRIPT" ]]; then
        python3 "$BUMP_SCRIPT" "$hap_dir/AppScope/app.json5" 2>/dev/null \
            || py "$BUMP_SCRIPT" "$hap_dir/AppScope/app.json5" 2>/dev/null \
            || echo "[csgo-ohos]   WARNING: versionCode bump failed"
    fi

    # hvigor 的 intermediates 会缓存旧 native libs（曾导致设备一直跑旧库），每次清掉
    rm -rf "$hap_dir/entry/build/default/intermediates"
    rm -rf "$hap_dir/entry/build/default/outputs"

    # 优先用 commandline-tools 的 hvigorw（无需 DevEco）；回退工程内 wrapper
    local hvigorw="$CLI_TOOLS/bin/hvigorw"
    [[ -f "$hvigorw" ]] || hvigorw="$hap_dir/hvigorw"
    if [[ ! -f "$hvigorw" ]]; then
        echo "[csgo-ohos]   hvigorw not found - use DevEco Studio to build/sign"
        echo "[csgo-ohos]   HAP project path: $hap_dir"
        return 0
    fi

    export JAVA_HOME
    # PATH 里的 Java 必须是 POSIX 形式：hvigor 的 node 进程 spawn java 按 PATH 查找，
    # 混入 "C:\...\jbr/bin" 这类混合分隔符路径会 ENOENT（PackageHap 报 00308018）
    export PATH="$CLI_TOOLS/bin:$CLI_TOOLS/tool/node/bin:/c/Program Files/Huawei/DevEco Studio/jbr/bin:$PATH"
    chmod +x "$hvigorw"
    "$hvigorw" --mode module -p product=default assembleHap --no-daemon

    local hap_out="$hap_dir/entry/build/default/outputs/default"
    local hap_file
    hap_file=$(find "$hap_out" -name "*.hap" -type f | head -1)

    if [[ -n "$hap_file" ]]; then
        cp "$hap_file" "$variant/csgo-ohos-$config.hap"
        echo "[csgo-ohos]   HAP: $variant/csgo-ohos-$config.hap"
    else
        echo "[csgo-ohos]   HAP build may have failed - check DevEco Studio"
    fi
}

# ── 阶段 5: 安装到设备（hdc）──────────────────────────────────
install_hap() {
    local hap_file="$variant/csgo-ohos-$config.hap"
    need "$hap_file"

    local hdc="${HDC_PATH:-hdc}"
    "$hdc" install "$hap_file"
    echo "[csgo-ohos] Installed. Launch with:"
    echo "[csgo-ohos]   hdc shell aa start -a EntryAbility -b com.csgosource.ohos"
}

# ── 阶段 6: 查看日志 ──────────────────────────────────────────
show_logs() {
    local hdc="${HDC_PATH:-hdc}"
    "$hdc" shell hilog | grep -E "CSGOHOS|SDL|DXVK|CSGO"
}

# ── 主入口 ────────────────────────────────────────────────────
action=${1:-build}
if (($#)); then shift; fi
case "$action" in
    build)
        stage_prebuilt
        build_napi
        stage_resources
        package_hap
        ;;
    prebuilt) stage_prebuilt ;;
    napi) build_napi ;;
    resources) stage_resources ;;
    package) stage_resources; package_hap ;;
    install) install_hap ;;
    logs) show_logs ;;
    all)
        stage_prebuilt
        build_napi
        stage_resources
        package_hap
        install_hap
        ;;
    *)
        die 'Usage: build-ohos.sh [build|prebuilt|napi|resources|package|install|logs|all]'
        ;;
esac