#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -n dev -- env \
        BUILD_JOBS="${BUILD_JOBS:-4}" BUILD_CONFIG="${BUILD_CONFIG:-release}" \
        bash "$repo_dir/scripts/build-linux.sh" "$@"
fi

jobs=${BUILD_JOBS:-4}
config=${BUILD_CONFIG:-release}
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { echo 'BUILD_JOBS must be a positive integer.' >&2; exit 2; }
[[ $config == release || $config == debug ]] || { echo 'BUILD_CONFIG must be release or debug.' >&2; exit 2; }

for tool in clang clang++ make pkg-config; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
pkg-config --exists freetype2 sdl3
bash "$repo_dir/scripts/build-linux-deps.sh"
export LIBRARY_PATH="$repo_dir/runtime/linux-sdl3/install/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"

# Keep the generator's Clang objects separate from any older GCC build.
make -C "$repo_dir/src/utils/vpc" -j"$jobs" \
    CC=clang CXX=clang++ OUTDIR=obj/Linux/clang-release
cd "$repo_dir/src"
./devtools/bin/vpc_linux /csgo /linux64 +csgo_partner_linux_client \
    /nop4add /mksln csgo_partner /f

if (( $# == 0 )); then
    set -- all-targets
fi
exec make -f csgo_partner.mak -j"$jobs" --output-sync=target \
    CFG="$config" USE_STEAM_RUNTIME=1 CC=clang CXX=clang++ \
    VALVE_NO_AUTO_P4=1 "$@"
