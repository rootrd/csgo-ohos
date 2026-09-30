#!/usr/bin/env bash
cd /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/mbedtls-build
/root/ohos-native/llvm/bin/clang --target=aarch64-linux-ohos --gcc-toolchain=/root/ohos-native/llvm --sysroot=/root/ohos-native/sysroot \
  -I/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/deps/mbedtls/include \
  -I/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/deps/mbedtls/library \
  -I/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/mbedtls-build/library \
  -D__MUSL__ -O2 -DNDEBUG -std=c99 -fPIC \
  -E /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/deps/mbedtls/library/net_sockets.c 2>/dev/null | grep -c "struct sockaddr_storage {" || true
echo "---which headers included---"
/root/ohos-native/llvm/bin/clang --target=aarch64-linux-ohos --gcc-toolchain=/root/ohos-native/llvm --sysroot=/root/ohos-native/sysroot \
  -I/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/deps/mbedtls/include \
  -I/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/deps/mbedtls/library \
  -I/root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/mbedtls-build/library \
  -D__MUSL__ -std=c99 -H -E /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/deps/mbedtls/library/net_sockets.c 2>/dev/null | grep -E "socket" | head -10
