#!/usr/bin/env bash
# One-time setup: copy source tree + OHOS SDK into WSL ext4 (build area).
# The WSL vhdx lives on E:\WSL\Ubuntu2404, so this still satisfies the E-drive rule.
set -ex

# 1) Source tree
rm -rf ~/csgo-src
mkdir -p ~/csgo-src
time tar -C /mnt/e/csgo -cf - CSGO-Source-Linux-20260928 | tar -C ~/csgo-src -xf -
echo "SOURCE_COPY_DONE"

# 2) OHOS SDK native toolchain (llvm + sysroot + cmake toolchain)
rm -rf ~/ohos-native
mkdir -p ~/ohos-native
NATIVE=/mnt/e/ohos-cli/command-line-tools/sdk/default/openharmony/native
time tar -C "$NATIVE" -cf - llvm sysroot build | tar -C ~/ohos-native -xf -
echo "SDK_COPY_DONE"

ls ~/csgo-src/CSGO-Source-Linux-20260928/ | head
du -sh ~/csgo-src ~/ohos-native
