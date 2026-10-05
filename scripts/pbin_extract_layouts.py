#!/usr/bin/env python3
"""Extract all panorama layout XMLs from the ORIGINAL game pbin (pbin_dump.bin)."""
import zipfile, io, os

data = open('pbin_dump.bin', 'rb').read()
idx = data.find(b'PK\x03\x04')
zf = zipfile.ZipFile(io.BytesIO(data[idx:]))
os.makedirs('night-logs/pbin-real', exist_ok=True)
n_out = 0
for n in zf.namelist():
    ln = n.lower().replace('\\', '/')
    if ln.startswith('panorama/layout/') and ln.endswith('.xml'):
        dst = 'night-logs/pbin-real/' + ln.replace('/', '_')
        with open(dst, 'wb') as f:
            f.write(zf.read(n))
        n_out += 1
print('extracted', n_out, 'layout xmls to night-logs/pbin-real')
