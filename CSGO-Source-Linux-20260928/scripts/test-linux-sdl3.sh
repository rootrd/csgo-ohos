#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -T -n dev -- bash "$repo_dir/scripts/test-linux-sdl3.sh" "$@"
fi
out="$repo_dir/runtime/linux-sdl3/tests"
libs="$repo_dir/src/lib/public/linux64"
bin="$repo_dir/game/bin/linux64"
deps="$repo_dir/runtime/linux-sdl3/install/lib"
mkdir -p "$out"
clang++ -std=c++11 -g -w -D_GLIBCXX_USE_CXX11_ABI=0 \
    -DPOSIX -D_POSIX -DLINUX -D_LINUX -DGNUC -DCOMPILER_GCC -DCSTRIKE15 \
    -DRAD_TELEMETRY_DISABLED -DUSE_SDL -DUSE_SDL3 -DUSE_DXVK_NATIVE \
    -I"$repo_dir/runtime/linux-sdl3/install/include/dxvk" \
    -I"$repo_dir/src/public" -I"$repo_dir/src/common" $(pkg-config --cflags sdl3) \
    "$repo_dir/src/appframework/tests/sdl3_platform_test.cpp" \
    -Wl,--start-group "$libs/appframework_client.a" "$libs/tier1_client.a" "$libs/mathlib_client.a" \
    "$libs/interfaces_client.a" \
    -Wl,--end-group -L"$bin" -L"$deps" -ltier0_client -lvstdlib_client -lSDL3 -ldl -lpthread \
    -o "$out/sdl3-platform-test"
LD_LIBRARY_PATH="$deps:$bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}" "$out/sdl3-platform-test" "$@"
