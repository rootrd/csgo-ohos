#!/usr/bin/env python3
"""Validate every KTX2, hash and subresource in a finished conversion manifest.
Run inside Distrobox dev; custom source.vtf metadata is the sole allowed Khronos warning.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import time

sys.path.insert(0,str(Path(__file__).parent/'tests'))
from support import ktx_info, validate_ktx


def digest(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()


def verify(root, validator, record, recipe):
    path=root/record['output']
    assert not Path(record['output']).is_absolute() and '..' not in Path(record['output']).parts
    assert record['status']=='converted' and record['recipe']==recipe
    assert json.loads(Path(str(path)+'.json').read_text())==record,'sidecar differs from manifest'
    assert path.stat().st_size==record['output_bytes'],'file size differs'
    assert digest(path)==record['output_sha256'],'SHA-256 differs'
    info=ktx_info(path);source=record['source']
    assert info['format']==record['vk_format']
    assert (info['width'],info['height'],info['depth'])==(source['width'],source['height'],source['depth'] if source['depth']>1 else 0)
    assert (info['mips'],info['faces'],info['layers'])==(source['mips'],source['faces'],source['frames'] if source['frames']>1 else 0)
    assert info['supercompression']==0
    if record['encoding']=='astc-ldr':
        assert source['depth']==1
        assert info['format']=={4:157,6:165,8:171}[record['block']]+(record['sampling']=='srgb')
        assert info['type_size']==1
    else:
        assert info['format'] in (17,37,38,91,97,100,103,106,109,83)
    total=0;subresources=0
    for mip,(offset,length,uncompressed) in enumerate(info['levels']):
        w,h,d=(max(1,source[k]>>mip) for k in ('width','height','depth'))
        if record['encoding']=='astc-ldr':
            block=record['block'];image_size=math.ceil(w/block)*math.ceil(h/block)*16
        else:
            bpp={17:2,37:4,38:4,91:8,97:8,100:4,103:8,106:12,109:16,83:4}[info['format']]
            image_size=w*h*bpp
        count=d*source['frames']*source['faces'];expected=image_size*count
        assert length==uncompressed==expected,(mip,length,expected)
        assert offset+length<=len(info['data'])
        total+=length;subresources+=count
    # libktx's in-memory dataSize includes inter-mip alignment. A final 1x1 RG8
    # level has two data bytes plus two padding bytes before the next mip.
    span=max(offset+length for offset,length,_ in info['levels'])-min(offset for offset,_,_ in info['levels'])
    assert span==record['payload_bytes'] and total<=span
    metadata=info['metadata']
    assert metadata['source.vtf.path'].rstrip(b'\0').decode()==record['path']
    assert metadata['source.vtf.sha256'].rstrip(b'\0').decode()==record['source_sha256']
    assert json.loads(metadata['source.vtf.info'].rstrip(b'\0'))==source
    assert json.loads(metadata['source.vtf.recipe'].rstrip(b'\0'))==recipe
    header=metadata['source.vtf.header']
    assert header[:4]==b'VTF\0' and struct.unpack_from('<I',header,52)[0]==source['format']
    assert struct.unpack_from('<HH',header,16)==(source['width'],source['height'])
    report=validate_ktx(validator,path)
    return subresources,len(report['messages'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack',required=True,type=Path)
    parser.add_argument('--ktx',required=True,type=Path)
    parser.add_argument('--inventory',type=Path)
    parser.add_argument('--threads',type=int,default=4)
    args=parser.parse_args();started=time.monotonic()
    assert args.threads>0
    root=args.pack;manifest=json.loads((root/'manifest.json').read_text())
    assert manifest['complete'] and manifest['failed']==0 and manifest['mode']=='convert','conversion did not complete'
    records=manifest['textures'];assert len(records)==manifest['selected']
    assert len({r['path'] for r in records})==len(records)
    expected={r['output'] for r in records};actual={str(p.relative_to(root)) for p in root.rglob('*.ktx2')}
    assert expected==actual,{'missing':sorted(expected-actual),'unexpected':sorted(actual-expected)}
    if args.inventory:
        inventory=json.loads(args.inventory.read_text());assert inventory['complete'] and inventory['failed']==0
        original={r['path']:(r['source_sha256'],r['source_bytes'],r['source']) for r in inventory['textures']}
        converted={r['path']:(r['source_sha256'],r['source_bytes'],r['source']) for r in records}
        assert original==converted,'source coverage differs from full inventory'
    errors=[];subresources=0;warnings=0
    with ThreadPoolExecutor(max_workers=args.threads) as executor:
        pending={executor.submit(verify,root,args.ktx,record,manifest['recipe']):record for record in records}
        for completed,future in enumerate(as_completed(pending),1):
            try:
                count,warn=future.result();subresources+=count;warnings+=warn
            except Exception as error:
                errors.append({'path':pending[future]['path'],'error':str(error)})
            if completed%500==0 or completed==len(records):
                print(f'Validated {completed}/{len(records)}; errors={len(errors)}',flush=True)
    summary={'schema':1,'valid':not errors,'textures':len(records),'subresources':subresources,
        'source_bytes':manifest['source_bytes'],'ktx2_bytes':manifest['output_bytes'],
        'manifest_sha256':digest(root/'manifest.json'),'inventory_sha256':digest(args.inventory) if args.inventory else None,
        'validator':'Khronos KTX-Software 4.4.2','errors':errors,
        'allowed_warning':{'id':7010,'description':'Application-defined source.vtf metadata keys permitted by KTX2','count':warnings},
        'encodings':dict(Counter(r['encoding'] for r in records)),
        'astc_blocks':dict(Counter(str(r['block']) for r in records if r['encoding']=='astc-ldr')),
        'duration_seconds':round(time.monotonic()-started,3)}
    temporary=root/'validation.json.partial';temporary.write_text(json.dumps(summary,indent=2)+'\n');temporary.replace(root/'validation.json')
    if not errors:
        checksum_paths=['manifest.json','validation.json']
        checksum_paths+=sorted(expected)
        checksum_paths+=sorted(p+'.json' for p in expected)
        with (root/'SHA256SUMS.partial').open('w') as output:
            for path in checksum_paths:
                output.write(f'{digest(root/path)}  {path}\n')
        (root/'SHA256SUMS.partial').replace(root/'SHA256SUMS')
    print(json.dumps(summary,indent=2),flush=True)
    return 1 if errors else 0


if __name__=='__main__':raise SystemExit(main())
