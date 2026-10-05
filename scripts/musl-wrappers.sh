#!/bin/bash
OBJ=/root/ohos-native/llvm/bin/llvm-objdump
NM=/root/ohos-native/llvm/bin/llvm-nm
M=/mnt/e/csgo/night-logs/ld-musl.aarch64.so
G=/mnt/e/csgo/hap/entry/libs/arm64-v8a/libglib-2.0.so.0.8000.4
echo "=== musl 是否有 close_range / fork / wait4 / execve 符号 ==="
$NM -C --defined-only "$M" 2>/dev/null | grep -E " (close_range|fork|wait4|execve|clone)$" | head -8
echo "=== musl <fork> 代码（看是 inline svc 还是 bl syscall）==="
$OBJ -d "$M" 2>/dev/null | grep -A 12 "<fork>:" | head -14
echo "=== fork_exec 0x14ef00-0x14f100 内所有 bl 目标 ==="
$OBJ -d --start-address=0x14ef00 --stop-address=0x14f100 "$G" 2>/dev/null | grep -E "bl\s" | head -25
