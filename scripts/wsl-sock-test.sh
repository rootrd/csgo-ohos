#!/usr/bin/env bash
echo "=== net_sockets.c lines 50-70 ==="
sed -n '50,70p' /root/csgo-src/CSGO-Source-Linux-20260928/runtime/ohos/deps/mbedtls/library/net_sockets.c
echo "=== musl sys/socket.h sockaddr_storage ==="
grep -n "sockaddr_storage" /root/ohos-native/sysroot/usr/include/sys/socket.h /root/ohos-native/sysroot/usr/include/bits/socket.h 2>/dev/null | head -5
echo "=== compile probe ==="
cat > /tmp/sock.c <<'EOF'
#include <sys/socket.h>
#include <netinet/in.h>
int main() { struct sockaddr_storage s; return (int)sizeof(s); }
EOF
/root/ohos-native/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=/root/ohos-native/sysroot -c /tmp/sock.c -o /tmp/sock.o && echo SOCK_PROBE_OK || echo SOCK_PROBE_FAIL
echo "=== probe with mbedtls-style include of netinet first ==="
cat > /tmp/sock2.c <<'EOF'
#include <netinet/in.h>
#include <sys/socket.h>
int main() { struct sockaddr_storage s; return (int)sizeof(s); }
EOF
/root/ohos-native/llvm/bin/clang --target=aarch64-linux-ohos --sysroot=/root/ohos-native/sysroot -c /tmp/sock2.c -o /tmp/sock2.o && echo SOCK2_OK || echo SOCK2_FAIL
