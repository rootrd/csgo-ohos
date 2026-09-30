#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -T -n dev -- bash "$repo_dir/scripts/build-vulkan-shaders.sh" "$@"
fi
revision=v1.8.2505.1
checksum=f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c
tools_dir="$repo_dir/runtime/vulkan/tools"
archive="$tools_dir/linux_dxc_2025_07_14.x86_64.tar.gz"
compiler_dir="$tools_dir/dxc-$revision"
mkdir -p "$tools_dir"
if [[ ! -f $archive ]]; then
    curl --fail --location --retry 3 --output "$archive.partial" \
        "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$revision/linux_dxc_2025_07_14.x86_64.tar.gz"
    mv -- "$archive.partial" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha256sum --check --status
if [[ ! -x $compiler_dir/bin/dxc ]]; then
    mkdir -p "$compiler_dir"
    tar -xzf "$archive" -C "$compiler_dir" --no-same-owner
fi
python3 "$repo_dir/scripts/prepare-vulkan-shaders.py" "$repo_dir/runtime/vulkan/shader-source"
exec python3 "$repo_dir/scripts/compile-vulkan-shaders.py" --dxc "$compiler_dir/bin/dxc" \
    --source "$repo_dir/runtime/vulkan/shader-source" "$@"
