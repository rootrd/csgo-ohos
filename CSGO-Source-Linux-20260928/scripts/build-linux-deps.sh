#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -T -n dev -- env BUILD_JOBS="${BUILD_JOBS:-4}" bash "$repo_dir/scripts/build-linux-deps.sh" "$@"
fi
jobs=${BUILD_JOBS:-4}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'BUILD_JOBS must be positive.' >&2; exit 2; }
for tool in git meson ninja pkg-config clang clang++; do
    command -v "$tool" >/dev/null || { echo "Missing build tool in dev: $tool" >&2; exit 1; }
done
pkg-config --atleast-version=3.2 sdl3

work="$repo_dir/runtime/linux-sdl3"
prefix="$work/install"
dxvk="$work/deps/dxvk"
revision=6a0ea561f9add008899680e6c313aa21c151e03e
mkdir -p "$work/deps" "$prefix/lib"
if [[ ! -d $dxvk/.git ]]; then
    git clone --no-checkout --reference-if-able "$repo_dir/runtime/android/deps/dxvk" \
        https://github.com/Digger1955/dxvk-gplall.git "$dxvk"
    git -C "$dxvk" checkout --detach "$revision"
fi
[[ $(git -C "$dxvk" rev-parse HEAD) == "$revision" ]] || { echo "Preserving unexpected DXVK checkout: $dxvk" >&2; exit 1; }
git -C "$dxvk" submodule update --init --recursive
cp -- "$repo_dir/android/patches/dxvk-android.patch" "$work/dxvk-source.patch"
if ! git -C "$dxvk" apply --reverse --check "$work/dxvk-source.patch" 2>/dev/null; then
    git -C "$dxvk" apply --check "$work/dxvk-source.patch"
    git -C "$dxvk" apply "$work/dxvk-source.patch"
fi

setup=()
[[ ! -f $work/dxvk-build/meson-private/coredata.dat ]] || setup+=(--reconfigure)
CC=clang CXX=clang++ meson setup "${setup[@]}" "$work/dxvk-build" "$dxvk" \
    --prefix "$prefix" --libdir lib --buildtype debugoptimized \
    -Denable_d3d8=false -Denable_d3d9=true -Denable_d3d10=false \
    -Denable_d3d11=false -Denable_dxgi=false -Dnative_sdl2=disabled \
    -Dnative_sdl3=enabled -Dnative_glfw=disabled -Dbuild_id=true \
    -Dcpp_link_args=-latomic
meson compile -C "$work/dxvk-build" -j "$jobs"
meson install -C "$work/dxvk-build" --no-rebuild
sdl_libdir=$(pkg-config --variable=libdir sdl3)
cp -L --reflink=auto "$sdl_libdir/libSDL3.so.0" "$prefix/lib/libSDL3.so.0"
ln -sfn libSDL3.so.0 "$prefix/lib/libSDL3.so"
python3 - "$work" "$revision" <<'PY'
import hashlib, json, pathlib, subprocess, sys
work=pathlib.Path(sys.argv[1])
receipt={'dxvk_revision':sys.argv[2], 'patch_sha256':hashlib.sha256((work/'dxvk-source.patch').read_bytes()).hexdigest(),
         'sdl3_version':subprocess.check_output(['pkg-config','--modversion','sdl3'],text=True).strip(), 'wsi':'SDL3', 'platform':'Linux x86_64'}
(work/'dependencies.json').write_text(json.dumps(receipt,indent=2)+'\n')
PY
