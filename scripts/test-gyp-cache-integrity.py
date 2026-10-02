#!/usr/bin/env python3
"""Exercise the actual gyp cache block with a verified official archive.
Usage: python3 scripts/test-gyp-cache-integrity.py path/to/pinned-gyp.tar[.gz]
This creates only temporary build/cache directories and never runs gyp.
"""
import gzip
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'scripts/build-ohos-v8.sh').read_text()
a=source.index('gyp_revision=')
b=source.index('fetch_text "$source_dir/base/trace_event',a)
block=source[a:b]
revision=re.search(r'^gyp_revision=(\w+)$',block,re.M).group(1)
digest=re.search(r'^gyp_tar_sha256=(\w+)$',block,re.M).group(1)
archive=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT/f'CSGO-Source-Linux-20260928/runtime/ohos/v8-build/downloads/gyp-{revision}.tar'
blob=archive.read_bytes()
if archive.suffix=='.gz': blob=gzip.decompress(blob)
assert hashlib.sha256(blob).hexdigest()==digest, 'Input must be the pinned official Git archive'
with tempfile.TemporaryDirectory(prefix='gyp-cache-integrity-') as d:
    root=Path(d); downloads=root/'downloads'; downloads.mkdir()
    cached=downloads/f'gyp-{revision}.tar'; cached.write_bytes(blob)
    src=root/'v8'; old=src/'tools/gyp'; old.mkdir(parents=True)
    (old/'gyp_main.py').write_text('unverified old source')
    (old/'unexpected.py').write_text('unverified cache content')
    script=root/'test.sh'
    script.write_text('set -euo pipefail\n'+
        'build_root=$1\ndownloads=$1/downloads\nsource_dir=$1/v8\n'+
        'verify() { printf "%s  %s\\n" "$2" "$1" | sha256sum -c -; }\n'+block)
    subprocess.run(['bash',str(script),d],check=True)
    assert (old/'gyp_main.py').read_text()!='unverified old source'
    assert not (old/'unexpected.py').exists()
    backups=list(root.glob('gyp-previous.*/tree/unexpected.py'))
    assert len(backups)==1 and backups[0].read_text()=='unverified cache content'
    sentinel=old/'sentinel-before-failed-verify'; sentinel.write_text('unchanged')
    cached.write_bytes(blob+b'corruption')
    result=subprocess.run(['bash',str(script),d],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    assert result.returncode!=0, 'Corrupt cached archive must fail closed'
    assert sentinel.read_text()=='unchanged', 'No extraction/source replacement after checksum failure'
print('PASS: actual gyp cache block verifies every hit, replaces stale sources, preserves backups, rejects corruption before extraction')
