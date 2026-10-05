#!/bin/bash
# pc = ld-musl 基址(0x59cd150000) + 0xB8FB8；找该处的符号 + svc 指令
OBJ=/root/ohos-native/llvm/bin/llvm-objdump
NM=/root/ohos-native/llvm/bin/llvm-nm
M=/mnt/e/csgo/night-logs/ld-musl.aarch64.so
echo "=== 0xB8FB8 附近 disasm（含 svc 检测）==="
$OBJ -d --start-address=0xB8E80 --stop-address=0xB90C0 "$M" 2>/dev/null | tail -45
echo "=== 全窗 svc 指令位置 ==="
$OBJ -d --start-address=0xB8000 --stop-address=0xB9800 "$M" 2>/dev/null | grep -n "svc" | head -10
echo "=== 符号表里 0xB8FB8 前方最近符号 ==="
$NM -C --numeric-sort "$M" 2>/dev/null | awk '{a=strtonum("0x" $1); if (a<=0xB8FB8 && a>0xB7000) print}' | tail -8
