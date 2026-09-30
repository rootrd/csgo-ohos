#!/bin/bash
set -ex

SYSROOT=/mnt/e/csgo/ohos-sdk/sysroot
CLANG_RT_DIR=/mnt/e/csgo/ohos-sdk/clang/15.0.4/lib/aarch64-linux-ohos
RESOURCE_DIR=/mnt/e/csgo/ohos-sdk/clang/15.0.4

cat > /tmp/test_ohos.c <<'EOF'
#include <stdio.h>
int main() { printf("hello ohos\n"); return 0; }
EOF

clang --target=aarch64-linux-ohos \
  --sysroot=${SYSROOT} \
  -fuse-ld=lld \
  -resource-dir=${RESOURCE_DIR} \
  -B${SYSROOT}/usr/lib/aarch64-linux-ohos \
  -o /tmp/test_ohos /tmp/test_ohos.c 2>&1

echo "Exit code<parameter name="command">wsl -d Ubuntu-24.04 -- bash /mnt/e/csgo/test_build.sh 2>&1