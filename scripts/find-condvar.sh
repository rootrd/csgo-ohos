#!/bin/bash
NM=/root/ohos-native/llvm/bin/llvm-nm
G=/mnt/e/csgo/hap/entry/libs/arm64-v8a/libglib-2.0.so.0.8000.4
# x0 = 条件变量地址 0x5de2a62698；本 run glib r-xp 起点 = lr - 0xA00CC = 0x5de2933000，p_off=0xaf000
# load_base = 0x5de2933000 - 0xaf000 = 0x5de2884000
# x0 的模块内偏移 = 0x5de2a62698 - 0x5de2884000 = 0x1DE698
OFF=$((0x1DE698))
echo "目标偏移: 0x$(printf %x $OFF)"
echo "=== glib 符号表中 0x1DD000-0x1E0000 范围（含 static 数据符号）==="
$NM -C --numeric-sort "$G" 2>/dev/null | awk -v lo=0x1DC000 -v hi=0x1E2000 '{a=strtonum("0x" $1); if (a>=lo && a<=hi) print}'
echo
echo "=== 直接找 once / cond / mutex 相关静态符号 ==="
$NM -C "$G" 2>/dev/null | grep -iE "once_cond|once_mutex|g_once|deprecat" | head -12
echo
echo "=== 所有 static GCond/GMutex 候选（b/B/d 段里带 cond/mutex/lock 的本地符号）==="
$NM -C --numeric-sort "$G" 2>/dev/null | grep -E " [bBdD] " | grep -iE "cond|mutex|lock|once" | head -20
