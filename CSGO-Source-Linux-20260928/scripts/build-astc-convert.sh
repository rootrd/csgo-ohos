#!/usr/bin/env bash
set -euo pipefail
repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
if [[ ${CONTAINER_ID:-} != dev ]]; then
    exec distrobox enter -T -n dev -- bash "$repo_dir/scripts/build-astc-convert.sh" "$@"
fi
if [[ ${1:-build} == package ]]; then
    shift
    exec python3 "$repo_dir/scripts/astc-convert/package_pack.py" "$@"
fi
tools_dir="$repo_dir/runtime/astc/tools"
build_dir="$repo_dir/runtime/astc/build"
mkdir -p "$tools_dir/include/nlohmann"
fetch() {
    local url=$1 output=$2 checksum=$3
    if [[ ! -f $output ]]; then
        curl --fail --silent --show-error --location --retry 3 -o "$output.partial" "$url"
        mv -- "$output.partial" "$output"
    fi
    printf '%s  %s\n' "$checksum" "$output" | sha256sum --check --status
}
fetch https://codeload.github.com/ARM-software/astc-encoder/tar.gz/refs/tags/5.7.0 \
    "$tools_dir/astc-encoder-5.7.0.tar.gz" 7c1b28ece59c9c2737e297123a9910c1c548676c7aed87a09d5734e2fa7bdaf0
fetch https://github.com/KhronosGroup/KTX-Software/releases/download/v4.4.2/KTX-Software-4.4.2-Linux-x86_64.tar.bz2 \
    "$tools_dir/KTX-Software-4.4.2-Linux-x86_64.tar.bz2" a8781bad05f9624edbf910b7f258cd0a4ba7d3e63b49ecc0a0ab440bf6a0a245
fetch https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp \
    "$tools_dir/include/nlohmann/json.hpp" aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63
if [[ ! -d $tools_dir/astc-encoder-5.7.0 ]]; then
    tar -xzf "$tools_dir/astc-encoder-5.7.0.tar.gz" -C "$tools_dir" --no-same-owner
fi
if [[ ! -d $tools_dir/KTX-Software-4.4.2-Linux-x86_64 ]]; then
    tar -xjf "$tools_dir/KTX-Software-4.4.2-Linux-x86_64.tar.bz2" -C "$tools_dir" --no-same-owner
fi
cmake -S "$repo_dir/scripts/astc-convert" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DASTCENC_ROOT="$tools_dir/astc-encoder-5.7.0" \
    -DKTX_ROOT="$tools_dir/KTX-Software-4.4.2-Linux-x86_64" -DJSON_ROOT="$tools_dir/include"
cmake --build "$build_dir" --target vtf2astc astcenc-native --parallel "${ASTC_BUILD_JOBS:-8}"
case ${1:-build} in
    build) ;;
    run) shift; exec "$build_dir/vtf2astc" "$@" ;;
    test)
        python3 "$repo_dir/scripts/astc-convert/tests/test_converter.py" --build "$build_dir"
        exec python3 "$repo_dir/scripts/astc-convert/tests/test_package.py"
        ;;
    *) printf 'Usage: %s [build|run <arguments>|test|package <arguments>]\n' "$0" >&2; exit 2 ;;
esac
