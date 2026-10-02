#!/usr/bin/env python3
"""修复 engine_startup.cpp 注入段的断裂字符串与残留行"""
import io

p = '/mnt/e/csgo/CSGO-Source-Linux-20260928/android/native/engine_startup.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()

broken1 = 'fprintf(ef, "fopen FAILED errno=%d path=%s\n", errno, cmdFilePath.c_str()); fclose(ef);'
fixed1 = 'fprintf(ef, "fopen FAILED errno=%d path=%s\\n", errno, cmdFilePath.c_str()); fclose(ef);'
broken2 = 'fprintf(ef, "read n=%zu content=%s\n", n, cmdBuf); fclose(ef);\n                            n ? cmdBuf : "(empty)");'
fixed2 = 'fprintf(ef, "read n=%zu content=%s\\n", n, cmdBuf); fclose(ef);'

n1 = s.count(broken1)
s = s.replace(broken1, fixed1)
n2 = s.count(broken2)
s = s.replace(broken2, fixed2)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('fixed', n1, n2)
