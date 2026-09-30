#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -T -n dev -- env BUILD_JOBS="${BUILD_JOBS:-4}" \
        bash "$repo_dir/scripts/build-vulkan-module.sh" "$@"
fi
jobs=${BUILD_JOBS:-4}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'BUILD_JOBS must be positive.' >&2; exit 2; }
bash "$repo_dir/scripts/build-vulkan-shaders.sh"
output="$repo_dir/runtime/vulkan/module"
cmake -S "$repo_dir/src/materialsystem/shaderapivulkan" -B "$output/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++
cmake --build "$output/build" --target shaderapivulkan stdshader_vulkan --parallel "$jobs"
install -m 755 "$output/build/shaderapivulkan_client.so" "$repo_dir/game/bin/linux64/"
install -m 755 "$output/build/stdshader_vulkan_client.so" "$repo_dir/game/bin/linux64/"
