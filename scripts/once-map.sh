#!/bin/bash
# 枚举各库中 bl g_once_init_enter* 的调用点，并反推其 once-location 数据地址
OBJ=/root/ohos-native/llvm/bin/llvm-objdump
L=/mnt/e/csgo/hap/entry/libs/arm64-v8a
OUT=/mnt/e/csgo/night-logs/once-map.txt
: > "$OUT"
for lib in libglib-2.0.so.0.8000.4 libgobject-2.0.so.0.8000.4 libgio-2.0.so.0.8000.4 \
           libpango-1.0.so.0.5400.0 libpangoft2-1.0.so.0.5400.0 libharfbuzz.so.0.60850.0 \
           libfontconfig.so.1 libfribidi.so.0.0.0; do
  f="$L/$lib"
  [ -f "$f" ] || continue
  echo "=== $lib ===" >> "$OUT"
  d=/tmp/dis-$$.txt
  $OBJ -d "$f" > "$d" 2>/dev/null
  python3 - "$lib" "$d" >> "$OUT" <<'PY'
import sys, re
lib, disf = sys.argv[1], sys.argv[2]
lines = open(disf, errors='replace').read().splitlines()
cur_func = '?'
insns = []
for ln in lines:
    s = ln.strip()
    m = re.match(r'^([0-9a-f]{6,16}) <(.+)>:$', s)
    if m:
        cur_func = m.group(2)
        continue
    m = re.match(r'^([0-9a-f]+):', s)
    if not m:
        continue
    addr = int(m.group(1), 16)
    toks = s[m.end():].split()
    if len(toks) >= 2 and re.fullmatch(r'[0-9a-f]+', toks[0]):
        asm = ' '.join(toks[1:])
        insns.append((addr, asm, cur_func))
found = 0
for i, (addr, asm, func) in enumerate(insns):
    parts = asm.split()
    if parts and parts[0] == 'bl' and 'g_once_init_enter' in asm:
        tm = re.search(r'<([^>]+)>', asm)
        tname = tm.group(1) if tm else '?'
        loc = None
        for j in range(max(0, i - 14), i):
            a2, asm2, _ = insns[j]
            # clang 常用 adr xN,#imm 直接装载 location（imm 为十进制字节偏移）
            m4 = re.match(r'adr\s+(x\d+),\s+#(\d+)', asm2)
            if m4:
                loc = a2 + int(m4.group(2))
                break
            m4b = re.match(r'adr\s+(x\d+),\s+0x([0-9a-f]+)', asm2)
            if m4b:
                loc = int(m4b.group(2), 16)
                break
            m2 = re.match(r'adrp\s+(x\d+),\s+0x([0-9a-f]+)', asm2)
            if not m2:
                continue
            reg, page = m2.group(1), int(m2.group(2), 16)
            for k in range(j + 1, i + 1):
                m3 = re.match(r'add\s+(x\d+),\s+(x\d+),\s+#0x([0-9a-f]+)', insns[k][1])
                if m3 and m3.group(1) == reg and m3.group(2) == reg:
                    loc = page + int(m3.group(3), 16)
                    break
                m3b = re.match(r'ldr\s+(x\d+),\s+\[(x\d+),\s+#0x([0-9a-f]+)\]', insns[k][1])
                if m3b and m3b.group(2) == reg:
                    loc = page + int(m3b.group(3), 16)
                    break
            if loc:
                break
        found += 1
        if loc:
            print('  call@0x%08x in %-50s -> once-loc=0x%08x (%s)' % (addr, func[:50], loc, tname))
        else:
            print('  call@0x%08x in %-50s -> once-loc=? (%s)' % (addr, func[:50], tname))
if found == 0:
    print('  (no g_once_init_enter calls found)')
PY
  rm -f "$d"
done
echo "== total lines: $(wc -l < "$OUT")"
