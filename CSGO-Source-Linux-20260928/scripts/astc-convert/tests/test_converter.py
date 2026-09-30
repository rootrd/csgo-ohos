#!/usr/bin/env python3
"""Run via scripts/build-astc-convert.sh test (inside Distrobox dev)."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
import numpy as np
from support import decode_astc, decode_dds, ktx_info, make_vtf, run, validate_ktx

PARSER = argparse.ArgumentParser()
PARSER.add_argument('--build', required=True, type=Path)
ARGS = PARSER.parse_args()
BUILD = ARGS.build.resolve()
BINARY = BUILD / 'vtf2astc'
CLI = BUILD / 'astcenc-build/Source/astcenc-native'
VALIDATOR = BUILD.parent / 'tools/KTX-Software-4.4.2-Linux-x86_64/bin/ktx'


class ConverterTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='vtf2astc-test-', dir=BUILD.parent)
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def convert(self, raw, name='fixture', *args, ok=True):
        source = self.root / f'{name}.vtf'
        source.write_bytes(raw)
        out = self.root / name
        result = run(BINARY, '--input', source, '--out', out, '--threads', 1, *args, ok=ok)
        output = out / f'{name}.ktx2'
        if ok:
            validate_ktx(VALIDATOR, output)
        return output, result

    def test_dxt3_dxt5_alpha_and_color_against_pillow(self):
        selectors = sum((i % 8) << (3 * i) for i in range(16))
        colors = struct.pack('<HHI', 0x001f, 0xf800, 0xaaaaaaaa)
        for fmt, fourcc, alpha in (
            (14, b'DXT3', bytes.fromhex('1032547698badcfe')),
            (15, b'DXT5', bytes((10, 200)) + selectors.to_bytes(6, 'little')),
            (15, b'DXT5', bytes((240, 20)) + selectors.to_bytes(6, 'little'))):
            with self.subTest(fmt=fmt, alpha=alpha.hex()):
                block = alpha + colors
                raw = make_vtf(fmt=fmt, payload=lambda *x: block, thumbnail=True)
                output, _ = self.convert(raw, f'alpha-{fmt}-{alpha[0]}', '--dump-rgba', '--block', '4x4')
                decoded = np.frombuffer(Path(str(output) + '.rgba').read_bytes(), dtype=np.uint8).reshape(4, 4, 4)
                reference = decode_dds(block, 4, 4, fourcc)
                self.assertLessEqual(np.abs(decoded.astype(int) - reference).max(), 1)
                self.assertEqual(int(decoded[:,:,3].min()), int(reference[:,:,3].min()))
                self.assertLess(int(decoded[:,:,3].min()), 255)

    def test_bc1_three_color_and_transparency(self):
        block = struct.pack('<HHI', 0x001f, 0xf800, 0xe4e4e4e4)
        output, _ = self.convert(make_vtf(fmt=13, payload=lambda *x: block), 'bc1', '--dump-rgba')
        decoded = np.frombuffer(Path(str(output) + '.rgba').read_bytes(), dtype=np.uint8).reshape(4,4,4)
        reference = decode_dds(block, 4, 4, b'DXT1')
        self.assertLessEqual(np.abs(decoded.astype(int) - reference).max(), 1)
        self.assertEqual(int(decoded[0,3,3]), 0)

    def test_all_block_sizes_mip_order_and_official_validation(self):
        for block, vk in ((4,157), (6,165), (8,171)):
            output, _ = self.convert(make_vtf(width=16, height=8, mip_count=5), f'block{block}', '--block', f'{block}x{block}')
            info = ktx_info(output)
            self.assertEqual(info['format'], vk)
            self.assertEqual(info['type_size'], 1)
            self.assertEqual(info['mips'], 5)
            offsets = [x[0] for x in info['levels']]
            self.assertEqual(offsets, sorted(offsets, reverse=True))
            self.assertTrue(all(o % 16 == 0 for o in offsets))
            for mip in range(5):
                decoded = decode_astc(CLI, output, self.root / f'mip-{block}-{mip}.png', mip=mip)
                self.assertLessEqual(np.abs(decoded.astype(int) - np.array([20 + mip * 30,30,40,255])).max(), 2)
        output, _ = self.convert(make_vtf(), 'srgb', '--srgb', '--block', '6x6')
        self.assertEqual(ktx_info(output)['format'], 166)

    def test_legacy_thumbnail_offsets(self):
        for minor in (0,1,2,3,4,5):
            output, _ = self.convert(make_vtf(minor=minor, thumbnail=True), f'v7-{minor}', '--dump-rgba')
            raw = Path(str(output) + '.rgba').read_bytes()
            self.assertEqual(raw, bytes((20,30,40,255)) * 16)
            self.assertIn('source.vtf.resource.00000001', ktx_info(output)['metadata'])

    def test_frames_cube_faces_and_legacy_sphere(self):
        for minor, stored in ((5,6), (4,7), (4,6)):
            output, _ = self.convert(make_vtf(frames=2, faces=6, stored_faces=stored, minor=minor), f'cube-{minor}-{stored}', '--block', '4x4')
            info = ktx_info(output)
            self.assertEqual((info['layers'], info['faces']), (2,6))
            for frame in range(2):
                for face in range(6):
                    image = decode_astc(CLI, output, self.root / f'f-{minor}-{stored}-{frame}-{face}.png', frame=frame, face=face)
                    self.assertLessEqual(np.abs(image.astype(int)-np.array([20,30+frame*60,40+face*25,255])).max(), 2)
            self.assertEqual('source.vtf.legacy_spheremap' in info['metadata'], stored == 7)

    def test_volume_including_source_compressed_depth_padding(self):
        for compressed in (False,True):
            def payload(mip, frame, face, z, w, h):
                value = 0xf800 if z == 0 else 0x07e0
                return struct.pack('<HHI', value, 0, 0) * (((w+3)//4)*((h+3)//4))
            raw = make_vtf(width=8, height=8, depth=4, mip_count=4, fmt=13 if compressed else 0,
                           payload=payload if compressed else None)
            output, _ = self.convert(raw, f'volume-{compressed}')
            info = ktx_info(output)
            self.assertEqual((info['format'], info['depth']), (37,4))
            for mip,(offset,size,_) in enumerate(info['levels']):
                w=h=max(1,8>>mip); d=max(1,4>>mip)
                self.assertEqual(size,w*h*d*4)
                image=np.frombuffer(info['data'][offset:offset+size],dtype=np.uint8).reshape(d,h,w,4)
                for z in range(d):
                    expected = [255,0,0,255] if compressed and z==0 else [0,255,0,255] if compressed else [20+mip*30,30,40+z*15,255]
                    self.assertTrue(np.all(image[z] == expected))

    def test_hdr_and_signed_values_remain_bit_exact(self):
        for fmt,vk,pixel in ((24,97,struct.pack('<4e',8.0,2.0,.5,-1.0)), (22,17,bytes((128,127)))):
            output, _ = self.convert(make_vtf(fmt=fmt,payload=lambda *x:pixel*16), f'raw-{fmt}')
            info=ktx_info(output);offset,size,_=info['levels'][0]
            self.assertEqual(info['format'],vk)
            self.assertEqual(info['data'][offset:offset+size],pixel*16)

    def test_signed_animation_mips_and_alignment(self):
        def payload(mip,frame,face,z,w,h):
            return bytes((128+frame,127-mip))*(w*h)
        output,_=self.convert(make_vtf(fmt=22,mip_count=3,frames=3,payload=payload),'signed-animation')
        info=ktx_info(output)
        self.assertEqual((info['format'],info['layers'],info['mips']),(17,3,3))
        for mip,(offset,size,_) in enumerate(info['levels']):
            w=h=max(1,4>>mip)
            expected=b''.join(payload(mip,frame,0,0,w,h) for frame in range(3))
            self.assertEqual(size,len(expected))
            self.assertEqual(info['data'][offset:offset+size],expected)

    def test_resume_validates_source_recipe_and_output_hash(self):
        raw=make_vtf()
        output,_=self.convert(raw,'resume')
        command=[BINARY,'--input',self.root/'resume.vtf','--out',output.parent,'--resume','--threads',1]
        run(*command)
        self.assertEqual(json.loads((output.parent/'manifest.json').read_text())['resumed'],1)
        original=hashlib.sha256(output.read_bytes()).hexdigest()
        output.write_bytes(output.read_bytes()[:-1])
        run(*command)
        self.assertEqual(json.loads((output.parent/'manifest.json').read_text())['resumed'],0)
        self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(),original)
        run(*command,'--block','8x8')
        self.assertEqual(ktx_info(output)['format'],171)
        changed=make_vtf(payload=lambda *x:bytes((80,30,40,255))*16)
        (self.root/'resume.vtf').write_bytes(changed)
        run(*command)
        self.assertEqual(json.loads((output.parent/'manifest.json').read_text())['resumed'],0)

    def test_bad_headers_fail_without_success_file(self):
        valid=make_vtf()
        cases=[valid[:-1],valid[:63],valid+b'extra']
        bad=bytearray(valid);bad[56]=255;cases.append(bytes(bad))
        bad=bytearray(valid);struct.pack_into('<I',bad,68,0xffffffff);cases.append(bytes(bad))
        bad=bytearray(valid);struct.pack_into('<I',bad,84,1);cases.append(bytes(bad))
        bad=bytearray(valid);struct.pack_into('<HH',bad,16,65535,65535);cases.append(bytes(bad))
        for i,raw in enumerate(cases):
            output,result=self.convert(raw,f'bad-{i}',ok=False)
            self.assertNotEqual(result.returncode,0)
            self.assertFalse(output.exists())
            manifest=json.loads((output.parent/'manifest.json').read_text())
            self.assertFalse(manifest['complete'])
            self.assertEqual(manifest['failed'],1)

    def test_vpk_embedded_preload_and_crc(self):
        raw=make_vtf();preload=37
        def archive(name,crc,version):
            tree=b'vtf\0materials\0'+name.encode()+b'\0'
            tree+=struct.pack('<IHHIIH',crc,preload,0x7fff,0,len(raw)-preload,0xffff)+raw[:preload]+b'\0\0\0'
            header=struct.pack('<III',0x55aa1234,version,len(tree))
            if version==2:header+=struct.pack('<4I',len(raw)-preload,0,0,0)
            return header+tree+raw[preload:]
        for version in (1,2):
            path=self.root/f'pak{version}_dir.vpk';path.write_bytes(archive('valid',zlib.crc32(raw),version))
            out=self.root/f'vpk{version}';run(BINARY,'--vpk',path,'--out',out)
            validate_ktx(VALIDATOR,out/'materials/valid.ktx2')
        path=self.root/'corrupt_dir.vpk';path.write_bytes(archive('bad',0,2))
        result=run(BINARY,'--vpk',path,'--out',self.root/'corrupt',ok=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('CRC mismatch',result.stderr)
        path=self.root/'unsafe_dir.vpk';path.write_bytes(archive('../../escape',zlib.crc32(raw),2))
        result=run(BINARY,'--vpk',path,'--out',self.root/'unsafe',ok=False)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('unsafe VPK path',result.stderr)


if __name__ == '__main__':
    unittest.main(argv=['test_converter'],verbosity=2)
