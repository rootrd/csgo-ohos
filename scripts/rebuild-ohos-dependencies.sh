#!/usr/bin/env bash
set -euo pipefail
# Builds source dependencies only; no original src/lib bundle is needed.
# Requires native OHOS SDK, host C/C++ compiler, CMake, Ninja, Meson, pkg-config,
# gperf, make, curl, patch, Perl and Python 3.12. Optional OHOS_HOST_ENV can set PATH.
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
if [[ -n ${OHOS_HOST_ENV:-} ]]; then source "$OHOS_HOST_ENV"; fi
REPO=${CSGO_SOURCE_ROOT:-$REPO_ROOT/CSGO-Source-Linux-20260928}
BASE=${CSGO_DEPS_WORK:-$REPO/runtime/ohos/dependency-build}
: "${OHOS_SDK:?Export the official OHOS native SDK path}"
export CONFIG_SHELL=/bin/bash
PREFIX=${DEPS_PREFIX:-$REPO/runtime/ohos/install}
JOBS=${BUILD_JOBS:-3}
CONFIG=${BUILD_CONFIG:-release}
[[ $CONFIG == release || $CONFIG == debug ]] || { echo 'BUILD_CONFIG must be release or debug' >&2; exit 2; }
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || { echo 'BUILD_JOBS must be positive' >&2; exit 2; }
CC="$OHOS_SDK/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$OHOS_SDK/sysroot"
CXX="$OHOS_SDK/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$OHOS_SDK/sysroot"
AR=$OHOS_SDK/llvm/bin/llvm-ar
RANLIB=$OHOS_SDK/llvm/bin/llvm-ranlib
CFLAGS='-O2 -fPIC -fsigned-char -fno-strict-aliasing -fno-fast-math -ffp-contract=off'
CXXFLAGS="$CFLAGS -stdlib=libc++"
LDFLAGS='-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384'
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_LIBDIR"
mkdir -p "$PREFIX/lib" "$PREFIX/include" "$BASE/build" "$BASE/logs" "$REPO/src/lib/public/androidarm64/$CONFIG" "$REPO/src/lib/common/androidarm64/$CONFIG" "$REPO/src/lib/androidarm64/release"
python3 "$REPO_ROOT/scripts/fetch-ohos-dependency-sources.py" "$BASE"
# Correct the optional Linux netlink backend probe for OHOS's socket headers.
if ! patch -d "$BASE/src/glib" -p1 -R --dry-run < "$REPO/android/patches/glib-ohos-netlink.patch" >/dev/null 2>&1; then
 patch -d "$BASE/src/glib" -p1 < "$REPO/android/patches/glib-ohos-netlink.patch"
fi
HOST_PROTOC=${PROTOC:-$BASE/host/protobuf/src/protoc}
ensure_host_protoc() {
 if [[ ! -x "$HOST_PROTOC" ]]; then
  mkdir -p "$BASE/host/protobuf"
  (cd "$BASE/host/protobuf"; CXXFLAGS='-O2 -std=c++11' /bin/bash "$REPO/src/thirdparty/protobuf-2.5.0/configure" --disable-shared --enable-static; make -C src -j"$JOBS" SHELL=/bin/bash protoc)
 fi
 "$HOST_PROTOC" --version
}
ensure_phonon_headers() {
 if [[ -f "$PREFIX/include/steam_audio/phonon.h" && -f "$PREFIX/include/steam_audio/phonon_version.h" ]]; then return; fi
 local archive="$BASE/downloads/steamaudio_api_2.0-beta.20.zip"
 if [[ ! -f "$archive" ]]; then
  curl -fLsS --retry 3 --connect-timeout 20 -o "$archive.download" https://github.com/ValveSoftware/steam-audio/releases/download/v2.0-beta.20/steamaudio_api_2.0-beta.20.zip
  mv "$archive.download" "$archive"
 fi
 python3 - "$archive" "$PREFIX" <<'PYHEADERS'
from pathlib import Path
import hashlib, sys, zipfile
archive, prefix = map(Path, sys.argv[1:])
if hashlib.sha256(archive.read_bytes()).hexdigest() != '284b7d3b9a5ee744951c9138835c342a93bb2b08a43a253f54956ab6ff70fdd6':
    raise SystemExit('Steam Audio header archive checksum mismatch')
headers = prefix / 'include/steam_audio'
headers.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(archive) as sdk:
    for name in ('phonon.h', 'phonon_version.h'):
        (headers / name).write_bytes(sdk.read('steamaudio_api/include/' + name))
PYHEADERS
}
cmake_args=(-G Ninja "-DCMAKE_TOOLCHAIN_FILE=$OHOS_SDK/build/cmake/ohos.toolchain.cmake" -DOHOS_ARCH=arm64-v8a -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON "-DCMAKE_INSTALL_PREFIX=$PREFIX" "-DCMAKE_PREFIX_PATH=$PREFIX" "-DCMAKE_SHARED_LINKER_FLAGS=$LDFLAGS" "-DCMAKE_EXE_LINKER_FLAGS=$LDFLAGS")
cmake_dep() { local name=$1 src=$2; shift 2; cmake -S "$src" -B "$BASE/build/$name" "${cmake_args[@]}" "$@"; cmake --build "$BASE/build/$name" --parallel "$JOBS"; cmake --install "$BASE/build/$name"; }
small() {
 ensure_host_protoc
 ensure_phonon_headers
 local libpub=$REPO/src/lib/public/androidarm64/$CONFIG libcom=$REPO/src/lib/common/androidarm64
 mkdir -p "$BASE/build/protobuf" "$BASE/build/png" "$BASE/build/bzip2" "$BASE/build/parsifal" "$BASE/build/cryptopp"
 (
 cd "$BASE/build/protobuf"
 if [[ ! -f Makefile ]]; then CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS -std=c++11" /bin/bash "$REPO/src/thirdparty/protobuf-2.5.0/configure" --host=aarch64-linux-gnu --build=x86_64-pc-linux-gnu --disable-shared --enable-static --with-protoc="$HOST_PROTOC"; fi
 make -C src -j"$JOBS" SHELL=/bin/bash libprotobuf.la
 cp src/.libs/libprotobuf.a "$libpub/libprotobuf.a"
 )
 # Build the repository's complete Crypto++ 5.61 sources without legacy p4 steps.
 cp -p "$REPO/src/external/crypto++-5.61/"*.{cpp,h} "$BASE/build/cryptopp/"
 sed '/^\tp4 edit /d; /^\t$(CP) libcryptopp.a ..\/..\/lib\//d' "$REPO/src/external/crypto++-5.61/GNUmakefile" > "$BASE/build/cryptopp/GNUmakefile"
 make -C "$BASE/build/cryptopp" -j"$JOBS" libcryptopp.a CXX="$CXX" AR="$AR" ARFLAGS=-cr RANLIB="$RANLIB" IS_SUN_CC=0 CXXFLAGS="-DNDEBUG -DCRYPTOPP_DISABLE_ASM $CXXFLAGS -std=c++11"
 cp "$BASE/build/cryptopp/libcryptopp.a" "$libpub/libcryptopp.a"
 for f in png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil; do
  $CC $CFLAGS -fvisibility=hidden -I"$REPO/src/thirdparty/libpng-1.5.2" -I"$REPO/src/thirdparty/zlib-1.2.5" -c "$REPO/src/thirdparty/libpng-1.5.2/$f.c" -o "$BASE/build/png/$f.o"
 done
 "$AR" crs "$REPO/src/lib/androidarm64/release/libpng.a" "$BASE/build/png/"*.o
 for f in blocksort huffman crctable randtable compress decompress bzlib; do $CC $CFLAGS -D_FILE_OFFSET_BITS=64 -c "$REPO/src/utils/bzip2/$f.c" -o "$BASE/build/bzip2/$f.o"; done
 "$AR" crs "$libcom/bzip2_client.a" "$BASE/build/bzip2/"*.o
 cp "$libcom/bzip2_client.a" "$libcom/$CONFIG/bzip2_client.a"
 "$AR" crs "$PREFIX/lib/libandroid.a"
 # OHOS compile-time fallback contains no ipl* calls. Satisfy legacy -lphonon
 # with an empty archive; no fake .so or foreign/Bionic runtime is shipped.
 "$AR" crs "$PREFIX/lib/libphonon.a"
}
openssl_dep() {
 mkdir -p "$BASE/build/openssl"
 (cd "$BASE/build/openssl"
  if [[ ! -f Makefile ]]; then
   CC="$CC" AR="$AR" RANLIB="$RANLIB" CFLAGS="$CFLAGS" LDFLAGS="$LDFLAGS" \
    perl "$BASE/src/openssl/Configure" linux-aarch64 no-shared no-tests no-module no-asm --prefix="$PREFIX" --libdir=lib
  fi
  make -j"$JOBS" build_libs
  make install_dev
 )
 # Panorama font-package decryption uses real AES_set_decrypt_key/AES_decrypt.
 cp -p "$BASE/build/openssl/libcrypto.a" "$REPO/src/lib/common/androidarm64/libcrypto_client.a"
}
base() {
 openssl_dep
 cmake_dep mbedtls "$BASE/src/mbedtls" -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
 cmake_dep expat "$BASE/src/libexpat/expat" -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_SHARED_LIBS=ON -DEXPAT_STATIC_LIBS=OFF
 cmake_dep curl "$BASE/src/curl" -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DHTTP_ONLY=ON -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBIDN2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF
 cmake_dep jpeg "$BASE/src/libjpeg-turbo" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF -DWITH_JPEG8=ON
 cp "$BASE/build/jpeg/libjpeg.a" "$REPO/src/lib/common/androidarm64/$CONFIG/jpeglib_client.a"
 cmake_dep freetype "$BASE/src/freetype" -DBUILD_SHARED_LIBS=ON -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
 mkdir -p "$BASE/build/parsifal"
 $CC $CFLAGS -fvisibility=hidden -I"$REPO/src/thirdparty/libparsifal-0.8.3/include" -I"$PREFIX/include" -c "$REPO/android/native/parsifal_expat.c" -o "$BASE/build/parsifal/parsifal.o"
 "$AR" crs "$REPO/src/lib/common/androidarm64/libparsifal.a" "$BASE/build/parsifal/parsifal.o"
}
write_cross() {
 cat > "$BASE/ohos.cross" <<EOF
[binaries]
c = ['$OHOS_SDK/llvm/bin/clang', '--target=aarch64-linux-ohos', '--sysroot=$OHOS_SDK/sysroot']
cpp = ['$OHOS_SDK/llvm/bin/clang++', '--target=aarch64-linux-ohos', '--sysroot=$OHOS_SDK/sysroot']
ar = '$AR'
strip = '$OHOS_SDK/llvm/bin/llvm-strip'
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-fPIC', '-fsigned-char', '-fno-strict-aliasing', '-fno-fast-math', '-ffp-contract=off']
cpp_args = ['-fPIC', '-fsigned-char', '-fno-strict-aliasing', '-fno-fast-math', '-ffp-contract=off', '-stdlib=libc++']
c_link_args = ['-fuse-ld=lld', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384', '-Wl,--build-id=sha1']
cpp_link_args = ['-fuse-ld=lld', '-stdlib=libc++', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384', '-Wl,--build-id=sha1']
[properties]
pkg_config_libdir = '$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig'
[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
}
meson_dep() { local name=$1;shift; if [[ ! -f "$BASE/build/$name/build.ninja" ]]; then meson setup "$BASE/build/$name" "$BASE/src/$name" --cross-file "$BASE/ohos.cross" --prefix "$PREFIX" --libdir lib --buildtype release --default-library shared "$@";fi; ninja -C "$BASE/build/$name" -j"$JOBS"; ninja -C "$BASE/build/$name" install; }
textstack() {
 write_cross
 meson_dep fribidi -Ddocs=false -Dtests=false
 mkdir -p "$BASE/build/libffi"
 (cd "$BASE/build/libffi"; if [[ ! -f Makefile ]];then CC="$CC" CXX="$CXX" AR="$AR" RANLIB="$RANLIB" CFLAGS="$CFLAGS" CXXFLAGS="$CXXFLAGS" LDFLAGS="$LDFLAGS" /bin/bash "$BASE/src/libffi/configure" --host=aarch64-linux-gnu --build=x86_64-pc-linux-gnu --prefix="$PREFIX" --enable-shared --disable-static --libdir="$PREFIX/lib";fi;make -j"$JOBS" SHELL=/bin/bash;make SHELL=/bin/bash install)
 cmake_dep pcre2 "$BASE/src/pcre2" -DPCRE2_BUILD_TESTS=OFF -DPCRE2_BUILD_PCRE2GREP=OFF -DPCRE2_BUILD_PCRE2_16=OFF -DPCRE2_BUILD_PCRE2_32=OFF -DPCRE2_SUPPORT_JIT=ON
 meson_dep glib -Dintrospection=disabled -Dlibelf=disabled -Dnls=disabled -Dman=false -Dtests=false -Dinstalled_tests=false -Dlibmount=disabled -Dselinux=disabled -Dsysprof=disabled -Dglib_debug=disabled -Dglib_assert=false -Dglib_checks=true
 meson_dep harfbuzz -Dtests=disabled -Ddocs=disabled -Dbenchmark=disabled -Dicu=disabled -Dglib=disabled -Dgobject=disabled -Dfreetype=enabled -Dcairo=disabled
 meson_dep fontconfig -Dtests=disabled -Ddoc=disabled -Ddoc-man=disabled -Dnls=disabled -Dtools=disabled -Dcache-build=disabled
 meson_dep pixman -Dtests=disabled -Ddemos=disabled
 python3 - "$BASE/src/cairo" <<'PY'
from pathlib import Path
import sys
r=Path(sys.argv[1]); p=r/'src/cairo-ft-private.h';s=p.read_text()
if 'FT_COLOR_H' not in s:s=s.replace('#define CAIRO_FT_PRIVATE_H','#define CAIRO_FT_PRIVATE_H\n#include <ft2build.h>\n#include FT_COLOR_H');p.write_text(s)
p=r/'src/cairo-ft-font.c';s=p.read_text()
if 'FT_COLOR_H' not in s:s=s.replace('#include FT_LCD_FILTER_H','#include FT_LCD_FILTER_H\n#include FT_COLOR_H');p.write_text(s)
PY
 meson_dep cairo -Dtests=disabled -Dxlib=disabled -Dxcb=disabled -Dxlib-xcb=disabled -Dgtk2-utils=disabled -Dgtk_doc=false -Dtee=disabled -Ddwrite=disabled -Dquartz=disabled -Dspectre=disabled -Dsymbol-lookup=disabled -Dpng=disabled -Dzlib=disabled -Dglib=disabled
 meson_dep pango -Dintrospection=disabled -Ddocumentation=false -Dbuild-testsuite=false -Dbuild-examples=false -Dgtk_doc=false -Dlibthai=disabled -Dsysprof=disabled -Dxft=disabled
}
case ${1:-all} in small)small;;base)base;;textstack)textstack;;openssl)openssl_dep;;all)small;base;textstack;;*)echo 'Usage: rebuild-ohos-dependencies.sh small|base|textstack|openssl|all';exit 2;;esac
