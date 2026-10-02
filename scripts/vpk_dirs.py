#!/usr/bin/env python3
import struct
import sys

data = open(sys.argv[1], 'rb').read()
sig, ver, tree = struct.unpack_from('<III', data, 0)
print('sig', hex(sig), 'ver', ver, 'tree', tree)
pos = 28
end = 28 + tree

def cstr(d, p):
    e = d.index(b'\0', p)
    return d[p:e].decode('utf-8', 'replace'), e + 1

exts = []
paths = []
names = []
n = 0
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
            pre = struct.unpack_from('<H', data, pos + 4)[0]
            pos += 18 + pre
            n += 1
            exts.append(ext)
            paths.append(path)

print('total entries:', n)
from collections import Counter
c = Counter(paths)
for p, k in sorted(c.items()):
    print(p, k)
