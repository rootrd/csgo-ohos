#!/usr/bin/env python3
"""Compare original VTF pixels with decoded ASTC at the same mip and dimensions.
Run inside Distrobox dev. Pillow independently verifies the BC source decoding.
"""
import argparse
import base64
import html
import json
import math
from pathlib import Path
import struct
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, str(Path(__file__).parent / 'tests'))
from support import decode_astc, decode_dds, ktx_info, run, validate_ktx

SAMPLES = [
    ('dust-brick', 'Dust II brick / BC1', 'color', 'materials/de_dust/hr_dust/hr_dust_brick_01_color.vtf'),
    ('dust-ground', 'Dust II ground / BC1', 'color', 'materials/de_dust/hr_dust/hr_dust_brick_ground_02_color.vtf'),
    ('sticker', 'Buster signature / BC3 alpha', 'alpha', 'materials/models/weapons/customization/stickers/katowice2019/sig_buster.vtf'),
    ('foliage', 'Palm foliage / BC3 alpha', 'alpha', 'materials/models/props/de_dust/hr_dust/foliage/palm_treecard_01.vtf'),
    ('normal-rgb', 'Mudbrick normal / BGR888', 'normal', 'materials/de_dust/hr_dust/hr_dust_mudbrick_01_normal.vtf'),
    ('normal-bc', 'Brick normal / BC1', 'normal', 'materials/de_dust/hr_dust/hr_dust_brick_01_normal.vtf'),
    ('ui', 'Dust II menu thumbnail / BC3', 'ui', 'materials/vgui/maps/menu_thumb_de_dust2.vtf'),
    ('metal', 'Pickem crystal metal / BC3', 'data', 'materials/models/inventory_items/katowice_pickem_2019/crystal_metal.vtf'),
]


def reference_pixels(raw):
    minor, header_size = struct.unpack_from('<II', raw, 8)
    width, height, flags, frames = struct.unpack_from('<HHIH', raw, 16)
    fmt = struct.unpack_from('<I', raw, 52)[0]
    mips = raw[56]
    assert frames == 1 and not flags & 0x4000
    assert minor < 2 or struct.unpack_from('<H', raw, 63)[0] == 1
    if minor >= 3 and struct.unpack_from('<I', raw, 68)[0]:
        resources = [struct.unpack_from('<II', raw, 80 + i * 8) for i in range(struct.unpack_from('<I', raw, 68)[0])]
        offset = next(value for tag, value in resources if tag == 48)
    else:
        low_format, low_w, low_h = struct.unpack_from('<iBB', raw, 57)
        assert low_format in (-1,13)
        offset = header_size + (math.ceil(low_w/4)*math.ceil(low_h/4)*8 if low_format == 13 else 0)
    def size(m):
        w,h=max(1,width>>m),max(1,height>>m)
        return math.ceil(w/4)*math.ceil(h/4)*(8 if fmt==13 else 16) if fmt in (13,14,15) else w*h*{0:4,3:3,12:4}[fmt]
    offset += sum(size(m) for m in range(1,mips))
    payload=raw[offset:offset+size(0)]
    if fmt in (13,14,15):
        return decode_dds(payload,width,height,{13:b'DXT1',14:b'DXT3',15:b'DXT5'}[fmt])
    pixels=np.frombuffer(payload,dtype=np.uint8).reshape(height,width,-1)
    if fmt==3:
        return np.concatenate((pixels[:,:,::-1],np.full((height,width,1),255,dtype=np.uint8)),axis=2)
    return pixels[:,:,[2,1,0,3]] if fmt==12 else pixels


def psnr(a,b):
    mse=float(np.mean((a.astype(np.float64)-b.astype(np.float64))**2))
    return None if mse==0 else 10*math.log10(255**2/mse)


def ssim8(a,b):
    # Mean of non-overlapping 8x8 luminance SSIM windows, specified to make results reproducible.
    a=a[:,:,:3].astype(float)@np.array([.2126,.7152,.0722]);b=b[:,:,:3].astype(float)@np.array([.2126,.7152,.0722])
    h,w=(a.shape[0]//8)*8,(a.shape[1]//8)*8
    a=a[:h,:w].reshape(h//8,8,w//8,8).transpose(0,2,1,3).reshape(-1,64)
    b=b[:h,:w].reshape(h//8,8,w//8,8).transpose(0,2,1,3).reshape(-1,64)
    ma,mb=a.mean(1),b.mean(1);va,vb=a.var(1),b.var(1);cov=((a-ma[:,None])*(b-mb[:,None])).mean(1)
    return float(np.mean(((2*ma*mb+2.55**2)*(2*cov+7.65**2))/((ma**2+mb**2+2.55**2)*(va+vb+7.65**2))))


def composite(a):
    h,w=a.shape[:2];y,x=np.indices((h,w));bg=np.where((x//16+y//16)%2,184,104)[:,:,None]
    alpha=a[:,:,3:4].astype(float)/255
    return np.rint(a[:,:,:3]*alpha+bg*(1-alpha)).clip(0,255).astype(np.uint8)


def metrics(source,converted,kind):
    out={'rgb_psnr_db':psnr(source[:,:,:3],converted[:,:,:3]),'alpha_psnr_db':psnr(source[:,:,3],converted[:,:,3]),
         'luma_ssim_8x8':ssim8(source,converted)}
    if kind=='alpha':out['checkerboard_psnr_db']=psnr(composite(source),composite(converted))
    if kind=='normal':
        a=source[:,:,:3].astype(float)/127.5-1;b=converted[:,:,:3].astype(float)/127.5-1
        la=np.linalg.norm(a,axis=2);lb=np.linalg.norm(b,axis=2);mask=(la>.1)&(lb>.1)
        cosine=np.sum(a*b,axis=2)[mask]/(la[mask]*lb[mask]);angle=np.degrees(np.arccos(np.clip(cosine,-1,1)))
        out.update(normal_mean_degrees=float(angle.mean()),normal_p95_degrees=float(np.percentile(angle,95)))
    return out


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--vpk',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    binary=args.build/'vtf2astc';cli=args.build/'astcenc-build/Source/astcenc-native'
    validator=args.build.parent/'tools/KTX-Software-4.4.2-Linux-x86_64/bin/ktx'
    rows=[];images={}
    for ident,label,kind,path in SAMPLES:
        print(f'Quality sample: {path}',flush=True)
        extracted=args.out/'source'
        run(binary,'--vpk',args.vpk,'--mode','extract','--filter',path,'--out',extracted,'--threads',1)
        raw=(extracted/path).read_bytes();reference=reference_pixels(raw)
        row={'id':ident,'label':label,'kind':kind,'path':path,'width':reference.shape[1],'height':reference.shape[0],'blocks':{}}
        for block in (4,6):
            folder=args.out/f'{ident}-{block}'
            run(binary,'--input',extracted/path,'--out',folder,'--block',f'{block}x{block}','--quality','medium','--dump-rgba','--threads',1)
            ktx=folder/Path(path).with_suffix('.ktx2').name
            validate_ktx(validator,ktx)
            original=np.frombuffer(Path(str(ktx)+'.rgba').read_bytes(),dtype=np.uint8).reshape(reference.shape)
            difference=np.abs(original.astype(int)-reference.astype(int))
            # Independent BC decoders may round interpolated values one step differently.
            assert difference.max()<=1,(path,int(difference.max()))
            converted=decode_astc(cli,ktx,args.out/f'{ident}-{block}.png')
            row['blocks'][str(block)]=metrics(original,converted,kind)
            row['blocks'][str(block)]['payload_bytes']=sum(level[1] for level in ktx_info(ktx)['levels'])
            row['source_decoder_max_difference']=int(difference.max())
            images[(ident,str(block))]=composite(converted) if kind=='alpha' else converted[:,:,:3]
        images[(ident,'original')]=composite(original) if kind=='alpha' else original[:,:,:3]
        Image.fromarray(original).save(args.out/f'{ident}-original.png')
        for variant in ('original','4','6'):
            Image.fromarray(images[(ident,variant)]).save(args.out/f'{ident}-{variant}-display.png')
        rows.append(row)
    report={'baseline':'Decoded original VTF, mip 0, frame 0, face 0. Additional ASTC loss only; not uncompressed authoring textures.',
            'profile':'LDR, preserve RGBA, medium, astcenc 5.7.0',
            'ssim_definition':'Mean non-overlapping 8x8 luminance SSIM windows (BT.709 weights)',
            'normal_definition':'Angular error assuming RGB encodes XYZ, vectors renormalized before measurement',
            'samples':rows}
    (args.out/'quality.json').write_text(json.dumps(report,indent=2)+'\n')
    font=ImageFont.load_default(size=17)
    for crop in (False,True):
        columns=3;width=320;row_height=358
        canvas=Image.new('RGB',(columns*width,len(rows)*row_height),(30,32,38));draw=ImageDraw.Draw(canvas)
        for index,row in enumerate(rows):
            for column,variant in enumerate(('original','4','6')):
                array=images[(row['id'],variant)];im=Image.fromarray(array)
                if crop:
                    w,h=im.size;size=min(160,w,h);im=im.crop(((w-size)//2,(h-size)//2,(w+size)//2,(h+size)//2))
                    im=im.resize((320,320),Image.Resampling.NEAREST)
                else:im=im.resize((320,320),Image.Resampling.LANCZOS)
                y=index*row_height
                canvas.paste(im,(column*width,y+38))
                title=row['label'] if variant=='original' else f'ASTC {variant}x{variant} | RGB {row["blocks"][variant]["rgb_psnr_db"]:.1f} dB'
                draw.text((column*width+6,y+8),title,fill=(240,240,240),font=font)
        canvas.save(args.out/('crops.png' if crop else 'comparison.png'))
    # Standalone comparison: all images are embedded, so file:// needs no server.
    embedded={}
    for row in rows:
        embedded[row['id']]={v:'data:image/png;base64,'+base64.b64encode((args.out/f'{row["id"]}-{v}-display.png').read_bytes()).decode() for v in ('original','4','6')}
    options=''.join(f'<option value="{r["id"]}">{html.escape(r["label"])}</option>' for r in rows)
    page='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>VTF / ASTC 画质对比</title>
<style>body{font:16px system-ui;background:#161a20;color:#eee;max-width:1100px;margin:30px auto;padding:16px}select,input{margin:10px}#view{position:relative;width:100%;aspect-ratio:1;overflow:hidden;background:#333}#view img{position:absolute;width:100%;height:100%;object-fit:contain}#top{clip-path:inset(0 50% 0 0)}.note{color:#bdc6d2}pre{white-space:pre-wrap}</style>
<h1>原 VTF 与 ASTC 画质对比</h1><p>左侧为原始 VTF 解码，右侧为 ASTC 再解码。拖动滑块查看差异；透明图使用相同棋盘背景。</p>
<select id="sample">OPTIONS</select><select id="block"><option value="6">ASTC 6×6</option><option value="4">ASTC 4×4</option></select>
<label>分割位置<input id="split" type="range" min="0" max="100" value="50"></label>
<div id="view"><img id="bottom"><img id="top"></div><pre id="stats"></pre>
<p class="note">仅比较 mip 0 的额外压缩损失，源 VTF 本身可能已经有 DXT 损失。这里不代表游戏内渲染或真机验收。RGB XYZ 法线角度统计仅适用于此处选取的法线样本。</p>
<script>const images=IMAGES;const rows=ROWS;const sample=document.querySelector('#sample');const block=document.querySelector('#block');const split=document.querySelector('#split');const topImage=document.querySelector('#top');function update(){const r=rows.find(x=>x.id===sample.value);topImage.src=images[r.id].original;document.querySelector('#bottom').src=images[r.id][block.value];document.querySelector('#view').style.aspectRatio=r.width+'/'+r.height;topImage.style.clipPath='inset(0 '+(100-split.value)+'% 0 0)';document.querySelector('#stats').textContent=r.path+'\\n'+JSON.stringify(r.blocks[block.value],null,2)}sample.onchange=block.onchange=split.oninput=update;update();</script></html>'''
    page=page.replace('OPTIONS',options).replace('IMAGES',json.dumps(embedded)).replace('ROWS',json.dumps(rows))
    (args.out/'comparison.html').write_text(page)
    print(json.dumps(report,indent=2),flush=True)


if __name__=='__main__':main()
