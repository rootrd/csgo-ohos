#!/bin/bash
# 分析 glib lr=0x14F0CC 处调用了什么 + 符号边界
OBJ=/root/ohos-native/llvm/bin/llvm-objdump
NM=/root/ohos-native/llvm/bin/llvm-nm
G=/mnt/e/csgo/hap/entry/libs/arm64-v8a/libglib-2.0.so.0.8000.4
M=/mnt/e/csgo/night-logs/ld-musl.aarch64.so
echo "=== glib symbols around 0x14F0CC ==="
$NM -C --defined-only --numeric-sort "$G" 2>/dev/null | awk '{a=strtonum("0x" $1); if (a>0x140000 && a<0x150500) print}' | tail -20
echo "=== glib disasm 0x14F080-0x14F0E0 (lr=0x14F0CC) ==="
$OBJ -d --start-address=0x14F080 --stop-address=0x14F0E0 "$G" 2>/dev/null | tail -28
echo "=== musl symbols around 0xB7FB8 ==="
$NM -C --defined-only --numeric-sort "$M" 2>/dev/null | awk '{a=strtonum("0x" $1); if (a>0xB7E00 && a<0xB8200) print}' | tail -14
echo "=== musl disasm 0xB7F80-0xB7FE8 ==="
$OBJ -d --start-address=0xB7F80 --stop-address=0xB7FE8 "$M" 2>/dev/null | tail -22
