#!/usr/bin/env python3
"""host_cmd.cpp 断裂字符串修复（WSL 内执行）：enter 后的真实换行 -> \\n 转义"""
import io

p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/engine/host_cmd.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()

broken = 'Host_Map_Helper enter\n" );'
fixed = 'Host_Map_Helper enter\\n" );'

count = s.count(broken)
s = s.replace(broken, fixed)

broken2 = 'Map_IsValid OK %s\n", ppath );'
fixed2 = 'Map_IsValid OK %s\\n", ppath );'
count2 = s.count(broken2)
s = s.replace(broken2, fixed2)

# sv_main.cpp 同类断裂
p_sv = '/root/csgo-src/CSGO-Source-Linux-20260928/src/engine/sv_main.cpp'
s_sv = io.open(p_sv, encoding='utf-8', errors='replace').read()
broken_sv = 'SpawnServer enter %s\n", mapname );'
fixed_sv = 'SpawnServer enter %s\\n", mapname );'
count_sv = s_sv.count(broken_sv)
s_sv = s_sv.replace(broken_sv, fixed_sv)
io.open(p_sv, 'w', encoding='utf-8', newline='').write(s_sv)

io.open(p, 'w', encoding='utf-8', newline='').write(s)

s2 = io.open(p, encoding='utf-8', errors='replace').read()
i = s2.find('Map_IsValid OK')
print('replaced:', count, count2, 'sv:', count_sv, '| now:', repr(s2[i:i+45]))
