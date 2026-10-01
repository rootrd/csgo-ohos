#!/usr/bin/env bash
# Native Linux/WSL build with no Windows path or executable dependencies.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
: "${OHOS_SDK:?Set OHOS_SDK to the native SDK directory (contains llvm and sysroot)}"
SDK=$(cd "$OHOS_SDK" && pwd)
source_dir="$root/deps/dxvk-ohos-legacy"
build_dir=${DXVK_BUILD_DIR:-$root/CSGO-Source-Linux-20260928/runtime/ohos/dxvk-build}
prefix=${DEPS_PREFIX:-$root/CSGO-Source-Linux-20260928/runtime/ohos/install}
jobs=${BUILD_JOBS:-4}
for tool in meson ninja glslangValidator pkg-config; do command -v "$tool" >/dev/null || { echo "Missing host tool: $tool" >&2; exit 1; }; done
for tool in clang clang++ llvm-ar llvm-strip; do test -x "$SDK/llvm/bin/$tool" || { echo "Missing SDK tool: $tool" >&2; exit 1; }; done
mkdir -p "$build_dir"
python3 - "$SDK" "$build_dir/ohos.cross" "$(command -v glslangValidator)" <<'PY'
import pathlib, sys
sdk, out, glslang = sys.argv[1:]
def q(s):
 if "'" in s or '\n' in s: raise SystemExit('Unsupported quote or newline in tool path')
 return "'" + s + "'"
args = [f'--target=aarch64-linux-ohos', f'--sysroot={sdk}/sysroot', '-D__OHOS__=1', '-fPIC']
link_args = args[:2] + ['-fuse-ld=lld', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384']
text = '[binaries]\n' + '\n'.join(f'{k} = {q(v)}' for k,v in {'c':f'{sdk}/llvm/bin/clang','cpp':f'{sdk}/llvm/bin/clang++','ar':f'{sdk}/llvm/bin/llvm-ar','strip':f'{sdk}/llvm/bin/llvm-strip','pkg-config':'pkg-config','glslangValidator':glslang}.items())
text += '\n[built-in options]\n' + '\n'.join(f'{k} = [{", ".join(q(a) for a in v)}]' for k,v in {'c_args':args,'cpp_args':args,'c_link_args':link_args,'cpp_link_args':link_args}.items())
text += "\n[properties]\nneeds_exe_wrapper = true\n[host_machine]\nsystem = 'linux'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n"
pathlib.Path(out).write_text(text)
PY
args=(--cross-file "$build_dir/ohos.cross" --buildtype release --default-library shared
      -Denable_tests=false -Denable_dxgi=true -Denable_d3d9=true
      -Denable_d3d10=false -Denable_d3d11=true -Dbuild_id=false -Dnative_ohos=true)
if [[ -f "$build_dir/build.ninja" ]]; then
 meson setup --reconfigure "$build_dir" "$source_dir" "${args[@]}"
else
 meson setup "$build_dir" "$source_dir" "${args[@]}"
fi
ninja -C "$build_dir" -j "$jobs"
mkdir -p "$prefix/lib" "$prefix/include/dxvk"
cp -a "$source_dir/include/native/windows/." "$prefix/include/dxvk/"
cp -a "$source_dir/include/native/directx/." "$prefix/include/dxvk/"
cp -a "$source_dir/include/native/wsi" "$source_dir/include/native/ohos" "$prefix/include/dxvk/"
install -m 755 "$build_dir/src/d3d9/d3d9.so" "$prefix/lib/libdxvk_d3d9.so"
cp -L "$build_dir/src/dxgi/libdxvk_dxgi.so.0" "$prefix/lib/libdxvk_dxgi.so.0"
"$SDK/llvm/bin/llvm-readelf" -h "$prefix/lib/libdxvk_d3d9.so" | grep -E 'Class:|Machine:'
printf 'DXVK built and staged: %s\n' "$prefix/lib"
