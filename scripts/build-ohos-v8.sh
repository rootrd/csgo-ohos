#!/usr/bin/env bash
# Build the real ABI-compatible V8 runtime. This does not prove that the target
# app sandbox permits V8 5.8's executable-memory allocations. No JIT permission
# or system security setting is changed here.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build_root=${V8_BUILD_ROOT:-"$root/CSGO-Source-Linux-20260928/runtime/ohos/v8-build"}
downloads=${V8_DOWNLOAD_DIR:-"$build_root/downloads"}
source_dir=${V8_SOURCE_DIR:-"$build_root/v8-5.8.283"}
install_dir=${V8_INSTALL_DIR:-"$root/CSGO-Source-Linux-20260928/src/lib/common/androidarm64"}
jobs=${BUILD_JOBS:-3}
host_cc=${HOST_CC:-clang}
host_cxx=${HOST_CXX:-clang++}
: "${OHOS_SDK:?Set OHOS_SDK to the official SDK native directory}"
test -x "$OHOS_SDK/llvm/bin/clang++"
test -d "$OHOS_SDK/sysroot"
command -v "$host_cc" >/dev/null
command -v "$host_cxx" >/dev/null
command -v python3 >/dev/null
mkdir -p "$downloads" "$build_root" "$install_dir"

verify() { printf '%s  %s\n' "$2" "$1" | sha256sum -c -; }
fetch() {
    local name=$1 url=$2 digest=$3
    if [[ ! -f "$downloads/$name" ]]; then
        curl --fail --location --retry 2 --max-time 240 "$url" -o "$downloads/$name.part"
        verify "$downloads/$name.part" "$digest"
        mv "$downloads/$name.part" "$downloads/$name"
    fi
    verify "$downloads/$name" "$digest"
}
fetch_text() {
    local destination=$1 url=$2 digest=$3
    if [[ ! -f "$destination" ]]; then
        mkdir -p "$(dirname "$destination")"
        curl --fail --location --retry 2 --max-time 120 "$url" -o "$destination.base64"
        base64 -d "$destination.base64" > "$destination"
    fi
    verify "$destination" "$digest"
}

fetch v8-5.8.283.tar.gz \
    https://codeload.github.com/v8/v8/tar.gz/refs/tags/5.8.283 \
    4c4b8b9a8763df3a67ac1bcf3d56b9cfdadc781d4889d91d8dca5ebd73664421
if [[ ! -f "$source_dir/src/v8.gyp" ]]; then
    mkdir -p "$source_dir"
    tar -xzf "$downloads/v8-5.8.283.tar.gz" -C "$source_dir" --strip-components=1
fi
# Ensure that the engine and dependency build use the same public API.
cmp "$source_dir/include/v8.h" "$root/CSGO-Source-Linux-20260928/src/thirdparty/v8/include/v8.h"

# googlesource +archive gzip metadata is not reproducible. Keep the original
# revision, verify its official Git commit/tree, and cache a deterministic tar.
# Verify the archive on EVERY cache hit; never trust only gyp_main.py's presence.
gyp_revision=e7079f0e0e14108ab0dba58728ff219637458563
gyp_tree=ea08eb644f21477d1f0dd77d3052e306d0a9da04
gyp_tar_sha256=435def02979d91b9ec743785a5893e66248e4d9061f4349b9f9a723e8aba4b84
gyp_archive="$downloads/gyp-$gyp_revision.tar"
if [[ ! -f "$gyp_archive" ]]; then
    command -v git >/dev/null
    gyp_git="$build_root/gyp-official.git"
    if [[ ! -d "$gyp_git" ]]; then
        git init --bare "$gyp_git"
    fi
    git -C "$gyp_git" fetch --no-tags --depth=1 \
        https://chromium.googlesource.com/external/gyp "$gyp_revision"
    test "$(git -C "$gyp_git" rev-parse FETCH_HEAD)" = "$gyp_revision"
    test "$(git -C "$gyp_git" rev-parse "$gyp_revision^{tree}")" = "$gyp_tree"
    git -C "$gyp_git" fsck --strict --no-dangling
    git -C "$gyp_git" archive --format=tar "$gyp_revision" > "$gyp_archive.part"
    verify "$gyp_archive.part" "$gyp_tar_sha256"
    mv "$gyp_archive.part" "$gyp_archive"
fi
verify "$gyp_archive" "$gyp_tar_sha256"
# Fresh extraction prevents stale or altered sources/bytecode from bypassing
# verification. Preserve the previous generated tree instead of deleting it.
gyp_export=$(mktemp -d "$build_root/gyp-verified.XXXXXX")
tar -xf "$gyp_archive" -C "$gyp_export"
mkdir -p "$source_dir/tools"
if [[ -e "$source_dir/tools/gyp" || -L "$source_dir/tools/gyp" ]]; then
    gyp_backup=$(mktemp -d "$build_root/gyp-previous.XXXXXX")
    mv "$source_dir/tools/gyp" "$gyp_backup/tree"
fi
mv "$gyp_export" "$source_dir/tools/gyp"
fetch_text "$source_dir/base/trace_event/common/trace_event_common.h" \
    'https://chromium.googlesource.com/chromium/src/base/trace_event/common/+/06294c8a4a6f744ef284cd63cfe54dbf61eea290/trace_event_common.h?format=TEXT' \
    36f266066214cc8c3ee8e2b11fb943148285cc6f8f86986af949281684809902
fetch_text "$source_dir/tools/clang/scripts/update.py" \
    'https://chromium.googlesource.com/chromium/src/tools/clang/+/9913fb19b687b0c858f697efd7bd2468d789a3d5/scripts/update.py?format=TEXT' \
    19029611d73de2e38479d3d16e29acaac7ceda09d216bf1a093a491cd2869d93
fetch gtest.tar.gz \
    https://codeload.github.com/google/googletest/tar.gz/6f8a66431cb592dad629028a50b3dd418a408c87 \
    31e3aef71150696d9df8cc2ec195ab31a2793b999fe139c62e23798c7569239e
if [[ ! -f "$source_dir/testing/gtest/include/gtest/gtest_prod.h" ]]; then
    mkdir -p "$build_root/gtest" "$source_dir/testing/gtest/include/gtest"
    tar -xzf "$downloads/gtest.tar.gz" -C "$build_root/gtest" --strip-components=1
    cp "$build_root/gtest/include/gtest/gtest_prod.h" "$source_dir/testing/gtest/include/gtest/"
fi
verify "$source_dir/testing/gtest/include/gtest/gtest_prod.h" \
    4a99a3d986a45b4d6d9b3af54809f015c54aa98274793a4ae173f5010d0ad33c

# Python 2 is only a build-time requirement of this pinned 2017 source tree.
python2=${PYTHON2:-"$build_root/python2/bin/python2.7"}
if [[ ! -x "$python2" ]]; then
    fetch Python-2.7.18.tgz https://www.python.org/ftp/python/2.7.18/Python-2.7.18.tgz \
        da3080e3b488f648a3d7a4560ddee895284c3380b11d6de75edb986526b9a814
    if [[ ! -d "$build_root/Python-2.7.18" ]]; then
        tar -xzf "$downloads/Python-2.7.18.tgz" -C "$build_root"
    fi
    (
        cd "$build_root/Python-2.7.18"
        CC="$host_cc" ./configure --prefix="$build_root/python2" --without-ensurepip
        make -j"$jobs"
        make install
    )
    python2="$build_root/python2/bin/python2.7"
fi
"$python2" -c 'import sys; assert sys.version_info[:2] == (2, 7)'
python2=$(realpath "$python2")
# Generated actions invoke "python", not PYTHON2. Keep that choice local.
mkdir -p "$build_root/host-bin"
ln -sfn "$(realpath "$python2")" "$build_root/host-bin/python"
export PATH="$build_root/host-bin:$PATH"

# Preserve the library names used by the checked-in VPC files, including their
# actual SONAMEs and inter-library DT_NEEDED references, rather than just copying
# a differently named binary. This patch changes no JavaScript implementation.
python3 - "$source_dir/src/v8.gyp" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
data = p.read_text()
for name in ("v8", "v8_libbase", "v8_libplatform"):
    marker = "'target_name': '%s'," % name
    replacement = marker + "\n      'product_name': '%s.cr'," % name
    if replacement not in data:
        if data.count(marker) != 1:
            raise SystemExit("Unexpected V8 GYP target: " + name)
        data = data.replace(marker, replacement)
p.write_text(data)
PY

(
    cd "$source_dir"
    GYP_GENERATORS=make "$python2" gypfiles/gyp_v8 src/v8.gyp \
        -Dtarget_arch=arm64 -Dv8_target_arch=arm64 -Dv8_use_snapshot=false \
        -Dv8_use_external_startup_data=0 -Dv8_enable_i18n_support=0 \
        -Dv8_enable_inspector=0 -Dcomponent=shared_library -Dclang=0 \
        -Dwerror='' -Dwant_separate_host_toolset=1
    make -C out BUILDTYPE=Release -j"$jobs" \
        "CC.host=$host_cc" "CXX.host=$host_cxx" "LINK.host=$host_cxx" \
        "CC.target=$OHOS_SDK/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=$OHOS_SDK/sysroot" \
        "CXX.target=$OHOS_SDK/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$OHOS_SDK/sysroot -stdlib=libc++" \
        "LINK.target=$OHOS_SDK/llvm/bin/clang++ --target=aarch64-linux-ohos --sysroot=$OHOS_SDK/sysroot -stdlib=libc++" \
        "AR.target=$OHOS_SDK/llvm/bin/llvm-ar" \
        'LDFLAGS.target=-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384' \
        Release/lib.target/libv8.cr.so Release/lib.target/libv8_libplatform.cr.so
)
for name in v8 v8_libbase v8_libplatform; do
    cp "$source_dir/out/Release/lib.target/lib$name.cr.so" "$install_dir/"
    "$OHOS_SDK/llvm/bin/llvm-readelf" -h -d "$install_dir/lib$name.cr.so"
done
{
    printf '%s\n\n' 'V8 5.8.283: https://github.com/v8/v8/tree/5.8.283'
    for name in LICENSE LICENSE.v8 LICENSE.strongtalk LICENSE.valgrind LICENSE.fdlibm; do
        printf '\n===== %s =====\n\n' "$name"
        cat "$source_dir/$name"
    done
} > "$install_dir/V8-5.8.283-NOTICES.txt"
"$OHOS_SDK/llvm/bin/clang++" --target=aarch64-linux-ohos --sysroot="$OHOS_SDK/sysroot" \
    -stdlib=libc++ -std=c++11 -Wno-deprecated-declarations \
    -I "$root/CSGO-Source-Linux-20260928/src/thirdparty/v8/include" \
    "$root/CSGO-Source-Linux-20260928/ohos/tests/panorama_v8_smoke.cpp" \
    -L "$install_dir" -Wl,-rpath-link,"$install_dir" \
    -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
    -l:libv8.cr.so -l:libv8_libplatform.cr.so -l:libv8_libbase.cr.so \
    -o "$install_dir/panorama_v8_smoke"
(
    cd "$install_dir"
    sha256sum libv8*.cr.so panorama_v8_smoke > v8-build.sha256
)
printf '%s\n' \
    'Built real V8 5.8.283 for ARM64 OHOS, without Intl or external snapshots.' \
    'NOT runtime-verified: run panorama_v8_smoke in the game app sandbox.' \
    'V8 5.8 still requires executable memory. No jitless claim is made.'
