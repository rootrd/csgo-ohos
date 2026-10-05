#!/bin/bash
NM=/root/ohos-native/llvm/bin/llvm-nm
OBJ=/root/ohos-native/llvm/bin/llvm-objdump
P=/mnt/e/csgo/hap/entry/libs/arm64-v8a/libpango-1.0.so.0.5400.0
echo "=== 相关符号地址 ==="
$NM -C --numeric-sort "$P" 2>/dev/null | grep -E " (pango_font_map_list_families|pango_font_map_get_families|pango_font_map_get_type_once|pango_font_map_get_type)$" | head
ADDR=$($NM -C "$P" 2>/dev/null | grep " pango_font_map_list_families$" | awk '{print $1}')
echo "pango_font_map_list_families @ 0x$ADDR"
echo "=== 反汇编（前 80 条指令）==="
python3 - "$ADDR" <<'PY' > /tmp/lf_addr.txt
import sys
a=int(sys.argv[1],16)
print(a); print(hex(a+0x200))
PY
read A B < <(cat /tmp/lf_addr.txt)
$OBJ -d --start-address=$A --stop-address=$B "$P" 2>/dev/null | head -90
