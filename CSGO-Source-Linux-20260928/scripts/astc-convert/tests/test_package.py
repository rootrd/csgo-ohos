#!/usr/bin/env python3
"""Archive integrity tests; KTX format/codec tests live in test_converter.py."""
import contextlib
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from package_pack import Entry, package, verify_archive


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, indent=2) + '\n').encode()


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='astc-package-test-')
        self.work = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def fixture(self, ident='pack'):
        root = self.work / ident
        root.mkdir()
        # Opaque packaging fixture with a mip index for quality-link checks.
        # The converter suite independently produces and validates real KTX2.
        payload = bytearray(104)
        payload[:12] = b'\xabKTX 20\xbb\r\n\x1a\n'
        struct.pack_into('<I', payload, 40, 1)
        struct.pack_into('<3Q', payload, 80, 104, 16, 16)
        payload += bytes(range(16))
        name = 'materials/test/texture with spaces.ktx2'
        record = {'path': name.replace('.ktx2', '.vtf'), 'output': name,
                  'source_sha256': digest(b'original'), 'source_bytes': 8,
                  'source': {'width': 4, 'height': 4, 'mips': 1},
                  'output_sha256': digest(payload), 'output_bytes': len(payload),
                  'status': 'converted', 'recipe': {'quality': 'medium'}, 'encoding': 'astc-ldr', 'block': 4}
        manifest = {'schema': 2, 'complete': True, 'failed': 0, 'mode': 'convert',
                    'source': 'fixture/pak01', 'selected': 1, 'selection': {'filter': '', 'limit': 0},
                    'recipe': record['recipe'], 'source_bytes': 8, 'output_bytes': len(payload), 'textures': [record]}
        inventory = self.work / (ident + '-inventory.json')
        inventory.write_bytes(encoded({'complete': True, 'failed': 0, 'textures': [record]}))
        validation = {'valid': True, 'errors': [], 'textures': 1, 'subresources': 1,
                      'source_bytes': 8, 'ktx2_bytes': len(payload),
                      'manifest_sha256': digest(encoded(manifest)), 'inventory_sha256': digest(inventory.read_bytes())}
        files = {'manifest.json': encoded(manifest), 'validation.json': encoded(validation),
                 name: bytes(payload), name + '.json': encoded(record)}
        for relative, data in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (root / 'SHA256SUMS').write_text(''.join(f'{digest(data)}  {name}\n' for name, data in sorted(files.items())))
        return root, inventory, record

    def build(self, root, inventory, output, quality=None):
        with contextlib.redirect_stdout(io.StringIO()):
            return package(root, inventory, output, quality, threads=1)

    def archive_files(self, path):
        raw = subprocess.check_output(['zstd', '-q', '-d', '-c', str(path)])
        with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
            members = archive.getmembers()
            for member in members:
                self.assertTrue(member.isfile())
                self.assertEqual((member.uid, member.gid, member.mtime, member.mode), (0, 0, 0, 0o644))
            return {member.name: archive.extractfile(member).read() for member in members}

    def test_roundtrip_checksums_and_reproducible_metadata(self):
        root, inventory, record = self.fixture()
        first, second = self.work / 'first.tar.zst', self.work / 'second.tar.zst'
        receipt = self.build(root, inventory, first)
        files = self.archive_files(first)
        self.assertEqual(files[root.name + '/' + record['output']], (root / record['output']).read_bytes())
        sums = files.pop(root.name + '/SHA256SUMS').decode().splitlines()
        expected = {}
        for line in sums:
            checksum, name = line.split('  ', 1)
            expected[root.name + '/' + name] = checksum
        self.assertEqual(set(files), set(expected))
        self.assertEqual({name: digest(data) for name, data in files.items()}, expected)
        self.assertEqual(receipt['sha256'], digest(first.read_bytes()))
        self.assertTrue(receipt['archive_readback_verified'])
        self.assertEqual(Path(str(first) + '.sha256').read_text(), f'{digest(first.read_bytes())}  {first.name}\n')
        for path in root.rglob('*'):
            if path.is_file():
                os.utime(path, (1234567, 1234567))
                path.chmod(0o600)
        self.build(root, inventory, second)
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_rejects_corruption_stale_validation_and_incomplete_coverage(self):
        for case in ('texture', 'sidecar', 'stale', 'missing', 'extra', 'inventory', 'checksums'):
            with self.subTest(case=case):
                root, inventory, record = self.fixture(case)
                texture = root / record['output']
                if case == 'texture':
                    data = bytearray(texture.read_bytes()); data[-1] ^= 1; texture.write_bytes(data)
                elif case == 'sidecar':
                    sidecar = Path(str(texture) + '.json'); sidecar.write_bytes(sidecar.read_bytes() + b' ')
                elif case == 'stale':
                    manifest = root / 'manifest.json'; manifest.write_bytes(manifest.read_bytes() + b' ')
                elif case == 'missing':
                    texture.unlink()
                elif case == 'extra':
                    (root / 'unlisted.ktx2').write_bytes(b'unlisted')
                elif case == 'inventory':
                    inventory.write_bytes(inventory.read_bytes() + b' ')
                else:
                    sums = root / 'SHA256SUMS'; sums.write_text('\n'.join(sums.read_text().splitlines()[:-1]) + '\n')
                output = self.work / (case + '.tar.zst')
                with self.assertRaises((ValueError, OSError)):
                    self.build(root, inventory, output)
                self.assertFalse(output.exists())
                self.assertFalse(Path(str(output) + '.sha256').exists())
                self.assertFalse(list(self.work.glob('.astc-package-*')))

    def test_rejects_symlink_busy_source_and_existing_output(self):
        root, inventory, record = self.fixture()
        texture = root / record['output']
        outside = self.work / 'outside.ktx2'
        texture.rename(outside); texture.symlink_to(outside)
        output = self.work / 'package.tar.zst'
        with self.assertRaisesRegex(ValueError, 'symlinks'):
            self.build(root, inventory, output)
        texture.unlink(); outside.rename(texture)
        with (root / '.vtf2astc.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaises(BlockingIOError):
                self.build(root, inventory, output)
        output.write_bytes(b'existing archive')
        with self.assertRaisesRegex(ValueError, 'already exists'):
            self.build(root, inventory, output)
        self.assertEqual(output.read_bytes(), b'existing archive')

    def test_readback_rejects_truncated_compression_frame(self):
        root, inventory, _ = self.fixture()
        output = self.work / 'package.tar.zst'
        self.build(root, inventory, output)
        files = self.archive_files(output)
        entries = {name[len(root.name) + 1:]: Entry.data(data) for name, data in files.items()}
        output.write_bytes(output.read_bytes()[:-5])
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises((ValueError, tarfile.TarError, EOFError)):
            verify_archive(output, root.name, entries)

    def test_quality_report_must_match_production_mips(self):
        root, inventory, record = self.fixture()
        folder = self.work / 'quality'; folder.mkdir()
        sample = folder / 'sample-4' / Path(record['output']).name
        sample.parent.mkdir()
        sample.write_bytes((root / record['output']).read_bytes())
        Path(str(sample) + '.json').write_bytes(encoded(record))
        (folder / 'quality.json').write_bytes(encoded({'samples': [
            {'id': 'sample', 'path': record['path'], 'width': 4, 'height': 4, 'blocks': {'4': {}}}]}))
        for name in ('comparison.html', 'comparison.png', 'crops.png'):
            (folder / name).write_bytes(b'quality fixture')
        output = self.work / 'with-quality.tar.zst'
        self.build(root, inventory, output, folder)
        files = self.archive_files(output)
        proof = json.loads(files[root.name + '/quality/production.json'])
        self.assertTrue(proof['all_mip_payloads_match'])
        self.assertEqual(proof['samples'][0]['output_sha256'], record['output_sha256'])
        data = bytearray(sample.read_bytes()); data[-1] ^= 1; sample.write_bytes(data)
        sidecar = dict(record, output_sha256=digest(data))
        Path(str(sample) + '.json').write_bytes(encoded(sidecar))
        with self.assertRaisesRegex(ValueError, 'mips differ'):
            self.build(root, inventory, self.work / 'bad-quality.tar.zst', folder)


if __name__ == '__main__':
    unittest.main(verbosity=2)
