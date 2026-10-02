#!/usr/bin/env python3
"""VPK 提取器 v2：正确处理 preload 数据 + 归档数据拼接"""
import struct
import sys
import os

dir_vpk = sys.argv[1]
outdir = sys.argv[2]
filter_sub = sys.argv[3].lower() if len(sys.argv) > 3 else None

data = open(dir_vpk, 'rb').read()
sig, ver, tree_size = struct.unpack_from('<III', data, 0)
assert sig == 0x55aa1234, hex(sig)
pos = 28 if ver == 2 else 12
end = pos + tree_size

def cstr(d, p):
    e = d.index(b'\0', p)
    return d[p:e].decode('utf-8', 'replace'), e + 1

base = os.path.dirname(dir_vpk)
archive_cache = {}
count = 0

def get_archive(idx):
    if idx not in archive_cache:
        name = os.path.basename(dir_vpk).replace('_dir.vpk', '')
        archive_cache[idx] = open(os.path.join(base, f'{name}_{idx:03d}.vpk'), 'rb')
    return archive_cache[idx]

while pos < end:
    ext, pos = cstr(data, pos)
    if not ext:
        break
    while True:
        path, pos = cstr(data, pos)
        if not path:
            break
        while True:
            name, pos = cstr(data, pos)
            if not name:
                break
            crc, pre, idx, off, ln, term = struct.unpack_from('<IHHIIH', data, pos)
            pos += 18 + pre
            full = (f'{path}/{name}.{ext}' if path else f'{name}.{ext}')
            low = full.lower()
            if filter_sub and filter_sub not in low:
                continue
            preload = data[pos - pre:pos] if pre else b''
            if idx == 0x7fff:
                content = data[off:off + ln]
            else:
                af = get_archive(idx)
                af.seek(off)
                content = af.read(ln)
            content = preload + content
            dest = os.path.join(outdir, full.replace('\\', '/'))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, 'wb') as out:
                out.write(content)
            count += 1

print('extracted', count, 'files to', outdir)
