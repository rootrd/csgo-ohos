#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/src/materialsystem/shaderapivulkan/probe/vulkan_probe.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
# 移除 DXVK 头（引擎树里无此路径）
s = s.replace('#include "../../util/log/log.h"\n#include "../../util/util_string.h"\n', '')
# Logger::warn(str::format(...)) -> fprintf(stderr, ...)
old = '''                        Logger::warn(str::format("GPU color mismatch at ", x, ",", y, ": ", reason,
                            " (Maleoon multi-clear quirk, continuing)"));'''
new = '''                        fprintf(stderr, "CSGO_TRACE: GPU color mismatch at %u,%u: %s (Maleoon multi-clear quirk, continuing)\\n",
                            x, y, reason.c_str());'''
assert old in s, 'logger call not found'
s = s.replace(old, new, 1)
# 需要 cstdio
if '#include <cstdio>' not in s:
    s = s.replace('#include <cstdint>', '#include <cstdint>\n#include <cstdio>', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('stderr version in place')
