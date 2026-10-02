#!/usr/bin/env python3
"""VPK v1 目录解析 + 抽取 hud 相关布局文件"""
import struct
import sys

DIR_VPK = sys.argv[1] if len(sys.argv) > 1 else '/tmp/vpkx/cstrike/cstrike_pak_dir.vpk'
OUT = sys.argv[2] if len(sys.argv) > 2 else '/tmp/vpkx/out'

with open(DIR_VPK, 'rb') as f:
    data = f.read()

sig, ver, tree_size = struct.unpack_from('<III', data, 0)
assert sig == 0x55aa1234, hex(sig)
print(f'VPK v{ver} tree_size={tree_size}')
pos = 12
if ver == 2:
    pos = 28  # file_data_section_size + archive_md5/other_md5/signature section sizes
end = 12 + tree_size

def cstr(d, p):
    e = d.index(b'\0', p)
    return d[p:e].decode('utf-8', 'replace'), e + 1

entries = {}
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
            crc, preload_bytes, archive_idx, entry_off, entry_len, term = struct.unpack_from('<IHHIIH', data, pos)
            pos += 18  # crc(4)+preload(2)+archive(2)+offset(4)+length(4)+terminator(2)
            preload = data[pos:pos + preload_bytes]
            pos += preload_bytes
            full = f'{path}.{ext}' if path else f'{name}.{ext}'
            entries[full.lower()] = (archive_idx, entry_off, entry_len, preload)

print(f'{len(entries)} files indexed')
import os
os.makedirs(OUT, exist_ok=True)
for full, (idx, off, length, preload) in entries.items():
    low = full.lower()
    if 'layout/hud' in low and (low.endswith('hud.xml') or 'base_hud' in low):
        print('MATCH:', full, idx, off, length)
        base = os.path.join(OUT, full.replace('\\', '/'))
        os.makedirs(os.path.dirname(base), exist_ok=True)
        with open(DIR_VPK.replace('_dir.vpk', f'_{idx:03d}.vpk'), 'rb') as af:
            af.seek(off)
            content = af.read(length)
        if preload:
            content = preload + content
        with open(base, 'wb') as out:
            out.write(content)
        print('  wrote', base, len(content))
