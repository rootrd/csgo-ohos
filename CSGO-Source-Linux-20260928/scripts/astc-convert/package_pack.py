#!/usr/bin/env python3
"""Package a validated texture overlay; run via build-astc-convert.sh package.

Hash each input while writing a deterministic tar, then decompress the completed
archive and verify every member before publishing it. No extraction is needed.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import tarfile
import tempfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n').encode()


def safe_name(name):
    path = PurePosixPath(name)
    require(bool(name) and name != '.' and not path.is_absolute()
            and str(path) == name and '..' not in path.parts
            and '\\' not in name and all(ord(c) >= 32 and ord(c) != 127 for c in name),
            f'unsafe archive path: {name!r}')
    return name


def regular_file(root, name):
    path = root
    for part in PurePosixPath(safe_name(name)).parts:
        path = path / part
        require(not path.is_symlink(), f'symlinks are not package inputs: {path}')
    require(path.is_file(), f'missing regular file: {path}')
    return path


@dataclass
class Entry:
    source: object  # bytes for metadata snapshots, Path for streamed payloads
    digest: str
    size: int

    @classmethod
    def data(cls, data):
        return cls(data, sha256(data), len(data))

    @classmethod
    def file(cls, path, digest=None):
        if digest is None:
            with path.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        return cls(path, digest, path.stat().st_size)

    def open(self):
        return io.BytesIO(self.source) if isinstance(self.source, bytes) else self.source.open('rb')


def checksums(data):
    result = {}
    for line in data.decode('utf-8').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        require(match is not None, 'invalid SHA256SUMS line')
        digest, name = match.groups()
        safe_name(name)
        require(name not in result, f'duplicate checksum: {name}')
        result[name] = digest
    return result


def collect_pack(root, inventory_path):
    manifest_data = regular_file(root, 'manifest.json').read_bytes()
    validation_data = regular_file(root, 'validation.json').read_bytes()
    manifest, validation = json.loads(manifest_data), json.loads(validation_data)
    require(manifest['complete'] and manifest['failed'] == 0 and manifest['mode'] == 'convert',
            'conversion did not complete')
    require(validation['valid'] and not validation['errors'], 'pack validation failed')
    require(validation['manifest_sha256'] == sha256(manifest_data), 'stale validation: manifest changed')
    records = manifest['textures']
    require(len(records) == manifest['selected'] == validation['textures'] and len(records) > 0,
            'texture count differs from validation')
    require(len({r['path'] for r in records}) == len(records), 'duplicate source path')
    outputs = {safe_name(r['output']) for r in records}
    require(len(outputs) == len(records) and all(p.endswith('.ktx2') for p in outputs),
            'duplicate or invalid output path')
    require(all(r['status'] == 'converted' and r['recipe'] == manifest['recipe'] for r in records),
            'incomplete record or inconsistent encoding recipe')
    for field in ('source_bytes', 'output_bytes'):
        require(sum(r[field] for r in records) == manifest[field], f'inconsistent {field}')
    require(manifest['output_bytes'] == validation['ktx2_bytes']
            and manifest['source_bytes'] == validation['source_bytes'], 'validation byte counts differ')
    expected = outputs | {p + '.json' for p in outputs} | {'manifest.json', 'validation.json'}
    sums = checksums(regular_file(root, 'SHA256SUMS').read_bytes())
    require(set(sums) == expected, 'checksum coverage differs from manifest')
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*.ktx2')}
    sidecars = {p.relative_to(root).as_posix() for p in root.rglob('*.ktx2.json')}
    require(actual == outputs and sidecars == {p + '.json' for p in outputs},
            'missing or unexpected texture/sidecar in pack')
    entries = {}
    for name, data in (('manifest.json', manifest_data), ('validation.json', validation_data)):
        require(sha256(data) == sums[name], f'SHA-256 differs: {name}')
        entries[name] = Entry.data(data)
    for record in records:
        name = record['output']
        require(sums[name] == record['output_sha256'], f'checksum differs from manifest: {name}')
        path = regular_file(root, name)
        require(path.stat().st_size == record['output_bytes'], f'file size differs: {name}')
        entries[name] = Entry.file(path, sums[name])
        sidecar = regular_file(root, name + '.json').read_bytes()
        require(sha256(sidecar) == sums[name + '.json'] and json.loads(sidecar) == record,
                f'sidecar differs: {name}')
        entries[name + '.json'] = Entry.data(sidecar)
    require(not inventory_path.is_symlink(), 'inventory must be a regular file')
    inventory_data = inventory_path.read_bytes()
    require(sha256(inventory_data) == validation['inventory_sha256'], 'source inventory differs from validation')
    inventory = json.loads(inventory_data)
    require(inventory['complete'] and inventory['failed'] == 0, 'source inventory did not complete')
    def coverage(rows):
        return {r['path']: (r['source_sha256'], r['source_bytes'], r['source']) for r in rows}
    require(len(inventory['textures']) == len(records)
            and coverage(inventory['textures']) == coverage(records), 'source inventory coverage differs')
    entries['source-inventory.json'] = Entry.data(inventory_data)
    return entries, manifest, validation


def mip_payloads(path):
    data = path.read_bytes()
    require(data[:12] == b'\xabKTX 20\xbb\r\n\x1a\n', f'not KTX2: {path}')
    count = struct.unpack_from('<I', data, 40)[0]
    require(0 < count <= 32, f'invalid mip count: {path}')
    result = []
    for mip in range(count):
        offset, length, _ = struct.unpack_from('<3Q', data, 80 + 24 * mip)
        require(offset + length <= len(data), f'truncated mip: {path}')
        result.append(data[offset:offset + length])
    return result


def attach_quality(entries, root, folder, manifest):
    report_data = regular_file(folder, 'quality.json').read_bytes()
    report = json.loads(report_data)
    records = {r['path']: r for r in manifest['textures']}
    matched = []
    for sample in report['samples']:
        ident = safe_name(sample['id'])
        require('/' not in ident, 'invalid quality sample id')
        record = records[sample['path']]
        block = str(record['block'])
        require(block in sample['blocks'], f'quality report lacks production block: {ident}')
        name = f'{ident}-{block}/{PurePosixPath(record["output"]).name}'
        path = regular_file(folder, name)
        sidecar = json.loads(regular_file(folder, name + '.json').read_bytes())
        require(sidecar['source_sha256'] == record['source_sha256']
                and (sample['width'], sample['height']) == (record['source']['width'], record['source']['height']),
                f'quality sample source differs: {ident}')
        require(sha256(path.read_bytes()) == sidecar['output_sha256'], f'quality sample changed: {ident}')
        require(mip_payloads(path) == mip_payloads(regular_file(root, record['output'])),
                f'quality sample mips differ from production: {ident}')
        matched.append({'id': ident, 'path': sample['path'], 'block': record['block'],
                        'source_sha256': record['source_sha256'], 'output_sha256': record['output_sha256']})
    require(bool(matched), 'empty quality report')
    entries['quality/quality.json'] = Entry.data(report_data)
    for name in ('comparison.html', 'comparison.png', 'crops.png'):
        entries['quality/' + name] = Entry.file(regular_file(folder, name))
    entries['quality/production.json'] = Entry.data(json_bytes({
        'schema': 1, 'manifest_sha256': entries['manifest.json'].digest,
        'quality_report_sha256': sha256(report_data), 'all_mip_payloads_match': True, 'samples': matched}))


class HashingReader:
    def __init__(self, stream):
        self.stream = stream
        self.hash = hashlib.sha256()

    def read(self, size=-1):
        data = self.stream.read(size)
        self.hash.update(data)
        return data


def write_archive(path, prefix, entries, threads):
    with path.open('wb') as output, subprocess.Popen(
            ['zstd', '-q', '-6', f'-T{threads}', '--check', '-c'], stdin=subprocess.PIPE, stdout=output) as process:
        try:
            with tarfile.open(fileobj=process.stdin, mode='w|', format=tarfile.GNU_FORMAT) as archive:
                for index, (name, entry) in enumerate(sorted(entries.items()), 1):
                    info = tarfile.TarInfo(prefix + '/' + name)
                    info.size, info.mode, info.mtime = entry.size, 0o644, 0
                    with entry.open() as source:
                        reader = HashingReader(source)
                        archive.addfile(info, reader)
                        require(not source.read(1), f'input grew while packaging: {name}')
                        require(reader.hash.hexdigest() == entry.digest, f'SHA-256 differs: {name}')
                    if index % 1000 == 0 or index == len(entries):
                        print(f'Packed {index}/{len(entries)}', flush=True)
        finally:
            process.stdin.close()
        require(process.wait() == 0, 'zstd compression failed')


def verify_archive(path, prefix, entries):
    seen = set()
    with subprocess.Popen(['zstd', '-q', '-d', '-c', str(path)], stdout=subprocess.PIPE) as process:
        try:
            with tarfile.open(fileobj=process.stdout, mode='r|') as archive:
                for member in archive:
                    require(member.isfile() and member.name.startswith(prefix + '/'), 'unexpected archive member')
                    name = safe_name(member.name[len(prefix) + 1:])
                    require(name in entries and name not in seen, f'unexpected/duplicate archive member: {name}')
                    require(member.size == entries[name].size, f'archive member size differs: {name}')
                    stream = archive.extractfile(member)
                    with stream:
                        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
                    require(digest == entries[name].digest, f'archive readback SHA-256 differs: {name}')
                    seen.add(name)
                    if len(seen) % 1000 == 0 or len(seen) == len(entries):
                        print(f'Archive verified {len(seen)}/{len(entries)}', flush=True)
            # Drain tar padding so the decompressor can finish and check its checksum.
            while process.stdout.read(1024 * 1024):
                pass
            require(process.wait() == 0, 'zstd decompression/checksum failed')
        finally:
            if process.poll() is None:
                process.terminate()
    require(seen == set(entries), 'archive is missing files')


def package(root, inventory, output, quality=None, threads=4):
    require(1 <= threads <= 64, '--threads must be between 1 and 64')
    root, inventory, output = root.resolve(), inventory.absolute(), output.absolute()
    prefix = safe_name(root.name)
    require(output.name.endswith('.tar.zst'), '--out must end in .tar.zst')
    require(not output.is_relative_to(root), 'archive output must be outside the input pack')
    companions = [Path(str(output) + suffix) for suffix in ('.sha256', '.json')]
    require(not any(p.exists() or p.is_symlink() for p in [output] + companions),
            'output already exists; choose a new archive name')
    with (root / '.vtf2astc.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        entries, manifest, validation = collect_pack(root, inventory)
        entries['README.md'] = Entry.data(Path(__file__).with_name('PACKAGE-README.md').read_bytes())
        if quality:
            attach_quality(entries, root, quality.resolve(), manifest)
        zstd_version = subprocess.check_output(['zstd', '--version'], text=True).strip()
        distribution = {
            'schema': 1, 'kind': 'source-vtf-texture-overlay', 'archive_root': prefix,
            'source': manifest['source'], 'selection': manifest['selection'],
            'textures': manifest['selected'], 'subresources': validation['subresources'],
            'encodings': dict(Counter(r['encoding'] for r in manifest['textures'])),
            'astc_blocks': dict(Counter(str(r['block']) for r in manifest['textures'] if r['encoding'] == 'astc-ldr')),
            'source_bytes': manifest['source_bytes'], 'ktx2_bytes': manifest['output_bytes'],
            'recipe': manifest['recipe'], 'manifest_sha256': entries['manifest.json'].digest,
            'inventory_sha256': entries['source-inventory.json'].digest,
            'quality_report': 'quality/comparison.html' if quality else None,
            'packaging': {'format': 'GNU tar + Zstandard', 'zstd': zstd_version,
                          'level': 6, 'threads': threads, 'tar_mtime': 0, 'tar_uid': 0, 'tar_gid': 0}}
        entries['distribution.json'] = Entry.data(json_bytes(distribution))
        entries['SHA256SUMS'] = Entry.data(''.join(
            f'{entry.digest}  {name}\n' for name, entry in sorted(entries.items())).encode())
        output.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.astc-package-', dir=output.parent) as temporary:
            pending = Path(temporary) / output.name
            write_archive(pending, prefix, entries, threads)
            verify_archive(pending, prefix, entries)
            with pending.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            receipt = {'schema': 1, 'archive': output.name, 'sha256': digest,
                       'archive_bytes': pending.stat().st_size, 'files': len(entries),
                       'uncompressed_file_bytes': sum(e.size for e in entries.values()),
                       'archive_readback_verified': True, 'distribution': distribution}
            # Hard linking publishes the completed file atomically without overwriting.
            os.link(pending, output)
            with companions[0].open('x') as stream:
                stream.write(f'{digest}  {output.name}\n')
            with companions[1].open('xb') as stream:
                stream.write(json_bytes(receipt))
        print(json.dumps(receipt, ensure_ascii=False, indent=2), flush=True)
        return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pack', required=True, type=Path)
    parser.add_argument('--inventory', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--quality', type=Path, help='existing quality_report.py output, matched against this pack')
    parser.add_argument('--threads', type=int, default=4)
    args = parser.parse_args()
    try:
        package(args.pack, args.inventory, args.out, args.quality, args.threads)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError, tarfile.TarError) as error:
        parser.exit(1, f'package_pack: {error}\n')


if __name__ == '__main__':
    main()
