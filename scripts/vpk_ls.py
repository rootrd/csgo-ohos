#!/usr/bin/env python3
"""Minimal VPK dir-file lister/extractor (v1/v2)."""
import struct, sys, os, io

def read_cstr(f):
    bs = bytearray()
    while True:
        c = f.read(1)
        if not c or c == b'\x00':
            break
        bs += c
    return bs.decode('utf-8', 'replace')

def main():
    path = sys.argv[1]
    pattern = sys.argv[2].lower() if len(sys.argv) > 2 else None
    outdir = sys.argv[3] if len(sys.argv) > 3 else None
    f = open(path, 'rb')
    sig, ver, tree_size = struct.unpack('<III', f.read(12))
    assert sig == 0x55aa1234, 'not a vpk: %x' % sig
    if ver == 2:
        f.read(8)  # archive md5 section sizes etc: skip header extras
        # v2 header: sig,ver,treeSize,fileDataSectionSize,archiveMD5SectionSize,otherMD5SectionSize,signatureSize
        f.seek(0)
        hdr = struct.unpack('<IIIIIII', f.read(28))
        tree_size = hdr[2]
        f.seek(28)
    entries = []
    while True:
        ext = read_cstr(f)
        if not ext:
            break
        while True:
            path_ = read_cstr(f)
            if not path_:
                break
            while True:
                name = read_cstr(f)
                if not name:
                    break
                crc, pre_bytes, archive_idx = struct.unpack('<IHH', f.read(8))
                entry_offset, entry_length = struct.unpack('<II', f.read(8))
                terminator = f.read(2)
                pre = f.read(pre_bytes) if pre_bytes else b''
                full = ('%s/%s.%s' % (path_, name, ext)) if ext != ' ' else ('%s/%s' % (path_, name))
                entries.append((full.lower(), crc, archive_idx, pre))
    n = 0
    for full, crc, aidx, pre in entries:
        if pattern and pattern not in full:
            continue
        print(full)
        n += 1
    print('total matches: %d / all entries: %d' % (n, len(entries)))

    if outdir and pattern:
        # need archive files to extract data; dir vpk only has preload bytes
        for full, crc, aidx, pre in entries:
            if pattern not in full:
                continue
            dst = os.path.join(outdir, full.replace('\\', '/'))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            io.open(dst, 'wb').write(pre)
            print('wrote preload %s (%d bytes)' % (dst, len(pre)))

main()
