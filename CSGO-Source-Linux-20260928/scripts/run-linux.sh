#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -n dev -- env CSGO_USE_STEAM="${CSGO_USE_STEAM:-0}" \
        bash "$repo_dir/scripts/run-linux.sh" "$@"
fi

resources_dir="$repo_dir/runtime/csgo-2019"
runtime_dir="$repo_dir/runtime/linux-sdl3/game"
deps_dir="$repo_dir/runtime/linux-sdl3/install/lib"
for required in "$resources_dir/csgo/gameinfo.txt" \
    "$resources_dir/bin/linux64/vphysics_client.so" \
    "$repo_dir/game/csgo_linux64" \
    "$repo_dir/game/bin/linux64/launcher_client.so" \
    "$repo_dir/game/bin/linux64/shaderapidx9_client.so" \
    "$repo_dir/game/csgo/bin/linux64/client_panorama_client.so" \
    "$repo_dir/game/csgo/bin/linux64/server_client.so" \
    "$deps_dir/libSDL3.so.0" "$deps_dir/libdxvk_d3d9.so.0"; do
    [[ -f $required ]] || { echo "Missing runtime or build output: $required" >&2; exit 1; }
done
if ! readelf -d "$repo_dir/game/bin/linux64/launcher_client.so" | rg -q '\[libSDL3\.so\.0\]'; then
    echo 'The launcher is from the SDL2 build. Run scripts/build-linux.sh before launching SDL3.' >&2
    exit 1
fi

# Keep configs, captures and rebuilt modules in an independent SDL3 runtime.
if [[ ! -f $runtime_dir/.sdl3-resources-ready ]]; then
    [[ ! -e $runtime_dir ]] || { echo "Unrecognized runtime directory: $runtime_dir" >&2; exit 1; }
    mkdir -p "$(dirname -- "$runtime_dir")"
    stage_dir=$(mktemp -d "${runtime_dir}.XXXXXX")
    trap 'rm -rf -- "$stage_dir"' EXIT
    cp -a --reflink=auto "$resources_dir/." "$stage_dir/"
    touch "$stage_dir/.sdl3-resources-ready"
    mv -- "$stage_dir" "$runtime_dir"
    trap - EXIT
fi
cp -a --reflink=auto "$repo_dir/game/." "$runtime_dir/"
cd "$runtime_dir"
export LD_LIBRARY_PATH="$deps_dir:$runtime_dir/bin/linux64:$runtime_dir/csgo/bin/linux64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DXVK_WSI_DRIVER=SDL3
export SOURCE_VULKAN_SHADERS="${SOURCE_VULKAN_SHADERS:-$repo_dir/runtime/vulkan/shaders}"
export SteamAppId=730
# Use SteamOS's PulseAudio interface from Distrobox. The native OpenAL
# PipeWire backend fails to connect here and aborts during D-Bus shutdown.
export ALSOFT_DRIVERS="${ALSOFT_DRIVERS:-pulse}"
# Same mobile menu the phone uses. Copied every launch so style edits do not need a client rebuild.
# -nomobileui returns to the console entry. The APK still packages these files only when Android is rebuilt.
ui_src="$repo_dir/android/app/src/main/assets/mobile_ui"
ui_root="$repo_dir/runtime/linux-sdl3/mobile-ui/panorama/mobile"
mkdir -p "$ui_root"
for ui_name in base.xml menu.xml menu.css menu.js; do
    [[ -f $ui_src/$ui_name ]] || { echo "Missing mobile UI asset: $ui_src/$ui_name" >&2; exit 1; }
done
cp -a --reflink=auto "$ui_src/." "$ui_root/"
export CSGO_MOBILE_UI_PATH="$repo_dir/runtime/linux-sdl3/mobile-ui"
launch_args=(-insecure -novid)
case ${CSGO_USE_STEAM:-0} in
    0) launch_args+=(-nosteam -console) ;;
    1) ;;
    *) echo 'CSGO_USE_STEAM must be 0 or 1.' >&2; exit 2 ;;
esac
exec ./csgo_linux64 "${launch_args[@]}" "$@"
