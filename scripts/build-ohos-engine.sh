#!/usr/bin/env bash
set -euo pipefail

# CS:GO OHOS 引擎交叉构建（在 WSL Ubuntu-24.04 内运行）
# 用法: build-ohos-engine.sh [vpc|foundation|engine-deps|text-stack|engine|native|stage]
# 源码构建区: ~/csgo-src (WSL ext4, vhdx 在 E:\WSL)，产物回传 E:\csgo 树
# 工具链: ~/ohos-native (来自 E:\ohos-cli commandline-tools 26.0.0.851)

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
repo=${CSGO_SOURCE_ROOT:-$root/CSGO-Source-Linux-20260928}
src="$repo/src"
out="$repo/runtime/ohos"
prefix=${DEPS_PREFIX:-$out/install}
config=${BUILD_CONFIG:-release}
jobs=${BUILD_JOBS:-4}

export OHOS_SDK=${OHOS_SDK:-$HOME/ohos-native}
OHOS_TARGET_TRIPLE=aarch64-linux-ohos
# makefile_base_posix.mak 的 ANDROID 分支参数（?=/env 可覆盖）
export ANDROID_TOOLCHAIN="$OHOS_SDK/llvm"
export ANDROID_TARGET="$OHOS_TARGET_TRIPLE"
export ANDROID_NDK_ROOT="$OHOS_SDK"          # VPC 需要 ANDROID_NDK_DIR 非空（OHOS 不用其布局）
export ANDROID_PLATFORM=30
export SSE2NEON_DIR="$out/deps/sse2neon"
export DEPS_PREFIX="$prefix"
# The source dependency builder supplies a current host executable, not i386.
export PROTOC=${PROTOC:-${CSGO_DEPS_WORK:-$out/dependency-build}/host/protobuf/src/protoc}
export ENGINE_SYSLIBS="-lm -ldl"
export ENGINE_SYSLIBS_SHLIB="-lm -ldl"

# 交叉编译命令必须显式带 target+sysroot（工具链文件只覆盖 cmake 路径；
# 手动 cc/Makefile 构建若不带 --target 会编成宿主 x86-64）
OHOS_CC="$ANDROID_TOOLCHAIN/bin/clang --target=aarch64-linux-ohos --sysroot=$OHOS_SDK/sysroot"
OHOS_CXX="$ANDROID_TOOLCHAIN/bin/clang++ --target=aarch64-linux-ohos --sysroot=$OHOS_SDK/sysroot"
OHOS_AR="$ANDROID_TOOLCHAIN/bin/llvm-ar"
OHOS_RANLIB="$ANDROID_TOOLCHAIN/bin/llvm-ranlib"
OHOS_TOOLCHAIN_CMAKE="$OHOS_SDK/build/cmake/ohos.toolchain.cmake"
# Archives do not always preserve executable bits on the helper script.
export GEN_SYM="OBJCOPY=$ANDROID_TOOLCHAIN/bin/llvm-objcopy bash $src/devtools/gendbg.sh"
# SDK 自带 cmake（认识 Ohos 平台）+ ninja；放在 PATH 最前
export PATH="$OHOS_SDK/build-tools/cmake/bin:$PATH"
CROSS_CFLAGS="-O2 -g -fPIC -fsigned-char -fno-strict-aliasing -fno-fast-math -ffp-contract=off"
CROSS_CXXFLAGS="$CROSS_CFLAGS -stdlib=libc++"

log() { echo "[csgo-ohos-engine] $*"; }
die() { echo "[csgo-ohos-engine] FATAL: $*" >&2; exit 1; }

# ── sse2neon ─────────────────────────────────────────────────
ensure_sse2neon() {
    if [[ ! -f "$SSE2NEON_DIR/sse2neon.h" ]]; then
        mkdir -p "$(dirname "$SSE2NEON_DIR")"
        rm -rf "$SSE2NEON_DIR"
        git clone --depth 1 https://github.com/DLTcollab/sse2neon.git "$SSE2NEON_DIR"
        git -C "$SSE2NEON_DIR" fetch --depth 1 origin 8d1d9f1cae82de66d9daea53f4f7ca30024f478b
        git -C "$SSE2NEON_DIR" checkout --detach 8d1d9f1cae82de66d9daea53f4f7ca30024f478b
    fi
    if ! git -C "$SSE2NEON_DIR" apply --reverse --check "$repo/android/patches/sse2neon-android.patch" 2>/dev/null; then
        git -C "$SSE2NEON_DIR" apply "$repo/android/patches/sse2neon-android.patch" \
            || log "sse2neon patch already applied or failed (continuing)"
    fi
}

# ── VPC (宿主工具) ───────────────────────────────────────────
do_vpc() {
    log "Building host VPC..."
    make -C "$src/utils/vpc" -j"$jobs" CC=clang CXX=clang++ OUTDIR=obj/Linux/clang-release
    cp -p "$src/utils/vpc/obj/Linux/clang-release/vpc" "$src/devtools/bin/vpc_linux"
    log "VPC -> $src/devtools/bin/vpc_linux"
}

# ── 8.2 引擎基础库 ───────────────────────────────────────────
do_foundation() {
    ensure_sse2neon
    (
        cd "$src"
        ANDROID_NDK_ROOT="$ANDROID_NDK_ROOT" ANDROID_PLATFORM=30 ./devtools/bin/vpc_linux \
            /csgo /androidarm64 +tier0 +tier1 +mathlib +interfaces +vstdlib \
            +tier2 +vpklib +filesystem_stdio +bitmap +vtf \
            /nop4add /mksln csgo_android_base /f
        make -f csgo_android_base.mak -j"$jobs" --output-sync=target \
            CFG="$config" VALVE_NO_AUTO_P4=1 all-targets
    )
    log "Foundation outputs:"
    ls -la "$src/lib/public/androidarm64/$config/" 2>/dev/null || true
}

# ── 8.3 引擎依赖库 ───────────────────────────────────────────
do_engine_deps() {
    local libraries="$src/lib/public/androidarm64/$config"
    local protobuf="$src/thirdparty/protobuf-2.5.0"
    local cryptopp="$out/cryptopp-build"
    mkdir -p "$libraries" "$out/protobuf-build" "$cryptopp" "$prefix/lib" "$prefix/include"

    local mbedtls="$out/deps/mbedtls" curl="$out/deps/curl" jpeg="$out/deps/libjpeg-turbo" freetype="$out/deps/freetype" expat="$out/deps/libexpat"
    # 依赖源码 checkout 放最前：网络失败时尽早暴露，避免白跑前面的构建
    ensure_git_checkout "$mbedtls" https://github.com/Mbed-TLS/mbedtls.git 068ff080b369adfac81509f9b57b2afabaf82dc5
    ensure_git_checkout "$curl" https://github.com/curl/curl.git 01346829096c61b372692f6dc43ffa778c6caccd
    ensure_git_checkout "$jpeg" https://github.com/libjpeg-turbo/libjpeg-turbo.git af9c1c268520a29adf98cad5138dafe612b3d318
    ensure_git_checkout "$freetype" https://gitlab.freedesktop.org/freetype/freetype.git 0a0221a1347e2f1e07c395263540026e9a0aa7c7
    ensure_git_checkout "$expat" https://github.com/libexpat/libexpat.git R_2_5_0

    # protobuf 2.5.0（host protoc 由树内 linux 版提供）
    (
        cd "$out/protobuf-build"
        if [[ ! -f Makefile ]]; then
            CC="$OHOS_CC" CXX="$OHOS_CXX" AR="$OHOS_AR" RANLIB="$OHOS_RANLIB" \
                CFLAGS="$CROSS_CFLAGS" CXXFLAGS="$CROSS_CXXFLAGS -std=c++11" \
                "$protobuf/configure" --host=aarch64-linux-gnu --build=x86_64-pc-linux-gnu \
                --disable-shared --enable-static \
                --with-protoc="$src/devtools/bin/linux/protoc"
        fi
        make -C src -j"$jobs" libprotobuf.la
        rm -f "$libraries/libprotobuf.a" # prebuilt-tree symlink; replace with our build
        cp -p src/.libs/libprotobuf.a "$libraries/libprotobuf.a"
    )

    # crypto++ 5.61
    rsync -a --include='/*.cpp' --include='/*.h' --exclude='*' \
        "$src/external/crypto++-5.61/" "$cryptopp/"
    python3 - "$src/external/crypto++-5.61/GNUmakefile" "$cryptopp/GNUmakefile" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
source = '\n'.join(line for line in source.splitlines()
                   if not line.startswith('\tp4 edit ') and not line.startswith('\t$(CP) libcryptopp.a ../../lib/')) + '\n'
destination = pathlib.Path(sys.argv[2])
if not destination.exists() or destination.read_text() != source:
    destination.write_text(source)
PY
    make -C "$cryptopp" -j"$jobs" libcryptopp.a \
        CXX="$OHOS_CXX" AR="$OHOS_AR" ARFLAGS=-cr RANLIB="$OHOS_RANLIB" IS_SUN_CC=0 \
        CXXFLAGS="-DNDEBUG -DCRYPTOPP_DISABLE_ASM $CROSS_CXXFLAGS -std=c++11"
    rm -f "$libraries/libcryptopp.a" # prebuilt-tree symlink; replace with our build
    cp -p "$cryptopp/libcryptopp.a" "$libraries/libcryptopp.a"

    # mbedtls + curl + jpeg + freetype（cmake + OHOS 工具链）
    local cmake_args=(
        -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$OHOS_TOOLCHAIN_CMAKE" -DOHOS_ARCH=arm64-v8a
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        "-DCMAKE_INSTALL_PREFIX=$prefix" "-DCMAKE_PREFIX_PATH=$prefix"
    )

    cmake -S "$mbedtls" -B "$out/mbedtls-build" "${cmake_args[@]}" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
        -DMBEDTLS_FATAL_WARNINGS=OFF
    cmake --build "$out/mbedtls-build" --parallel "$jobs"
    cmake --install "$out/mbedtls-build"

    # expat（panorama/parsifal 依赖，Android 用预编译，这里从源码构建）
    local expat="$out/deps/libexpat"
    ensure_git_checkout "$expat" https://github.com/libexpat/libexpat.git R_2_5_0
    cmake -S "$expat/expat" -B "$out/expat-build" "${cmake_args[@]}" \
        -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
        -DEXPAT_SHARED_LIBS=ON -DEXPAT_STATIC_LIBS=OFF
    cmake --build "$out/expat-build" --parallel "$jobs"
    cmake --install "$out/expat-build"

    cmake -S "$curl" -B "$out/curl-build" "${cmake_args[@]}" \
        -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
        -DHTTP_ONLY=ON -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
        -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBIDN2=OFF \
        -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF
    cmake --build "$out/curl-build" --parallel "$jobs"
    cmake --install "$out/curl-build"

    cmake -S "$jpeg" -B "$out/jpeg-build" "${cmake_args[@]}" \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_JPEG8=ON
    cmake --build "$out/jpeg-build" --parallel "$jobs" --target jpeg-static
    cp -p "$out/jpeg-build/libjpeg.a" "$prefix/lib/libjpeg.a"

    # freetype：共享库（vgui/panorama 字体栅格化）
    cmake -S "$freetype" -B "$out/freetype-build" "${cmake_args[@]}" -DBUILD_SHARED_LIBS=ON \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    cmake --build "$out/freetype-build" --parallel "$jobs"
    cmake --install "$out/freetype-build"

    # libpng 1.5.2（树内源码，直接编译，链 sysroot libz）
    local png="$src/thirdparty/libpng-1.5.2" pngout="$src/lib/androidarm64/release"
    mkdir -p "$pngout" "$out/png-obj"
    for f in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil; do
        $OHOS_CC -O2 -g -fPIC -fvisibility=hidden -I"$png" -I"$src/thirdparty/zlib-1.2.5" \
            -c "$png/$f.c" -o "$out/png-obj/$f.o"
    done
    "$OHOS_AR" crs "$pngout/libpng.a" "$out/png-obj/"*.o

    # parsifal（保留回调 ABI，expat 提供 XML 解析）
    local parsifal="$src/thirdparty/libparsifal-0.8.3" libcommon="$src/lib/common/androidarm64"
    mkdir -p "$libcommon" "$out/parsifal-obj"
    $OHOS_CC -O2 -g -fPIC -fvisibility=hidden -I"$parsifal/include" -I"$prefix/include" \
        -c "$repo/android/native/parsifal_expat.c" -o "$out/parsifal-obj/parsifal.o"
    "$OHOS_AR" crs "$libcommon/libparsifal.a" "$out/parsifal-obj/parsifal.o"

    # jpeglib_client.a：client/server/panorama 链接的名字（同 libjpeg.a，JPEG8 ABI）
    cp -p "$prefix/lib/libjpeg.a" "$src/lib/common/androidarm64/$config/jpeglib_client.a"

    # bzip2 1.0.8（client_panorama 链接 $LIBCOMMON/bzip2_client.a；树内无预编译版）
    local bzip2="$out/deps/bzip2-1.0.8"
    if [[ ! -f "$bzip2/bzlib.h" ]]; then
        mkdir -p "$(dirname "$bzip2")"
        curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --connect-timeout 20 \
            -o "$out/deps/bzip2.tar.gz" https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz \
            && tar -C "$(dirname "$bzip2")" -xzf "$out/deps/bzip2.tar.gz" \
            || die "bzip2 download failed"
    fi
    if [[ ! -f "$libcommon/bzip2_client.a" ]]; then
        mkdir -p "$out/bzip2-obj"
        for f in blocksort huffman crctable randtable compress decompress bzlib; do
            $OHOS_CC -O2 -g -fPIC -D_FILE_OFFSET_BITS=64 -c "$bzip2/$f.c" -o "$out/bzip2-obj/$f.o"
        done
        "$OHOS_AR" crs "$libcommon/bzip2_client.a" "$out/bzip2-obj/"*.o
    fi

    # OHOS disables Steam Audio calls at compile time; do not fabricate callable exports.
    build_phonon_disabled

    # -landroid 空档案：vgui_surfacelib 的 [$ANDROIDALL] SystemLibraries 含 android，
    # OHOS 无此库；空 archive 让 lld 满足 -landroid 而不引入任何符号
    "$OHOS_AR" crs "$prefix/lib/libandroid.a"

    log "engine-deps done. prefix/lib:"
    ls "$prefix/lib"
}

ensure_git_checkout() {
    local directory=$1 url=$2 revision=$3
    if [[ ! -d $directory/.git && ! -f $directory/.ohos-rev ]]; then
        rm -rf "$directory"
        local ok=0
        for attempt in 1 2 3 4 5; do
            git clone "$url" "$directory" && { ok=1; break; }
            rm -rf "$directory"
            sleep $((attempt * 3))
        done
        # TLS 对大仓库不稳定时的兜底：codeload tarball（github）或 gitlab archive
        if (( ! ok )); then
            log "git clone failed, falling back to tarball: $url"
            mkdir -p "$directory"
            if [[ $url == https://github.com/* ]]; then
                local slug=${url#https://github.com/} slug=${slug%.git}
                curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 \
                    -o "$directory.tgz" "https://codeload.github.com/$slug/tar.gz/$revision" \
                    && tar -C "$directory" --strip-components=1 -xzf "$directory.tgz" \
                    && rm -f "$directory.tgz" && ok=1
            elif [[ $url == https://gitlab.freedesktop.org/* ]]; then
                local slug=${url#https://gitlab.freedesktop.org/} slug=${slug%.git}
                curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 \
                    -o "$directory.tgz" "https://gitlab.freedesktop.org/$slug/-/archive/$revision/$(basename "$slug")-$revision.tar.gz" \
                    && tar -C "$directory" --strip-components=1 -xzf "$directory.tgz" \
                    && rm -f "$directory.tgz" && ok=1
            fi
            (( ok )) || die "git clone and tarball fallback both failed: $url"
            echo "$revision" > "$directory/.ohos-rev"
        fi
    fi
    if [[ -d $directory/.git ]]; then
        if [[ $(git -C "$directory" rev-parse HEAD 2>/dev/null) != "$revision" ]]; then
            git -C "$directory" fetch origin "$revision" 2>/dev/null || true
            git -C "$directory" checkout --detach "$revision"
        fi
        git -C "$directory" submodule update --init --recursive 2>/dev/null || true
    fi
}

# phonon stub：Steam Audio 2.0 beta20 SDK 头（ANDROID 分支用 steam_audio/phonon.h）
# + 从中提取全部 ipl* 符号生成汇编空实现 .so（链接器只看符号名；snd_dma.cpp 的
# phonon 初始化在 ANDROID 构建下是死代码，运行期 no-op 安全）
build_phonon_disabled() {
    local phonon_h="$src/public/phonon/phonon.h"
    local sdk_zip="$out/deps/steamaudio_api_2.0-beta.20.zip"
    local stub_dir="$out/phonon-stub"
    mkdir -p "$stub_dir" "$prefix/lib" "$prefix/include/steam_audio"
    if [[ ! -f "$sdk_zip" ]]; then
        curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --connect-timeout 20 \
            -o "$sdk_zip.download" \
            https://github.com/ValveSoftware/steam-audio/releases/download/v2.0-beta.20/steamaudio_api_2.0-beta.20.zip \
            && mv "$sdk_zip.download" "$sdk_zip"
    fi
    python3 - "$sdk_zip" "$prefix" <<'PY'
import hashlib, pathlib, sys, zipfile
archive, prefix = map(pathlib.Path, sys.argv[1:])
expected = '284b7d3b9a5ee744951c9138835c342a93bb2b08a43a253f54956ab6ff70fdd6'
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit('Steam Audio SDK archive checksum mismatch')
with zipfile.ZipFile(archive) as sdk:
    for member, target in (
        ('steamaudio_api/include/phonon.h', 'include/steam_audio/phonon.h'),
        ('steamaudio_api/include/phonon_version.h', 'include/steam_audio/phonon_version.h'),
    ):
        destination = prefix / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(sdk.read(member))
print('steam_audio headers installed')
PY
    # The __OHOS__ engine path must not reference ipl* at all. An empty static
    # archive satisfies the legacy -lphonon flag without publishing unsafe stubs.
    [[ ! -f "$prefix/lib/libphonon.so" ]] || die "Remove obsolete generated libphonon.so before rebuilding"
    "$OHOS_AR" crs "$prefix/lib/libphonon.a"
    log "Steam Audio disabled on OHOS; ordinary stereo mixer remains available"
}

# ── Panorama 文本栈（pango/glib/harfbuzz/fontconfig/cairo）──
# 依赖顺序：expat(已建于 engine-deps) → fribidi → libffi → pcre2 → glib →
#           harfbuzz → fontconfig → pixman → cairo → pango
# 全部 meson 构建、共享库、安装进 $prefix（.pc 供 pkg-config 发现）。
do_text_stack() {
    local cross_file="$out/meson-cross-ohos.txt"
    mkdir -p "$out" "$prefix/lib/pkgconfig"
    cat > "$cross_file" <<EOF
[binaries]
c = ['$ANDROID_TOOLCHAIN/bin/clang', '--target=aarch64-linux-ohos', '--sysroot=$OHOS_SDK/sysroot']
cpp = ['$ANDROID_TOOLCHAIN/bin/clang++', '--target=aarch64-linux-ohos', '--sysroot=$OHOS_SDK/sysroot']
ar = '$ANDROID_TOOLCHAIN/bin/llvm-ar'
strip = '$ANDROID_TOOLCHAIN/bin/llvm-strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-fPIC', '-fsigned-char', '-fno-strict-aliasing', '-fno-fast-math', '-ffp-contract=off']
cpp_args = ['-fPIC', '-fsigned-char', '-fno-strict-aliasing', '-fno-fast-math', '-ffp-contract=off', '-stdlib=libc++']
c_link_args = ['-fuse-ld=lld', '-Wl,-z,max-page-size=16384', '-Wl,--build-id=sha1']
cpp_link_args = ['-fuse-ld=lld', '-stdlib=libc++', '-Wl,-z,max-page-size=16384', '-Wl,--build-id=sha1']

[properties]
pkg_config_libdir = '$prefix/lib/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

    # type|name|location|tag-or-dirname|extra meson args
    # tarball 的 location 是 URL，tag-or-dirname 是解压后目录名；git 的 location 是仓库，tag-or-dirname 是 tag
    local projects=(
        "tarball|fribidi|https://github.com/fribidi/fribidi/archive/refs/tags/v1.0.15.tar.gz|fribidi-1.0.15|-Ddocs=false -Dtests=false"
        "autoconf|libffi|https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz|libffi-3.4.6|"
        "cmake|pcre2|https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.45/pcre2-10.45.tar.gz|pcre2-10.45|-DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF -DPCRE2_SUPPORT_JIT=ON"
        "git|glib|https://github.com/GNOME/glib.git|2.80.4|-Dnls=disabled -Dman=false -Dtests=false -Dinstalled_tests=false -Dlibmount=disabled -Dselinux=disabled -Dsysprof=disabled -Dglib_debug=disabled -Dglib_assert=false -Dglib_checks=true"
        "tarball|harfbuzz|https://github.com/harfbuzz/harfbuzz/archive/refs/tags/8.5.0.tar.gz|harfbuzz-8.5.0|-Dtests=disabled -Ddocs=disabled -Dbenchmark=disabled -Dicu=disabled -Dglib=disabled -Dgobject=disabled -Dfreetype=enabled"
        "git|fontconfig|https://gitlab.freedesktop.org/fontconfig/fontconfig.git|2.15.0|-Dtests=disabled -Ddoc=disabled -Ddoc-man=disabled -Dnls=disabled -Dtools=disabled -Dcache-build=disabled"
        "git|pixman|https://gitlab.freedesktop.org/pixman/pixman.git|pixman-0.44.2|-Dtests=disabled -Ddemos=disabled"
        "git|cairo|https://gitlab.freedesktop.org/cairo/cairo.git|1.18.2|-Dtests=disabled -Dxlib=disabled -Dxcb=disabled -Dxlib-xcb=disabled -Dgtk2-utils=disabled -Dgtk_doc=false -Dtee=disabled -Ddwrite=disabled -Dquartz=disabled -Dspectre=disabled -Dsymbol-lookup=disabled -Dpng=disabled -Dzlib=disabled -Dglib=disabled"
        "tarball|pango|https://github.com/GNOME/pango/archive/refs/tags/1.54.0.tar.gz|pango-1.54.0|-Dintrospection=disabled -Ddocumentation=false -Dbuild-testsuite=false -Dbuild-examples=false -Dgtk_doc=false -Dlibthai=disabled -Dsysprof=disabled -Dxft=disabled"
    )

    export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
    export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
    command -v gperf >/dev/null || die "gperf missing (needed by fontconfig): apt install gperf"

    for entry in "${projects[@]}"; do
        IFS='|' read -r kind name location dirname extra <<<"$entry"
        local srcdir="$out/textstack/$dirname" builddir="$out/textstack-build/$name"

        # cairo git tag 的 include 顺序 bug：cairo-ft-private.h 使用 FT_Color 但未包含
        # FT_COLOR_H（发版 tarball 有条件包含，git tag 没有）。幂等补丁，重克隆后自愈。
        if [[ $name == cairo && -f "$srcdir/src/cairo-ft-private.h" ]]; then
            python3 - "$srcdir" <<'PY'
import io, sys
root = sys.argv[1]
h = root + '/src/cairo-ft-private.h'
s = io.open(h, encoding='utf-8').read()
if 'FT_COLOR_H' not in s:
    s = s.replace('#define CAIRO_FT_PRIVATE_H',
                  '#define CAIRO_FT_PRIVATE_H\n#include <ft2build.h>\n#include FT_COLOR_H /* ZCode patch: FT_Color in prototypes below */', 1)
    io.open(h, 'w', encoding='utf-8', newline='').write(s)
c = root + '/src/cairo-ft-font.c'
s = io.open(c, encoding='utf-8').read()
if 'FT_COLOR_H' not in s:
    s = s.replace('#include FT_LCD_FILTER_H',
                  '#include FT_LCD_FILTER_H\n#include FT_COLOR_H /* ZCode patch */', 1)
    io.open(c, 'w', encoding='utf-8', newline='').write(s)
print('cairo patches ensured')
PY
        fi
        local stack_cmake_args=(
            -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$OHOS_TOOLCHAIN_CMAKE" -DOHOS_ARCH=arm64-v8a
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            "-DCMAKE_INSTALL_PREFIX=$prefix" "-DCMAKE_PREFIX_PATH=$prefix"
        )

        if [[ $kind == autoconf || $kind == cmake ]]; then
            # 非 meson 库：tarball 需要先取源码
            if [[ ! -f "$srcdir/configure" && ! -f "$srcdir/CMakeLists.txt" ]]; then
                rm -rf "$srcdir"
                mkdir -p "$out/textstack-dl" "$(dirname "$srcdir")"
                local tarball="$out/textstack-dl/$dirname.tar.gz"
                curl -fL --retry 2 --connect-timeout 20 -o "$tarball" "$location"
                tar -C "$(dirname "$srcdir")" -xf "$tarball"
            fi
        fi

        if [[ $kind == autoconf ]]; then
            # libffi 等 meson 之前的库：tarball 自带 configure，交叉配置后 make
            mkdir -p "$builddir"
            (
                cd "$builddir"
                if [[ ! -f Makefile ]]; then
                    CC="$OHOS_CC" CXX="$OHOS_CXX" AR="$OHOS_AR" RANLIB="$OHOS_RANLIB" \
                        CFLAGS="-O2 -g -fPIC" CXXFLAGS="-O2 -g -fPIC" \
                        "$srcdir/configure" --host=aarch64-linux-gnu --build=x86_64-pc-linux-gnu \
                        --prefix="$prefix" --enable-shared --disable-static \
                        --libdir="$prefix/lib"
                fi
                make -j"$jobs"
                make install
            )
            log "text-stack: $name OK"
            continue
        fi

        if [[ $kind == cmake ]]; then
            # pcre2 等：CMake + OHOS 工具链
            # shellcheck disable=SC2086
            cmake -S "$srcdir" -B "$builddir" "${stack_cmake_args[@]}" $extra \
                || die "cmake setup failed: $name"
            cmake --build "$builddir" --parallel "$jobs" || die "cmake build failed: $name"
            cmake --install "$builddir" || die "cmake install failed: $name"
            log "text-stack: $name OK"
            continue
        fi

        if [[ ! -f "$srcdir/meson.build" ]]; then
            rm -rf "$srcdir"
            mkdir -p "$out/textstack-dl" "$(dirname "$srcdir")"
            if [[ $kind == git ]]; then
                git clone --depth 1 --branch "$dirname" "$location" "$srcdir" 2>/dev/null \
                    || git clone "$location" "$srcdir"
                git -C "$srcdir" checkout --detach "$dirname" 2>/dev/null || true
                git -C "$srcdir" submodule update --init --depth 1 2>/dev/null || true
            else
                local tarball="$out/textstack-dl/$dirname.tar.gz"
                curl -fsSL --retry 5 --retry-all-errors --retry-delay 2 --connect-timeout 20 -o "$tarball" "$location" \
                    || die "download failed: $location"
                tar -C "$(dirname "$srcdir")" -xf "$tarball"
                if [[ ! -f "$srcdir/meson.build" ]]; then
                    local inner
                    inner=$(tar -tf "$tarball" | sed -n '1p' | cut -d/ -f1)
                    if [[ -n $inner && $inner != "$dirname" ]]; then
                        mv "$(dirname "$srcdir")/$inner" "$srcdir"
                    fi
                fi
            fi
        fi
        if [[ $name == glib ]]; then
            local netlink_patch="$repo/android/patches/glib-ohos-netlink.patch"
            if ! patch --batch --silent -R --dry-run -d "$srcdir" -p1 < "$netlink_patch" >/dev/null 2>&1; then
                patch --batch -d "$srcdir" -p1 < "$netlink_patch"
            fi
        fi
        if [[ ! -f "$builddir/build.ninja" ]]; then
            rm -rf "$builddir"
            # shellcheck disable=SC2086
            meson setup "$builddir" "$srcdir" --cross-file "$cross_file" \
                --prefix "$prefix" --buildtype release --default-library shared \
                $extra || die "meson setup failed: $name"
        fi
        ninja -C "$builddir" -j"$jobs" || die "ninja failed: $name"
        ninja -C "$builddir" install || die "install failed: $name"
        log "text-stack: $name OK"
    done

    log "text-stack libs:"
    ls "$prefix/lib" | grep -E "pango|glib|harfbuzz|fontconfig|cairo|pixman|fribidi|ffi|pcre"
}

# ── 8.4 引擎主模块 ───────────────────────────────────────────
do_engine() {
    ensure_sse2neon
    (
        cd "$src"
        ANDROID_NDK_ROOT="$ANDROID_NDK_ROOT" ANDROID_PLATFORM=30 ./devtools/bin/vpc_linux \
            /csgo /androidarm64 /define:DEVELOPMENT_ONLY \
            @launcher @engine @filesystem_stdio @inputsystem \
            @vphysics @materialsystem @shaderapidx9 @datacache @studiorender @soundemittersystem @vaudio_minimp3 @scenefilecache \
            @vscript @vguimatsurface @vgui_dll @localize @stdshader_dbg @stdshader_dx9 \
            @panorama @panoramauiclient @panorama_text_pango @matchmaking \
            @client_panorama @server \
            /nop4add /mksln csgo_android_engine /f
        make -k -f csgo_android_engine.mak -j"$jobs" --output-sync=target \
            CFG="$config" VALVE_NO_AUTO_P4=1 "${@:-all-targets}"
    )
}

# ── 8.5 native 层 libmain.so ─────────────────────────────────
do_native() {
    cmake -S "$repo/android" -B "$out/native-build" -G Ninja \
        "-DCMAKE_TOOLCHAIN_FILE=$OHOS_TOOLCHAIN_CMAKE" -DOHOS_ARCH=arm64-v8a \
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo" "-DCSGO_OHOS_DEPS=$prefix" \
        "-DCSGO_SOURCE_CONFIG=$config" -DCSGO_OHOS_PACKAGE=com.csgosource.ohos \
        -DCSGO_BUILD_ID=ohos-dev
    cmake --build "$out/native-build" --parallel "$jobs"
    log "libmain.so: $out/native-build/libmain.so"
}

# ── 8.6 产物回传 ─────────────────────────────────────────────
do_stage() {
    local hap=${HAP_LIBS_DIR:-$root/hap/entry/libs/arm64-v8a}
    local v8=${V8_RUNTIME_DIR:-$src/lib/common/androidarm64}
    for lib in libv8.cr.so libv8_libbase.cr.so libv8_libplatform.cr.so; do
        [[ -f "$v8/$lib" ]] || die "Missing real OHOS V8 runtime: $v8/$lib. Zero-return V8 stubs are not supported."
    done
    for input in "$out/native-build/libmain.so" "$prefix/lib/libSDL3.so" \
                 "$prefix/lib/libdxvk_d3d9.so" "$prefix/lib/libdxvk_dxgi.so.0"; do
        [[ -f "$input" ]] || die "Missing native output: $input"
    done
    mkdir -p "$hap"
    cp -p "$out/native-build/libmain.so" "$hap/"
    local found=0
    for so in "$repo/game/bin/androidarm64/$config/"*.so "$repo/game/csgo/bin/androidarm64/$config/"*.so; do
        [[ -f "$so" ]] || continue
        cp -p "$so" "$hap/"
        found=$((found + 1))
    done
    (( found >= 26 )) || die "Engine module set incomplete: found $found, expected at least 26"
    cp -L -p "$prefix/lib/libSDL3.so" "$hap/libSDL3.so"
    cp -L -p "$prefix/lib/libSDL3.so" "$hap/libSDL3.so.0"
    cp -L -p "$prefix/lib/libdxvk_d3d9.so" "$hap/d3d9.so"
    cp -L -p "$prefix/lib/libdxvk_dxgi.so.0" "$hap/"
    for so in "$prefix/lib/"*.so.* "$prefix/lib/"*.so; do
        [[ -f "$so" ]] || continue
        case "$(basename "$so")" in
            libSDL3.so|libSDL3.so.0|libdxvk_d3d9.so|libphonon.so) continue ;;
        esac
        cp -L -p "$so" "$hap/"
    done
    for lib in libv8.cr.so libv8_libbase.cr.so libv8_libplatform.cr.so; do
        cp -L -p "$v8/$lib" "$hap/$lib"
    done
    cp -p "$OHOS_SDK/llvm/lib/aarch64-linux-ohos/libc++_shared.so" "$hap/"
    python3 "$root/scripts/verify-ohos-libs.py" "$hap" --sdk "$OHOS_SDK"
    log "Staged verified ARM64 OHOS libraries: $hap"
}

if [[ ${1:-all} != vpc ]]; then
    [[ -x "$ANDROID_TOOLCHAIN/bin/clang" ]] || die "OHOS compiler missing: $ANDROID_TOOLCHAIN/bin/clang"
    [[ -d "$OHOS_SDK/sysroot" ]] || die "OHOS sysroot missing: $OHOS_SDK/sysroot"
fi

case "${1:-all}" in
    vpc) do_vpc ;;
    foundation) do_foundation ;;
    engine-deps) bash "$root/scripts/rebuild-ohos-dependencies.sh" small; bash "$root/scripts/rebuild-ohos-dependencies.sh" base ;;
    text-stack) bash "$root/scripts/rebuild-ohos-dependencies.sh" textstack ;;
    engine) shift; do_engine "$@" ;;
    native) do_native ;;
    stage) do_stage ;;
    all) do_vpc; do_foundation ;;
    *) die "Usage: build-ohos-engine.sh [vpc|foundation|engine-deps|text-stack|engine|native|stage|all]" ;;
esac
