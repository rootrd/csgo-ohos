#!/bin/bash
A2L=/root/ohos-native/llvm/bin/llvm-addr2line
T=/mnt/e/csgo/hap/entry/libs/arm64-v8a/libpanorama_text_pango_client.so
P=/mnt/e/csgo/hap/entry/libs/arm64-v8a/libpango-1.0.so.0.5400.0
echo "=== 调用方（我们的文本库 +0x86638）==="
$A2L -Cfipe "$T" 0x86638
echo "=== pango 崩溃点 +0x3dba8 ==="
$A2L -Cfipe "$P" 0x3dba8
echo "=== 我们库 0x86638 附近函数符号 ==="
/root/ohos-native/llvm/bin/llvm-nm -C --numeric-sort "$T" 2>/dev/null | awk '{a=strtonum("0x" $1); if (a>0x84000 && a<0x88000) print}' | tail -10
