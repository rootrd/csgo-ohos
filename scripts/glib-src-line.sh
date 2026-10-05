#!/bin/bash
A2L=/root/ohos-native/llvm/bin/llvm-addr2line
OBJ=/root/ohos-native/llvm/bin/llvm-objdump
NM=/root/ohos-native/llvm/bin/llvm-nm
G=/mnt/e/csgo/hap/entry/libs/arm64-v8a/libglib-2.0.so.0.8000.4
echo "=== addr2line lr=0x14F0CC / 前一指令 0x14F0C8（要源码行！）==="
$A2L -f -C -i -e "$G" 0x14F0CC 0x14F0C8
echo
echo "=== 0xfbc1c 到底是 g_free 还是 PLT 桩？==="
$OBJ -d --start-address=0xfbc00 --stop-address=0xfbc60 "$G" 2>/dev/null | tail -20
echo
echo "=== 0xfbc1c 附近符号 ==="
$NM -C --numeric-sort "$G" 2>/dev/null | awk '{a=strtonum("0x" $1); if (a>=0xfb000 && a<=0xfc500) print}' | head -20
