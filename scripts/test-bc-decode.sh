#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
dxvk="$root/deps/dxvk-ohos-legacy"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"${CXX:-g++}" -std=c++17 -O1 -g ${BC_TEST_SANITIZERS:--fsanitize=address,undefined} \
  -I"$dxvk/include" -I"$dxvk/src" \
  "$root/scripts/tests/test_bc_decode.cpp" "$dxvk/src/util/util_bc.cpp" \
  "$dxvk/src/util/etcpak/ProcessRGB.cpp" "$dxvk/src/util/etcpak/Tables.cpp" \
  -o "$tmp/test-bc"
"$tmp/test-bc"
