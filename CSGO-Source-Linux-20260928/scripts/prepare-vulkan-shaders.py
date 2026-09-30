#!/usr/bin/env python3
"""Stage native shaders and adapt Panorama's existing offline HLSL contracts."""
import itertools
import json
from pathlib import Path
import re
import sys

ROOT=Path(__file__).resolve().parent.parent
SOURCE=ROOT/'src/materialsystem/shaderapivulkan/shaders'
LEGACY=ROOT/'src/materialsystem/stdshaders'


def write(path,content):
    if not path.exists() or path.read_text()!=content:
        path.write_text(content)


def main():
    output=Path(sys.argv[1])
    output.mkdir(parents=True,exist_ok=True)
    for path in SOURCE.glob('*.hlsl'):
        write(output/path.name,path.read_text())
    manifest=json.loads((SOURCE/'manifest.json').read_text())
    manifest['shaders'].append(dict(name='native_shadow_vs',source='shadow.hlsl',stage='vertex',entry='ShadowVS',defines={'SHADOW_BUILD':0}))
    for blobby,water in itertools.product(range(2),repeat=2):
        manifest['shaders'].append(dict(name=f'native_shadow_ps_{blobby}_{water}',source_name='native_shadow_ps',
            source='shadow.hlsl',stage='fragment',entry='ShadowPS',static_index=blobby,dynamic_index=water,
            defines={'SHADOW_BUILD':0,'SHADOW_BLOBBY':blobby,'SHADOW_WATER_FOG':water}))
    for skin in range(2):
        manifest['shaders'].append(dict(name=f'native_shadowbuild_vs_{skin}',source_name='native_shadowbuild_vs',
            source='shadow.hlsl',stage='vertex',entry='BuildVS',dynamic_index=skin,defines={'SHADOW_BUILD':1,'SHADOW_SKIN':skin}))
    manifest['shaders'].append(dict(name='native_shadowbuild_ps',source='shadow.hlsl',stage='fragment',entry='BuildPS',defines={'SHADOW_BUILD':1}))
    for color,skin in itertools.product(range(2),repeat=2):
        manifest['shaders'].append(dict(name=f'native_twotexture_vs_{color}_{skin}',source_name='native_twotexture_vs',
            source='twotexture.hlsl',stage='vertex',entry='VSMain',static_index=color,dynamic_index=skin,
            defines={'DUAL_COLOR':color,'DUAL_SKIN':skin}))
    for effect,translucent in itertools.product(range(3),range(2)):
        manifest['shaders'].append(dict(name=f'native_twotexture_ps_{effect}_{translucent}',source_name='native_twotexture_ps',
            source='twotexture.hlsl',stage='fragment',entry='PSMain',static_index=effect*2+translucent,
            defines={'DUAL_EFFECT':effect,'DUAL_TRANSLUCENT':translucent},alpha_test=True))
    for stage,entry in [('vertex','ClearVS'),('fragment','ClearPS')]:
        manifest['shaders'].append(dict(name='native_clear_'+('vs' if stage=='vertex' else 'ps'),source='auxiliary.hlsl',stage=stage,entry=entry,defines={}))
    for variant in range(4):
        manifest['shaders'].append(dict(name='native_general_vs_'+str(variant),source_name='native_general_vs',
            source='auxiliary.hlsl',stage='vertex',entry='GeneralVS',static_index=variant,
            defines={'GENERAL_COLOR':variant&1,'GENERAL_TRANSFORM':variant>>1}))
    for effect in range(8):
        manifest['shaders'].append(dict(name='native_general_ps_'+str(effect),source_name='native_general_ps',
            source='auxiliary.hlsl',stage='fragment',entry='GeneralPS',static_index=effect,defines={'GENERAL_EFFECT':effect},alpha_test=True))
    manifest['shaders'].append(dict(name='native_screen_vs',source='screen.hlsl',stage='vertex',entry='VSMain',defines={}))
    for effect in range(5):
        manifest['shaders'].append(dict(name='native_screen_ps_'+str(effect),source_name='native_screen_ps',
            source='screen.hlsl',stage='fragment',entry='PSMain',static_index=effect,defines={'NATIVE_SCREEN_EFFECT':effect}))
    for shader in list(manifest['shaders']):
        if shader.get('source_name')=='native_model_vs':
            baked=dict(shader, name=shader['name']+'_baked', dynamic_index=shader['dynamic_index']+2,
                       defines=dict(shader['defines'],NATIVE_STATIC_LIGHT=1))
            manifest['shaders'].append(baked)
    for fancy in [False,True]:
        prefix='panoramafancy' if fancy else 'panorama'
        for stage in ['vertex','fragment']:
            name=prefix+('_vs30' if stage=='vertex' else '_ps30')
            original=(LEGACY/(name+'.fxc')).read_text()
            text=re.sub(r'#include "common_(?:vs|ps)_fxc.h"','#include "source_api.hlsl"',original)
            if stage=='vertex':
                head,tail=text.split('struct VS_OUTPUT',1)
                head=re.sub(r'float4 vPosition : POSITION;',r'[[vk::location(0)]] float3 vPosition : POSITION;',head)
                head=re.sub(r'(float4\s+\w+\s*:\s*TEXCOORD)(\d+)',lambda m:'[[vk::location('+str(int(m[2])+1)+')]] '+m[0],head)
                tail=tail.replace('vPosition : POSITION','vPosition : SV_Position')
                tail=re.sub(r'(float4\s+\w+\s*:\s*TEXCOORD)(\d+)',lambda m:'[[vk::location('+m[2]+')]] '+m[0],tail)
                tail=tail.replace('o.vPosition = i.vPosition;','o.vPosition = float4(i.vPosition,1.0);')
                text=head+'struct VS_OUTPUT'+tail
            else:
                text=re.sub(r'float([234]?)\s+(\w+)\s*:\s*register\s*\(\s*c(\d+)\s*\)\s*;',
                    lambda m:'#define '+m[2]+' sourcePixelC['+m[3]+']'+('.'+{ '':'x','2':'xy','3':'xyz','4':'xyzw'}[m[1]]),text)
                text=re.sub(r'sampler\s+(\w+)\s*:\s*register\s*\(\s*s(\d+)\s*\)\s*;',
                    lambda m:'SOURCE_PIXEL_TEXTURE('+m[1]+','+m[1]+'_sampler,'+m[2]+')',text)
                text=text.replace('#define Tex2D tex2D','')
                text=re.sub(r'Tex2D\(\s*(g_tTexture\d)\s*,',lambda m:m[1]+'.Sample('+m[1]+'_sampler,',text)
                text=re.sub(r'(float4\s+\w+\s*:\s*TEXCOORD)(\d+)',lambda m:'[[vk::location('+m[2]+')]] '+m[0],text)
                text=text.replace('float4_color_return_type','float4')
                text=re.sub(r'\)\s*:\s*COLOR',') : SV_Target0',text)
                text=re.sub(r'FinalOutput\(\s*o.vColor,\s*0,\s*PIXEL_FOG_TYPE_NONE,\s*TONEMAP_SCALE_NONE\s*\)','o.vColor',text)
                text='float3 SrgbGammaToLinear(float3 c) { return lerp(c/12.92,pow(max(0,(c+0.055)/1.055),2.4),step(0.04045,c)); }\n'+text
            filename=name+'.hlsl'
            write(output/filename,'// Adapted from '+str((LEGACY/(name+'.fxc')).relative_to(ROOT))+'\n'+text)
            combos=re.findall(r'// DYNAMIC: "(\w+)" "(\d+)\.\.(\d+)"',original)
            for values in itertools.product(*(range(int(lo),int(hi)+1) for _,lo,hi in combos)):
                defines={combo[0]:value for combo,value in zip(combos,values)}
                if defines.get('D_USEOUTERCORNER') and defines.get('D_USEINNERCORNER'):
                    continue
                index=0;scale=1
                for (_,lo,hi),value in zip(combos,values):
                    index+=(value-int(lo))*scale;scale*=int(hi)-int(lo)+1
                manifest['shaders'].append(dict(name=name+'_combo'+str(index),source_name=name,source=filename,
                    stage=stage,entry='main',defines=defines,static_index=0,dynamic_index=index))
    write(output/'manifest.json',json.dumps(manifest,indent=2)+'\n')


if __name__=='__main__':
    main()
