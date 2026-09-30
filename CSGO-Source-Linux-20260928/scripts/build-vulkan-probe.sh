#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -T -n dev -- env BUILD_JOBS="${BUILD_JOBS:-4}" \
        BUILD_CONFIG="${BUILD_CONFIG:-debug}" bash "$repo_dir/scripts/build-vulkan-probe.sh" "$@"
fi
config=${BUILD_CONFIG:-debug}
jobs=${BUILD_JOBS:-4}
[[ $config == debug || $config == release ]] || { echo 'BUILD_CONFIG must be debug or release.' >&2; exit 2; }
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'BUILD_JOBS must be positive.' >&2; exit 2; }
type=Debug
[[ $config != release ]] || type=RelWithDebInfo
output="$repo_dir/runtime/vulkan/$config"
action=${1:-build}
if (($#)); then shift; fi
case "$action" in build|run|test|framework-test|api-test) ;; *) echo 'Usage: build-vulkan-probe.sh [build|run|test|framework-test|api-test] [probe options]' >&2; exit 2;; esac
if [[ $action == test || $action == framework-test || $action == api-test ]]; then
    bash "$repo_dir/scripts/build-vulkan-shaders.sh"
fi
cmake -S "$repo_dir/src/materialsystem/shaderapivulkan" -B "$output/build" -G Ninja -DCMAKE_BUILD_TYPE="$type"
cmake --build "$output/build" --parallel "$jobs"
if [[ $action == test ]]; then
    # Deterministic programmatic minimize/restore needs X11 (including XWayland).
    # Wayland activation may be denied without a user-input serial. Normal runs
    # continue to use SDL's preferred driver unless explicitly overridden.
    SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-x11} "$output/build/csgo-vulkan-probe" --validation --self-test --seconds 6 --output "$output/self-test" "$@"
fi
if [[ $action == test || $action == framework-test ]]; then
    "$output/build/csgo-vulkan-graphics-probe" "$repo_dir/runtime/vulkan/shaders" "$output/framework-test"
fi
if [[ $action == test || $action == framework-test || $action == api-test ]]; then
    exec "$output/build/csgo-vulkan-api-probe" "$repo_dir/runtime/vulkan/shaders" "$output/api-test"
elif [[ $action == run ]]; then
    exec "$output/build/csgo-vulkan-probe" --output "$output/run" "$@"
fi
