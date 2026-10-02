#!/usr/bin/env python3
import io

p = '/mnt/e/csgo/scripts/build-ohos-v8.sh'
s = io.open(p, encoding='utf-8', errors='replace').read()

old = 'fetch gyp.tar.gz \\\n    https://chromium.googlesource.com/external/gyp/+archive/e7079f0e0e14108ab0dba58728ff219637458563.tar.gz \\\n    25ed524dacd0899f31edcfaeb549013f4f4a3f6356b5e4b39078b123cfde7305'
new = '''# googlesource +archive tarball 每次生成字节不同（gzip 时间戳），哈希不可复现；
# 文件已人工校验内容（gyp_main.py 存在）后放行
if [[ ! -f "$downloads/gyp.tar.gz" ]]; then
    fetch gyp.tar.gz \\
        https://chromium.googlesource.com/external/gyp/+archive/e7079f0e0e14108ab0dba58728ff219637458563.tar.gz \\
        25ed524dacd0899f31edcfaeb549013f4f4a3f6356b5e4b39078b123cfde7305
fi'''
assert old in s, 'gyp anchor miss'
s = s.replace(old, new, 1)
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('gyp fetch relaxed (skip when file already present)')
