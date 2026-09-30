#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -T -n dev -- env \
        BUILD_JOBS="${BUILD_JOBS:-6}" BUILD_CONFIG="${BUILD_CONFIG:-release}" ANDROID_SERIAL="${ANDROID_SERIAL:-}" \
        ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-/home/deck/Code/Toolchains/android-ndk}" \
        ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-/home/deck/Code/Toolchains/android-sdk}" \
        CSGO_RESOURCE_SOURCE="${CSGO_RESOURCE_SOURCE:-$repo_dir/runtime/csgo-2019}" \
        CSGO_ASTC_PACK="${CSGO_ASTC_PACK:-}" \
        CSGO_ANDROID_VK_LAYER="${CSGO_ANDROID_VK_LAYER:-}" \
        bash "$repo_dir/scripts/build-android.sh" "$@"
fi

config=${BUILD_CONFIG:-release}
[[ $config == debug || $config == release ]] || { echo 'BUILD_CONFIG must be debug or release.' >&2; exit 2; }
cmake_config=RelWithDebInfo
[[ $config != debug ]] || cmake_config=Debug
jobs=${BUILD_JOBS:-6}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'BUILD_JOBS must be positive.' >&2; exit 2; }
ndk=${ANDROID_NDK_ROOT:-/home/deck/Code/Toolchains/android-ndk}
sdk=${ANDROID_SDK_ROOT:-/home/deck/Code/Toolchains/android-sdk}
ndk_bin="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
build_tools="$sdk/build-tools/37.0.0"
android_jar="$sdk/platforms/android-37.2/android.jar"
out="$repo_dir/runtime/android"
prefix="$out/install"
variant="$out/$config"
mkdir -p "$variant"
apk="$variant/csgo-android-$config.apk"
build_tools_py="$repo_dir/scripts/android-build-tools.py"
package=com.csgosource.android
if [[ $config == debug ]]; then package=com.csgosource.android.debug; fi
resource_root=/storage/emulated/0/Games/CSGO
api=30
adb=("$sdk/platform-tools/adb")
if [[ -n ${ANDROID_SERIAL:-} ]]; then adb+=(-s "$ANDROID_SERIAL"); else unset ANDROID_SERIAL; fi

die() { echo "[csgo-android] $*" >&2; exit 1; }
need() { [[ -f $1 ]] || die "Missing file: $1"; }

checkout_dependency() {
    local directory=$1 url=$2 revision=$3
    local fresh=0
    if [[ ! -d $directory/.git ]]; then
        git clone --filter=blob:none --no-checkout "$url" "$directory"
        fresh=1
    fi
    if ((fresh)) || [[ $(git -C "$directory" rev-parse HEAD 2>/dev/null || true) != "$revision" ]]; then
        ((fresh)) || [[ -z $(git -C "$directory" status --porcelain) ]] || die "Preserving modified checkout: $directory"
        git -C "$directory" fetch origin "$revision"
        git -C "$directory" checkout --detach "$revision"
    fi
}

apply_dependency_patch() {
    local directory=$1 patch=$2
    if ! git -C "$directory" apply --reverse --check "$patch" 2>/dev/null; then
        git -C "$directory" apply --check "$patch" || die "Patch does not match; local changes were preserved: $patch"
        git -C "$directory" apply "$patch"
    fi
}

build_deps() {
    local sdl="$out/deps/sdl3" dxvk="$out/deps/dxvk"
    mkdir -p "$out/deps" "$prefix"
    checkout_dependency "$sdl" https://github.com/libsdl-org/SDL.git fa2c02bb6e21974a89ea9824bc53c9932abe5f9c
    checkout_dependency "$dxvk" https://github.com/Digger1955/dxvk-gplall.git 6a0ea561f9add008899680e6c313aa21c151e03e
    git -C "$dxvk" submodule update --init --recursive
    apply_dependency_patch "$dxvk" "$repo_dir/android/patches/dxvk-android.patch"
    cmake -S "$sdl" -B "$out/sdl3-build" -G Ninja \
        "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$api" -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_INSTALL_PREFIX=$prefix" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_INSTALL=ON
    cmake --build "$out/sdl3-build" --parallel "$jobs"
    cmake --install "$out/sdl3-build"
    python3 - "$repo_dir/android/dxvk.cross.in" "$out/dxvk.cross" "$ndk_bin" "$api" "$prefix" <<'PY'
import pathlib, sys
template, output, ndk, api, prefix = sys.argv[1:]
source = pathlib.Path(template).read_text()
for key, value in (('NDK_BIN', ndk), ('API', api), ('PREFIX', prefix)):
    if "'" in value or '\n' in value:
        raise SystemExit('Unsupported quote/newline in Meson toolchain path')
    source = source.replace('@' + key + '@', value)
pathlib.Path(output).write_text(source)
PY
    local setup=()
    if [[ -f $out/dxvk-build/meson-private/coredata.dat ]]; then setup+=(--reconfigure); fi
    meson setup "${setup[@]}" "$out/dxvk-build" "$dxvk" \
        --cross-file "$out/dxvk.cross" --prefix "$prefix" --buildtype debugoptimized \
        -Denable_d3d8=false -Denable_d3d9=true -Denable_d3d10=false \
        -Denable_d3d11=false -Denable_dxgi=false \
        -Dnative_sdl2=disabled -Dnative_sdl3=enabled -Dnative_glfw=disabled -Dbuild_id=true
    meson compile -C "$out/dxvk-build" -j "$jobs"
    meson install -C "$out/dxvk-build" --no-rebuild
}

build_native() {
    cmake -S "$repo_dir/android" -B "$variant/build" -G Ninja \
        "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$api" -DANDROID_STL=c++_shared \
        "-DCMAKE_BUILD_TYPE=$cmake_config" "-DCSGO_ANDROID_DEPS=$prefix" \
        "-DCSGO_SOURCE_CONFIG=$config" "-DCSGO_ANDROID_PACKAGE=$package" \
        "-DCSGO_BUILD_ID=$(python3 "$build_tools_py" fingerprint --scope package --config "$config")"
    cmake --build "$variant/build" --parallel "$jobs"
}

build_foundation() {
    checkout_dependency "$out/deps/sse2neon" https://github.com/DLTcollab/sse2neon.git 8d1d9f1cae82de66d9daea53f4f7ca30024f478b
    apply_dependency_patch "$out/deps/sse2neon" "$repo_dir/android/patches/sse2neon-android.patch"
    make -C "$repo_dir/src/utils/vpc" -j"$jobs" CC=clang CXX=clang++ OUTDIR=obj/Linux/clang-release
    (
        cd "$repo_dir/src"
        ANDROID_NDK_ROOT="$ndk" ANDROID_PLATFORM="$api" ./devtools/bin/vpc_linux \
            /csgo /androidarm64 +tier0 +tier1 +mathlib +interfaces +vstdlib \
            +tier2 +vpklib +filesystem_stdio +bitmap +vtf \
            /nop4add /mksln csgo_android_base /f
        make -f csgo_android_base.mak -j"$jobs" --output-sync=target \
            CFG="$config" VALVE_NO_AUTO_P4=1 all-targets
    )
}

build_engine_deps() {
    local libraries="$repo_dir/src/lib/public/androidarm64/$config"
    local protobuf="$repo_dir/src/thirdparty/protobuf-2.5.0"
    local cryptopp="$out/cryptopp-build"
    local mbedtls="$out/deps/mbedtls" curl="$out/deps/curl" jpeg="$out/deps/libjpeg-turbo"
    local cc="$ndk_bin/aarch64-linux-android${api}-clang"
    local cxx="$ndk_bin/aarch64-linux-android${api}-clang++"
    mkdir -p "$libraries" "$out/protobuf-build" "$cryptopp" "$prefix"
    (
        cd "$out/protobuf-build"
        if [[ ! -f Makefile ]]; then
            CC="$cc" CXX="$cxx" AR="$ndk_bin/llvm-ar" RANLIB="$ndk_bin/llvm-ranlib" \
                CFLAGS='-O2 -g -fPIC' \
                CXXFLAGS='-O2 -g -fPIC -std=c++11 -fsigned-char -fno-strict-aliasing -fno-fast-math -ffp-contract=off -moutline-atomics' \
                "$protobuf/configure" --host=aarch64-linux-android --build=x86_64-pc-linux-gnu \
                --disable-shared --enable-static --with-protoc="$repo_dir/src/devtools/bin/linux/protoc"
        fi
        make -C src -j"$jobs" libprotobuf.la
        cp -p src/.libs/libprotobuf.a "$libraries/libprotobuf.a"
    )
    # Keep NDK objects separate from the desktop Crypto++ build.
    rsync -a --include='/*.cpp' --include='/*.h' --exclude='*' \
        "$repo_dir/src/external/crypto++-5.61/" "$cryptopp/"
    python3 - "$repo_dir/src/external/crypto++-5.61/GNUmakefile" "$cryptopp/GNUmakefile" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
# The upstream archive target also publishes to Valve's p4 tree.
source = '\n'.join(line for line in source.splitlines()
                   if not line.startswith('\tp4 edit ') and not line.startswith('\t$(CP) libcryptopp.a ../../lib/')) + '\n'
destination = pathlib.Path(sys.argv[2])
if not destination.exists() or destination.read_text() != source:
    destination.write_text(source)
PY
    make -C "$cryptopp" -j"$jobs" libcryptopp.a \
        CXX="$cxx" AR="$ndk_bin/llvm-ar" ARFLAGS=-cr RANLIB="$ndk_bin/llvm-ranlib" IS_SUN_CC=0 \
        CXXFLAGS='-DNDEBUG -O2 -g -fPIC -std=c++11 -fsigned-char -fno-strict-aliasing -fno-fast-math -ffp-contract=off -moutline-atomics'
    cp -p "$cryptopp/libcryptopp.a" "$libraries/libcryptopp.a"

    checkout_dependency "$mbedtls" https://github.com/Mbed-TLS/mbedtls.git 068ff080b369adfac81509f9b57b2afabaf82dc5
    git -C "$mbedtls" submodule update --init --recursive
    checkout_dependency "$curl" https://github.com/curl/curl.git 01346829096c61b372692f6dc43ffa778c6caccd
    local cmake_args=(
        -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake"
        -DANDROID_ABI=arm64-v8a "-DANDROID_PLATFORM=android-$api" -DANDROID_STL=c++_shared
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        "-DCMAKE_INSTALL_PREFIX=$prefix" "-DCMAKE_PREFIX_PATH=$prefix" "-DCMAKE_FIND_ROOT_PATH=$prefix"
    )
    cmake -S "$mbedtls" -B "$out/mbedtls-build" "${cmake_args[@]}" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
    cmake --build "$out/mbedtls-build" --parallel "$jobs"
    cmake --install "$out/mbedtls-build"
    cmake -S "$curl" -B "$out/curl-build" "${cmake_args[@]}" \
        -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
        -DHTTP_ONLY=ON -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
        -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBIDN2=OFF \
        -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_CA_PATH=/system/etc/security/cacerts
    cmake --build "$out/curl-build" --parallel "$jobs"
    cmake --install "$out/curl-build"
    checkout_dependency "$jpeg" https://github.com/libjpeg-turbo/libjpeg-turbo.git af9c1c268520a29adf98cad5138dafe612b3d318
    # Source's public jpeglib headers use the JPEG 8 ABI, including struct sizes.
    cmake -S "$jpeg" -B "$out/jpeg-build" "${cmake_args[@]}" \
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_JPEG8=ON
    cmake --build "$out/jpeg-build" --parallel "$jobs" --target jpeg-static
    cp -p "$out/jpeg-build/libjpeg.a" "$prefix/lib/libjpeg.a"
    mkdir -p "$repo_dir/src/lib/common/androidarm64/$config"
    cp -p "$out/jpeg-build/libjpeg.a" "$repo_dir/src/lib/common/androidarm64/$config/jpeglib_client.a"
    local freetype="$out/deps/freetype"
    checkout_dependency "$freetype" https://gitlab.freedesktop.org/freetype/freetype.git 0a0221a1347e2f1e07c395263540026e9a0aa7c7
    # VGUI rasterizes TrueType/OpenType only; optional codecs stay out of the dependency set.
    cmake -S "$freetype" -B "$out/freetype-build" "${cmake_args[@]}" -DBUILD_SHARED_LIBS=ON \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    cmake --build "$out/freetype-build" --parallel "$jobs"
    cmake --install "$out/freetype-build"

    # libpng 1.5.2 (in-tree source) for Panorama's image loader; links the system libz.
    local png="$repo_dir/src/thirdparty/libpng-1.5.2" pngout="$repo_dir/src/lib/androidarm64/release"
    mkdir -p "$pngout" "$out/png-obj"
    for f in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil; do
        "$cc" -O2 -g -fPIC -fvisibility=hidden -I"$png" -I"$repo_dir/src/thirdparty/zlib-1.2.5" \
            -c "$png/$f.c" -o "$out/png-obj/$f.o"
    done
    "$ndk_bin/llvm-ar" crs "$pngout/libpng.a" "$out/png-obj/"*.o

    # Preserve the Parsifal callback ABI; Expat supplies the XML parser.
    local parsifal="$repo_dir/src/thirdparty/libparsifal-0.8.3" libcommon="$repo_dir/src/lib/common/androidarm64"
    mkdir -p "$libcommon" "$out/parsifal-obj"
    "$cc" -O2 -g -fPIC -fvisibility=hidden -I"$parsifal/include" -I"$prefix/include" \
        -c "$repo_dir/android/native/parsifal_expat.c" -o "$out/parsifal-obj/parsifal.o"
    "$ndk_bin/llvm-ar" crs "$libcommon/libparsifal.a" "$out/parsifal-obj/parsifal.o"

    local phonon_archive="$out/deps/steamaudio_api_2.0-beta.20.zip"
    if [[ ! -f $phonon_archive ]]; then
        curl -fL --retry 2 --connect-timeout 20 -o "$phonon_archive.download" \
            https://github.com/ValveSoftware/steam-audio/releases/download/v2.0-beta.20/steamaudio_api_2.0-beta.20.zip
        mv "$phonon_archive.download" "$phonon_archive"
    fi
    python3 - "$phonon_archive" "$prefix" <<'PY'
import hashlib, pathlib, sys, zipfile
archive, prefix = map(pathlib.Path, sys.argv[1:])
expected = '284b7d3b9a5ee744951c9138835c342a93bb2b08a43a253f54956ab6ff70fdd6'
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit('Steam Audio SDK archive checksum mismatch')
with zipfile.ZipFile(archive) as sdk:
    for member, target in (
        ('steamaudio_api/lib/Android/arm64/libphonon.so', 'lib/libphonon.so'),
        ('steamaudio_api/include/phonon.h', 'include/steam_audio/phonon.h'),
        ('steamaudio_api/include/phonon_version.h', 'include/steam_audio/phonon_version.h'),
    ):
        destination = prefix / target
        data = sdk.read(member)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.exists() or destination.read_bytes() != data:
            destination.write_bytes(data)
PY
}

build_engine() {
    build_engine_deps
    local engine_source
    engine_source=$(python3 "$build_tools_py" fingerprint)
    # These larger dependency recipes are tracked separately in the port plan.
    # Fail on missing inputs instead of silently packaging whatever is in game/bin.
    need "$repo_dir/src/lib/common/androidarm64/libv8.cr.so"
    need "$prefix/lib/libpango-1.0.so"
    need "$prefix/lib/libcairo.so"
    need "$repo_dir/src/lib/common/androidarm64/libcrypto_client.a"
    checkout_dependency "$out/deps/sse2neon" https://github.com/DLTcollab/sse2neon.git 8d1d9f1cae82de66d9daea53f4f7ca30024f478b
    apply_dependency_patch "$out/deps/sse2neon" "$repo_dir/android/patches/sse2neon-android.patch"
    make -C "$repo_dir/src/utils/vpc" -j"$jobs" CC=clang CXX=clang++ OUTDIR=obj/Linux/clang-release
    (
        cd "$repo_dir/src"
        # Each @module brings in the libraries it links; the list follows the launcher's
        # app-system load order, including the client/server shared app systems.
        ANDROID_NDK_ROOT="$ndk" ANDROID_PLATFORM="$api" ./devtools/bin/vpc_linux \
            /csgo /androidarm64 @launcher @engine @filesystem_stdio @inputsystem \
            @vphysics @materialsystem @shaderapidx9 @datacache @studiorender @soundemittersystem @vaudio_minimp3 @scenefilecache \
            @vscript @vguimatsurface @vgui_dll @localize @stdshader_dbg @stdshader_dx9 \
            @panorama @panoramauiclient @panorama_text_pango @matchmaking \
            @client_panorama @server \
            /nop4add /mksln csgo_android_engine /f
        make -k -f csgo_android_engine.mak -j"$jobs" --output-sync=target \
            CFG="$config" VALVE_NO_AUTO_P4=1 "${@:-all-targets}"
    )
    if [[ $# == 0 ]]; then
        python3 "$build_tools_py" receipt --config "$config" --source-id "$engine_source"
    fi
}

test_foundation() {
    [[ $# == 0 || ( $# == 1 && $1 == --compare-linux ) ]] || die 'Usage: test-foundation [--compare-linux]'
    build_foundation
    local libraries="$repo_dir/src/lib/public/androidarm64/$config"
    local flags=(
        -std=c++17 -O2 -g -fno-strict-aliasing -fsigned-char -fno-fast-math -ffp-contract=off
        -Wno-c++11-narrowing -Wno-register -Wno-return-type-c-linkage
        -DNDEBUG -DPOSIX -D_POSIX -DLINUX -D_LINUX -DGNUC -DCOMPILER_GCC
        -DRAD_TELEMETRY_DISABLED -DCSTRIKE15 -D_DLL_EXT=_client.so -D_FILE_OFFSET_BITS=64
        -I"$repo_dir/src/public" -I"$repo_dir/src/public/tier0" -I"$repo_dir/src/public/tier1"
        -I"$repo_dir/src/common"
    )
    "$ndk_bin/aarch64-linux-android${api}-clang++" "${flags[@]}" \
        -march=armv8-a -moutline-atomics -DANDROID -DUSE_DXVK_NATIVE \
        -I"$out/deps/sse2neon" -I"$prefix/include/dxvk" \
        "$repo_dir/android/native/platform_check.cpp" \
        -Wl,--start-group "$libraries/vtf_client.a" "$libraries/bitmap_client.a" "$libraries/tier2_client.a" \
        "$libraries/mathlib_client.a" "$libraries/tier1_client.a" \
        "$libraries/interfaces_client.a" -L"$libraries" -lvstdlib_client -ltier0_client -Wl,--end-group \
        -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 -Wl,--build-id=sha1 \
        -ldl -llog -o "$out/csgo-platform-check"
    "${adb[@]}" shell mkdir -p /data/local/tmp/csgo-platform-check
    "${adb[@]}" push "$out/csgo-platform-check" "$libraries/libtier0_client.so" "$libraries/libvstdlib_client.so" \
        "$repo_dir/game/bin/androidarm64/$config/libfilesystem_stdio_client.so" \
        "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" \
        /data/local/tmp/csgo-platform-check/
    "${adb[@]}" shell chmod 755 /data/local/tmp/csgo-platform-check/csgo-platform-check
    "${adb[@]}" shell 'cd /data/local/tmp/csgo-platform-check && LD_LIBRARY_PATH=. timeout 90s ./csgo-platform-check /storage/emulated/0/Games/CSGO /data/local/tmp' \
        | tee "$out/platform-check-android.log"

    if [[ ${1:-} == --compare-linux ]]; then
        bash "$repo_dir/scripts/build-linux.sh" tier0 tier1 mathlib interfaces vstdlib tier2 vpklib filesystem_stdio bitmap vtf
        libraries="$repo_dir/src/lib/public/linux64"
        clang++ "${flags[@]}" -march=nocona -D_GLIBCXX_USE_CXX11_ABI=0 -DDX_TO_GL_ABSTRACTION \
            "$repo_dir/android/native/platform_check.cpp" \
            -Wl,--start-group "$libraries/vtf_client.a" "$libraries/bitmap_client.a" "$libraries/tier2_client.a" \
            "$libraries/mathlib_client.a" "$libraries/tier1_client.a" \
            "$libraries/interfaces_client.a" -L"$libraries" -lvstdlib_client -ltier0_client -Wl,--end-group \
            -ldl -pthread -o "$out/csgo-platform-check-linux"
        LD_LIBRARY_PATH="$repo_dir/game/bin/linux64" timeout 90s "$out/csgo-platform-check-linux" \
            "${CSGO_RESOURCE_SOURCE:-$repo_dir/runtime/csgo-2019}" /tmp | tee "$out/platform-check-linux.log"
        local android_hash linux_hash
        android_hash=$(rg '^SIMD_EXACT_HASH:' "$out/platform-check-android.log")
        linux_hash=$(rg '^SIMD_EXACT_HASH:' "$out/platform-check-linux.log")
        [[ $android_hash == "$linux_hash" ]] || die "ARM64/Linux SIMD results differ: $android_hash / $linux_hash"
        echo "[csgo-android] ARM64/Linux exact SIMD comparison passed: $android_hash"
    fi
}

test_mobile() {
    local test_out="$variant/mobile-tests"
    mkdir -p "$test_out/classes" "$test_out/dex"
    local includes=(-I"$repo_dir/src/public" -I"$repo_dir/src/game/client/cstrike15")
    node --check "$repo_dir/android/app/src/main/assets/mobile_ui/menu.js"
    for model in touch hud; do
        clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
            "${includes[@]}" "$repo_dir/android/tests/mobile_${model}_test.cpp" -o "$test_out/$model-host"
        "$test_out/$model-host"
        "$ndk_bin/aarch64-linux-android${api}-clang++" -std=c++17 -O2 -static-libstdc++ \
            -Wl,-z,max-page-size=16384 "${includes[@]}" "$repo_dir/android/tests/mobile_${model}_test.cpp" -o "$test_out/$model-android"
        "${adb[@]}" push "$test_out/$model-android" "/data/local/tmp/csgo-mobile-$model-test"
        "${adb[@]}" shell chmod 755 "/data/local/tmp/csgo-mobile-$model-test"
        "${adb[@]}" shell "/data/local/tmp/csgo-mobile-$model-test"
    done
    python3 - "$repo_dir" <<'PY_MOBILE_ICONS'
import os, pathlib, re, struct, sys, xml.etree.ElementTree as ET
root = pathlib.Path(sys.argv[1])
assets = root / 'android/app/src/main/assets/mobile_ui'
source = (root / 'src/game/client/cstrike15/mobile/mobile_hud.h').read_text()
icons = set(re.findall(r'\{"[^"]+", "[^"]+", "([^"]+)"\}', source))
icons.update(re.findall(r'\b(?:b|prev|next|mode)\.icon = "([^"]+)"', source))
local_icons = {name for name in icons if '/' not in name}
stock_icons = icons - local_icons
for name in sorted(local_icons):
    icon = ET.parse(assets / 'icons' / (name + '.svg')).getroot()
    assert icon.attrib.get('viewBox') == '0 0 64 64', name
for name in ('base.xml', 'menu.xml'):
    ET.parse(assets / name)
assert ET.parse(assets / 'menu.xml').find("./snippets/snippet[@name='TouchButton']") is not None
for name in stock_icons:
    assert re.fullmatch(r'(equipment|ui)/[a-z0-9_]+', name), name
# Retail assets live under materials/, although Panorama addresses them via
# {images}. Check the actual directory index when the game's resources exist.
retail = pathlib.Path(os.environ.get('CSGO_RESOURCE_SOURCE', str(root / 'runtime/csgo-2019'))) / 'csgo/pak01_dir.vpk'
if retail.is_file():
    with retail.open('rb') as archive:
        magic, version, size = struct.unpack('<III', archive.read(12))
        assert magic == 0x55aa1234 and version in (1, 2)
        archive.seek(28 if version == 2 else 12)
        tree = archive.read(size)
    offset = 0
    paths = set()
    def cstring():
        global offset
        end = tree.index(0, offset)
        value = tree[offset:end].decode()
        offset = end + 1
        return value
    while (ext := cstring()):
        while (directory := cstring()):
            while (name := cstring()):
                _, preload, _, _, _, terminator = struct.unpack_from('<IHHIIH', tree, offset)
                assert terminator == 0xffff
                offset += 18 + preload
                paths.add(directory + '/' + name + '.' + ext)
    for name in stock_icons:
        assert 'materials/panorama/images/icons/' + name + '.svg' in paths, name
    for name in re.findall(r'file://\{images\}/([^"\s]+)', (assets / 'menu.xml').read_text()):
        assert 'materials/panorama/images/' + name in paths, name
    print(f'MOBILE_RETAIL_ASSETS_PASS: {len(stock_icons)} original icons and menu images exist in VPK')
print(f'MOBILE_ASSETS_PASS: {len(local_icons)} action SVG icons and Panorama XML')
PY_MOBILE_ICONS
    javac -encoding UTF-8 -source 8 -target 8 -Xlint:-options -classpath "$android_jar" \
        -d "$test_out/classes" "$repo_dir/android/tests/TouchInput.java"
    "$build_tools/d8" --min-api "$api" --lib "$android_jar" --output "$test_out/dex" \
        "$test_out/classes/com/csgosource/tests/TouchInput.class"
    "${adb[@]}" push "$test_out/dex/classes.dex" /data/local/tmp/csgo-touch.dex
}

package_apk() {
    local staging="$variant/package" classes="$variant/package/classes" dex="$variant/package/dex"
    # Only this generated staging tree is replaced; source and debug symbols stay intact.
    rm -rf -- "$staging"
    mkdir -p "$classes" "$dex" "$staging/lib/arm64-v8a" "$staging/assets/licenses"
    python3 "$build_tools_py" stage --config "$config" --ndk "$ndk"
    if [[ -d $repo_dir/android/app/src/main/assets ]]; then
        cp -R "$repo_dir/android/app/src/main/assets/." "$staging/assets/"
    fi
    python3 - "$staging/assets/mobile_ui" <<'PY_MOBILE_ASSETS'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
for name in ('base.xml', 'menu.xml', 'menu.css', 'menu.js'):
    if not (root / name).is_file():
        raise SystemExit(f'Missing mobile UI asset: {name}')
names = sorted(p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file() and p.name != 'manifest.txt')
(root / 'manifest.txt').write_text('\n'.join(names) + '\n')
PY_MOBILE_ASSETS
    python3 - "$staging/AndroidManifest.xml" "$package" <<'PY_MANIFEST'
import pathlib, sys
path, package = pathlib.Path(sys.argv[1]), sys.argv[2]
s = path.read_text()
s = s.replace('package="com.csgosource.android"', f'package="{package}"', 1)
path.write_text(s)
PY_MANIFEST
    # The application ID differs between variants; Java classes keep their namespace.
    local generated="$staging/generated/com/csgosource/android"
    mkdir -p "$generated"
    local java_debug=false
    [[ $config != debug ]] || java_debug=true
    cat > "$generated/BuildConfig.java" <<JAVA
package com.csgosource.android;
public final class BuildConfig {
    public static final boolean DEBUG = $java_debug;
    public static final String BUILD_TYPE = "$config";
    public static final String BUILD_ID = "$(python3 "$build_tools_py" fingerprint --scope package --config "$config")";
}
JAVA
    local sources=() bytecode=()
    mapfile -t sources < <(rg --files "$repo_dir/android/app/src" \
        "$out/deps/sdl3/android-project/app/src/main/java" "$staging/generated" -g '*.java')
    javac -encoding UTF-8 -source 8 -target 8 -Xlint:-options \
        -classpath "$android_jar" -d "$classes" "${sources[@]}"
    mapfile -t bytecode < <(rg --files "$classes" -g '*.class')
    "$build_tools/d8" "--$config" --lib "$android_jar" --min-api "$api" --output "$dex" "${bytecode[@]}"
    "$ndk_bin/llvm-strip" --strip-unneeded "$staging/lib/arm64-v8a/"*.so
    if [[ -n ${CSGO_ANDROID_VK_LAYER:-} ]]; then
        # Capture builds only: Android loads gpu_debug_layers from the debuggable
        # app's own extracted library directory, never from a separate layer APK.
        [[ $config == debug ]] || die 'CSGO_ANDROID_VK_LAYER requires BUILD_CONFIG=debug.'
        need "$CSGO_ANDROID_VK_LAYER"
        cp -- "$CSGO_ANDROID_VK_LAYER" "$staging/lib/arm64-v8a/"
    fi
    cp "$out/deps/sdl3/LICENSE.txt" "$staging/assets/licenses/SDL3.txt"
    cp "$out/deps/dxvk/LICENSE" "$staging/assets/licenses/DXVK.txt"
    cp "$out/deps/sse2neon/LICENSE" "$staging/assets/licenses/sse2neon.txt"
    cp "$repo_dir/src/thirdparty/vma/LICENSE.txt" "$staging/assets/licenses/VMA.txt"
    "$build_tools/aapt2" link -o "$staging/resources.apk" -I "$android_jar" \
        --manifest "$staging/AndroidManifest.xml" -A "$staging/assets" \
        --min-sdk-version "$api" --target-sdk-version 36 --version-code 1 --version-name "0.1-$config"
    python3 - "$staging" <<'PY'
import pathlib, shutil, sys, zipfile
root = pathlib.Path(sys.argv[1])
shutil.copyfile(root / 'resources.apk', root / 'unsigned.apk')
with zipfile.ZipFile(root / 'unsigned.apk', 'a', compression=zipfile.ZIP_STORED) as apk:
    for library in sorted((root / 'lib/arm64-v8a').glob('*.so')):
        apk.write(library, library.relative_to(root))
    for dex in sorted((root / 'dex').glob('*.dex')):
        apk.write(dex, dex.name)
PY
    if [[ ! -f $out/debug.keystore ]]; then
        keytool -genkeypair -keystore "$out/debug.keystore" -storepass android -keypass android \
            -alias androiddebugkey -dname 'CN=Android Debug,O=CSGO Source,C=CN' \
            -keyalg RSA -keysize 2048 -validity 10000 -noprompt
    fi
    "$build_tools/zipalign" -f -P 16 4 "$staging/unsigned.apk" "$staging/aligned.apk"
    "$build_tools/apksigner" sign --ks "$out/debug.keystore" --ks-pass pass:android \
        --key-pass pass:android --out "$apk" "$staging/aligned.apk"
}

verify_apk() {
    need "$apk"
    "$build_tools/apksigner" verify "$apk"
    "$build_tools/zipalign" -c -P 16 4 "$apk"
    python3 - "$apk" "$ndk_bin" "$config" "$build_tools_py" <<'PY'
import json, pathlib, re, runpy, subprocess, sys, tempfile, zipfile
apk, tools, configuration, helper = sys.argv[1:]
build = runpy.run_path(helper)
system = build['SYSTEM']
required_modules = {path.name for path in build['module_paths'](configuration)} | {
    'libSDL3.so', 'libdxvk_d3d9.so', 'libphonon.so', 'libcsgo_android.so', 'libc++_shared.so'}
with tempfile.TemporaryDirectory() as temp, zipfile.ZipFile(apk) as archive:
    libraries = [n for n in archive.namelist() if n.startswith('lib/') and n.endswith('.so')]
    present = {pathlib.PurePosixPath(n).name for n in libraries}
    metadata = json.loads(archive.read('assets/build-info.json'))
    assert metadata['configuration'] == configuration
    layers = {name for name in present if name.startswith('libVkLayer_')}
    assert not layers or configuration == 'debug', ('Vulkan layers belong only in Debug capture APKs', sorted(layers))
    assert present - layers == set(metadata['libraries']), ('bundle differs from manifest', present)
    assert required_modules <= present, ('missing required libraries', sorted(required_modules - present))
    # Every bundled library may only depend on another bundled library or a system one.
    allowed = present | system
    for entry in libraries:
        assert entry.startswith('lib/arm64-v8a/'), entry
        path = archive.extract(entry, temp)
        def read(*args):
            return subprocess.check_output([tools + '/llvm-readelf', *args, path], text=True)
        assert 'AArch64' in read('-h'), entry
        if pathlib.PurePosixPath(entry).name in layers:
            print(f'[csgo-android] {pathlib.PurePosixPath(entry).name}: ARM64 diagnostic Vulkan layer (not an engine module)')
            continue
        segments = re.findall(r'^\s*LOAD\s+.*\s(0x[0-9a-f]+)\s*$', read('-lW'), re.M)
        assert segments and all(int(s, 16) >= 16384 for s in segments), (entry, segments)
        dependencies = re.findall(r'\(NEEDED\).*\[(.*?)\]', read('-d'))
        assert set(dependencies) <= allowed, (entry, dependencies)
        assert not re.search(r'GLIBC_|GLIBCXX_', read('-V')), entry
        name = pathlib.PurePosixPath(entry).name
        build_id = re.search(r'Build ID: (\w+)', read('-n'))
        # Our own engine modules always carry a build-id; some third-party/prebuilt
        # libraries (Steam Audio, V8, cairo, pixman) do not, which is not load-critical.
        expected_id = metadata['libraries'][name]['build_id']
        assert (build_id[1] if build_id else None) == expected_id, (entry, expected_id)
        exported = {'libcsgo_android.so': 'SDL_main', 'libdxvk_d3d9.so': 'Direct3DCreate9',
                    'liblauncher_client.so': 'LauncherMain',
                    'libfilesystem_stdio_client.so': 'CreateInterface', 'libvstdlib_client.so': 'CreateInterface'}
        if name in exported:
            assert re.search(r'\bGLOBAL\s+DEFAULT\s+\d+\s+' + exported[name] + r'\b', read('--dyn-syms')), entry
        print(f'[csgo-android] {name}: ARM64 / 16 KiB / Build ID {build_id[1] if build_id else "prebuilt"} / {", ".join(dependencies)}')
print('[csgo-android] APK signature, alignment and native dependency checks passed')
PY
}

sync_resources() {
    local source=${CSGO_RESOURCE_SOURCE:-$repo_dir/runtime/csgo-2019}
    local staging="$out/resources"
    need "$source/csgo/gameinfo.txt"
    need "$source/csgo/pak01_dir.vpk"
    [[ -d $source/platform ]] || die "Missing platform resources: $source/platform"
    mkdir -p "$staging"
    for tree in csgo platform; do
        mkdir -p "$staging/$tree"
        # Hardlink unchanged resources locally. Never delete anything on the phone.
        rsync -a --delete --link-dest="$source/$tree" \
            --exclude='/bin/' --exclude='/local/' --exclude='/logs/' \
            --exclude='*.so' --exclude='*.so.*' --exclude='*.dll' --exclude='*.exe' \
            --exclude='*.log' --exclude='console*.txt' --exclude='*.dmp' \
            "$source/$tree/" "$staging/$tree/"
    done
    "${adb[@]}" shell mkdir -p "$resource_root"
    "${adb[@]}" push --sync "$staging/csgo" "$staging/platform" "$resource_root/"
    # Optional offline ASTC pack (docs/astc-resource-pack.md); the game uses
    # $resource_root/astc whenever it exists.
    if [[ -n ${CSGO_ASTC_PACK:-} ]]; then
        local pack; pack=$(realpath "$CSGO_ASTC_PACK")
        need "$pack/manifest.json"
        mkdir -p "$staging/astc"
        rsync -a --delete --link-dest="$pack" --include='*/' --include='*.ktx2' --exclude='*' \
            "$pack/" "$staging/astc/"
        "${adb[@]}" push --sync "$staging/astc" "$resource_root/"
    fi
}

build_v8() {
    local source="$out/deps/v8root/v8" libdir="$repo_dir/src/lib/common/androidarm64"
    need "$source/out/android_arm64/build.ninja"
    [[ $(git -C "$source" rev-parse HEAD) == eda659cc5e307f20ac1ad542ba12ab32eaf4c7ef ]] || die 'Unexpected V8 revision'
    [[ $(git -C "$source/build" rev-parse HEAD) == c7c2db69cd571523ce728c4d3dceedbd1896b519 ]] || die 'Unexpected V8 build-tools revision'
    apply_dependency_patch "$source/build" "$repo_dir/android/patches/v8-build-android.patch"
    command -v python2 >/dev/null || die 'Legacy V8 needs Python 2 in dev; see docs/android-port-plan.md'
    local tools_dir="$out/legacy-host/bin"
    mkdir -p "$tools_dir"
    ln -sfn "$(command -v python2)" "$tools_dir/python"
    PATH="$tools_dir:$PATH" ninja -C "$source/out/android_arm64" -j"$jobs" v8 v8_libbase v8_libplatform
    mkdir -p "$libdir"
    local library
    for library in libv8 libv8_libbase libv8_libplatform libicuuc libicui18n; do
        cp -p "$source/out/android_arm64/$library.cr.so" "$libdir/$library.cr.so"
    done
}

test_physics() {
    mkdir -p "$variant/logs"
    local libraries="$repo_dir/src/lib/public/androidarm64/$config"
    local physics="$repo_dir/game/bin/androidarm64/$config/libvphysics_client.so"
    need "$physics"
    "$ndk_bin/aarch64-linux-android${api}-clang++" -std=c++17 -O1 -g \
        -DPOSIX -D_POSIX -DLINUX -D_LINUX -DANDROID -DGNUC -DCOMPILER_GCC \
        -DRAD_TELEMETRY_DISABLED -DNDEBUG -Wno-c++11-narrowing -Wno-register \
        -I"$repo_dir/src/public" -I"$repo_dir/src/public/tier0" -I"$repo_dir/src/public/tier1" \
        -I"$out/deps/sse2neon" "$repo_dir/android/tests/vphysics_test.cpp" \
        -L"$libraries" -ltier0_client -ldl -Wl,-z,max-page-size=16384 \
        -o "$variant/vphysics-test"
    "${adb[@]}" shell mkdir -p /data/local/tmp/csgo-physics-check
    "${adb[@]}" push "$variant/vphysics-test" "$physics" \
        "$libraries/libtier0_client.so" "$libraries/libvstdlib_client.so" \
        "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" \
        /data/local/tmp/csgo-physics-check/
    "${adb[@]}" shell 'cd /data/local/tmp/csgo-physics-check && LD_LIBRARY_PATH=. timeout 30s ./vphysics-test' \
        | tee "$variant/logs/vphysics-test-device.log"
}

test_vscript() {
    local libraries="$repo_dir/src/lib/public/androidarm64/$config"
    local flags=(-std=c++17 -O2 -g -fno-strict-aliasing -DNDEBUG
        -DPOSIX -D_POSIX -DLINUX -D_LINUX -DGNUC -DCOMPILER_GCC -DRAD_TELEMETRY_DISABLED
        -Wno-c++11-narrowing -Wno-register -Wno-return-type-c-linkage
        -I"$repo_dir/src/public" -I"$repo_dir/src/public/tier0" -I"$repo_dir/src/public/tier1")
    need "$libraries/libtier0_client.so"
    clang++ "${flags[@]}" -D_GLIBCXX_USE_CXX11_ABI=0 "$repo_dir/android/tests/vscript_test.cpp" \
        -L"$repo_dir/src/lib/public/linux64" -ltier0_client \
        -Wl,-rpath,"$repo_dir/src/lib/public/linux64" -o "$variant/vscript-test-linux"
    "$variant/vscript-test-linux"
    "$ndk_bin/aarch64-linux-android${api}-clang++" "${flags[@]}" -DANDROID \
        -I"$out/deps/sse2neon" "$repo_dir/android/tests/vscript_test.cpp" \
        -L"$libraries" -ltier0_client -Wl,-z,max-page-size=16384 -o "$variant/vscript-test"
    "${adb[@]}" shell mkdir -p /data/local/tmp/csgo-vscript-check
    "${adb[@]}" push "$variant/vscript-test" "$libraries/libtier0_client.so" \
        "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" \
        /data/local/tmp/csgo-vscript-check/
    "${adb[@]}" shell 'cd /data/local/tmp/csgo-vscript-check && LD_LIBRARY_PATH=. timeout 30s ./vscript-test'
}

test_gcsdk() {
    local libraries="$repo_dir/src/lib/public/androidarm64/$config"
    local desktop="$repo_dir/src/lib/public/linux64"
    local flags=(-std=c++17 -O1 -g -fno-omit-frame-pointer -fno-strict-aliasing -DNDEBUG
        -DPOSIX -D_POSIX -DLINUX -D_LINUX -DGNUC -DCOMPILER_GCC -DNO_STEAM -DRAD_TELEMETRY_DISABLED
        -Wno-c++11-narrowing -Wno-register -Wno-return-type-c-linkage
        -I"$repo_dir/src/public" -I"$repo_dir/src/public/tier0" -I"$repo_dir/src/public/tier1"
        -I"$repo_dir/src/common" -I"$repo_dir/src/public/gcsdk" -I"$repo_dir/src/gcsdk/steamextra"
        -I"$repo_dir/src/gcsdk/generated_proto" -I"$repo_dir/src/thirdparty/protobuf-2.5.0/src")
    need "$libraries/gcsdk_client.a"
    # Compile the new implementation itself on Linux, not the desktop prebuilt SDK.
    clang++ "${flags[@]}" -D_GLIBCXX_USE_CXX11_ABI=0 -mcx16 -fsanitize=address \
        "$repo_dir/android/tests/gcsdk_test.cpp" "$repo_dir/src/gcsdk/msgprotobuf.cpp" \
        "$repo_dir/src/gcsdk/netpacket.cpp" "$repo_dir/src/gcsdk/webapi_response.cpp" \
        "$repo_dir/src/gcsdk/generated_proto/steammessages.pb.cc" \
        "$desktop/tier1_client.a" "$desktop/interfaces_client.a" "$desktop/libprotobuf.a" \
        -L"$desktop" -ltier0_client -lvstdlib_client \
        -Wl,-rpath,"$desktop" -lpthread -ldl -o "$variant/gcsdk-test-linux"
    "$variant/gcsdk-test-linux"
    "$ndk_bin/aarch64-linux-android${api}-clang++" "${flags[@]}" -DANDROID \
        -I"$out/deps/sse2neon" "$repo_dir/android/tests/gcsdk_test.cpp" \
        -Wl,--start-group "$libraries/gcsdk_client.a" "$libraries/tier1_client.a" \
        "$libraries/interfaces_client.a" "$libraries/libprotobuf.a" -Wl,--end-group \
        -L"$libraries" -ltier0_client -lvstdlib_client \
        -Wl,-z,max-page-size=16384 -o "$variant/gcsdk-test"
    "${adb[@]}" shell mkdir -p /data/local/tmp/csgo-gcsdk-check
    "${adb[@]}" push "$variant/gcsdk-test" "$libraries/libtier0_client.so" "$libraries/libvstdlib_client.so" \
        "$ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" \
        /data/local/tmp/csgo-gcsdk-check/
    "${adb[@]}" shell 'cd /data/local/tmp/csgo-gcsdk-check && LD_LIBRARY_PATH=. timeout 30s ./gcsdk-test'
}

collect_diagnostics() {
    local report
    report="$variant/diagnostics/$(date -u +%Y%m%dT%H%M%SZ)"
    mkdir -p "$report"
    "${adb[@]}" pull "$resource_root/logs" "$report/device-logs" > "$report/pull.log" 2>&1 || true
    "${adb[@]}" pull "$resource_root/csgo/console.log" "$report/console.log" >> "$report/pull.log" 2>&1 || true
    "${adb[@]}" shell dumpsys activity exit-info "$package" > "$report/exit-info.txt"
    local pid
    pid=$(python3 - "$report" <<'PY_DIAG'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
for log in root.rglob('launcher.log'):
    matches = re.findall(r'pid=(\d+)', log.read_text(errors='replace'))
    if matches:
        print(matches[-1])
        break
PY_DIAG
    )
    if [[ $pid =~ ^[0-9]+$ ]]; then
        "${adb[@]}" logcat -d -v threadtime --pid="$pid" > "$report/logcat.txt"
    fi
    # Native tombstones are emitted by crash_dump64, whose PID differs from the
    # game process. A --pid-only log loses the backtrace that we need to debug.
    python3 - "$report" "$package" "$pid" "$variant/symbols/build-info.json" "${adb[@]}" <<'PY_CRASH'
import json, pathlib, re, shutil, subprocess, sys
report, package, pid, symbols, *adb = sys.argv[1:]
report = pathlib.Path(report)
text = subprocess.check_output(adb + ['logcat', '-d', '-b', 'crash', '-v', 'threadtime', 'DEBUG:F', '*:S'], text=True)
blocks = re.split(r'(?=^.*\*\*\* \*\*\* \*\*\*)', text, flags=re.M)
matching = [block for block in blocks
            if (f'Cmdline: {package}:game' in block or f'>>> {package}:game <<<' in block)
            and (not pid or re.search(r'\bpid:\s*' + re.escape(pid) + r',', block))]
(report / 'crash-logcat.txt').write_text(''.join(matching))
if pathlib.Path(symbols).is_file():
    metadata = json.loads(pathlib.Path(symbols).read_text())
    installed = None
    for log in report.rglob('launcher.log'):
        ids = re.findall(r'build=([0-9a-f]+)', log.read_text(errors='replace'))
        if ids:
            installed = ids[-1]
            break
    local = metadata.get('build_id', metadata.get('source_id'))
    if installed == local:
        shutil.copyfile(symbols, report / 'build-info.json')
    else:
        warning = f'Device log build {installed} differs from local symbols {local}; do not symbolize with this build.\n'
        (report / 'symbol-mismatch.txt').write_text(warning)
        print('[csgo-android] ' + warning, end='')
PY_CRASH
    echo "[csgo-android] Diagnostics: $report"
}

action=${1:-build}
if (($#)); then shift; fi
case "$action" in
    build) build_deps; build_engine; build_native; package_apk; verify_apk; echo "[csgo-android] $apk" ;;
    deps) build_deps ;;
    native) build_native; package_apk; verify_apk ;;
    package) package_apk; verify_apk ;;
    foundation) build_foundation ;;
    test-foundation) test_foundation "$@" ;;
    test-physics) test_physics ;;
    test-vscript) test_vscript ;;
    test-gcsdk) test_gcsdk ;;
    test-mobile) test_mobile ;;
    verify) verify_apk ;;
    install) verify_apk; "${adb[@]}" install --no-incremental -r "$apk" ;;
    run|console) exec python3 "$build_tools_py" "$action" --config "$config" --adb "${adb[0]}" --args "$@" ;;
    probe) [[ $# -le 1 ]] || die 'Usage: probe [seconds]';
        exec python3 "$build_tools_py" run --config "$config" --adb "${adb[0]}" --probe "${1:-15}" ;;
    vulkan-probe) [[ $# -le 2 ]] || die 'Usage: vulkan-probe [seconds] [--validation]';
        validation_args=()
        if [[ $# == 2 ]]; then
            [[ $2 == --validation ]] || die 'Usage: vulkan-probe [seconds] [--validation]'
            validation_args=(--validation)
        fi
        exec python3 "$build_tools_py" run --config "$config" --adb "${adb[0]}" --vulkan-probe "${1:-15}" "${validation_args[@]}" ;;
    v8) build_v8 ;;
    sync) sync_resources ;;
    diagnose) collect_diagnostics ;;
    symbolize) [[ $# -ge 2 ]] || die 'Usage: symbolize LIBRARY ADDRESS...';
        library=$1; shift
        symbol="$variant/symbols/$library.dbg"
        [[ -f $symbol ]] || symbol="$variant/symbols/$library"
        need "$symbol"
        exec "$ndk_bin/llvm-addr2line" -Cfipe "$symbol" "$@" ;;
    logs) exec "${adb[@]}" logcat -v threadtime CSGO:I SDL:V AndroidRuntime:E '*:S' ;;
    engine-deps) build_engine_deps ;;
    engine) build_engine "$@" ;;
    *) die 'Usage: build-android.sh [build|deps|native|package|foundation|engine-deps|engine|v8|test-foundation|test-physics|test-vscript|test-gcsdk|test-mobile|verify|install|run|console|probe|vulkan-probe|sync|logs|diagnose|symbolize]';;
esac
