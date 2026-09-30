#!/usr/bin/env python3
import io
p = '/root/csgo-src/CSGO-Source-Linux-20260928/android/native/android_main.cpp'
s = io.open(p, encoding='utf-8', errors='replace').read()
old = '''    auto LoadLocalFile = [](const std::string &path, size_t *outSize) -> void * {
        std::ifstream file(path, std::ios::binary);
        if (!file) return nullptr;'''
new = '''    auto LoadLocalFile = [](const std::string &path, size_t *outSize) -> void * {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            fprintf(stderr, "CSGO_TRACE: LoadLocalFile FAILED path=%s errno=%d\\n", path.c_str(), errno);
            return nullptr;
        }'''
assert old in s
s = s.replace(old, new, 1)
if '#include <cerrno>' not in s:
    s = s.replace('#include <cstdint>', '#include <cstdint>\n#include <cerrno>', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('LoadLocalFile failure trace added')
