#!/usr/bin/env bash
set -ex
NATIVE=/mnt/e/ohos-cli/command-line-tools/sdk/default/openharmony/native
cp -p "$NATIVE"/oh-uni-package.json /root/ohos-native/ || true
ls "$NATIVE" | head
tar -C "$NATIVE" -cf - build-tools | tar -C /root/ohos-native -xf -
ls /root/ohos-native/build-tools/cmake/bin/
/root/ohos-native/build-tools/cmake/bin/cmake --version | head -1
