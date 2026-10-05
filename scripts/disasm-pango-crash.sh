#!/bin/bash
OBJ=/root/ohos-native/llvm/bin/llvm-objdump
NM=/root/ohos-native/llvm/bin/llvm-nm
P=/mnt/e/csgo/hap/entry/libs/arm64-v8a/libpango-1.0.so.0.5400.0
T=/mnt/e/csgo/hap/entry/libs/arm64-v8a/libpanorama_text_pango_client.so
echo "=== pango 崩溃点 +0x3dba8 上下文（找 faulting 指令/寄存器）==="
$OBJ -d --start-address=0x3db40 --stop-address=0x3dbd0 "$P" 2>/dev/null | tail -25
echo "=== pango 0x3dba8 所在函数（nm 边界）==="
$NM -C --numeric-sort "$P" 2>/dev/null | awk '{a=strtonum("0x" $1); if (a>0x3d000 && a<0x3e400) print}' | tail -8
echo
echo "=== 我们的 BInitialize 调用点 lr-4=0x86634 附近 ==="
$OBJ -d --start-address=0x865c0 --stop-address=0x86680 "$T" 2>/dev/null | tail -34
