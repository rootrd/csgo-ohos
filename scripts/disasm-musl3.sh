#!/bin/bash
OBJ=/root/ohos-native/llvm/bin/llvm-objdump
M=/mnt/e/csgo/night-logs/ld-musl.aarch64.so
echo "=== 两个 svc 前后的完整 wrapper 代码（读 x8 系统调用号）==="
$OBJ -d --start-address=0xB8F60 --stop-address=0xB8FC0 "$M" 2>/dev/null | tail -30
echo
echo "=== 再往上找这些 wrapper 的边界 ==="
$OBJ -d --start-address=0xB8F00 --stop-address=0xB8F64 "$M" 2>/dev/null | tail -28
