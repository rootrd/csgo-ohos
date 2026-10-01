#!/usr/bin/env python3
"""Fetch checksum-pinned official sources for the OHOS dependency rebuild.

Uses Python 3.12 safe tar extraction. Existing marked source trees are retained,
including local port patches. Does not fetch proprietary game resources/binaries.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import tarfile
import tempfile

SOURCES = [{'name': 'sse2neon',
  'url': 'https://codeload.github.com/DLTcollab/sse2neon/tar.gz/8d1d9f1cae82de66d9daea53f4f7ca30024f478b',
  'sha256': '3bf5f9fc10a52359e7f826b7df3f042563eb9240ffe05590d05f7b09f67230ef',
  'bytes': 248931,
  'destination': 'sse2neon'},
 {'name': 'libjpeg-turbo',
  'url': 'https://codeload.github.com/libjpeg-turbo/libjpeg-turbo/tar.gz/af9c1c268520a29adf98cad5138dafe612b3d318',
  'sha256': '01ffa0ee8dfac5c92446b8ed2ea21badd5f490afc594ef1b28758297e9b3ca39',
  'bytes': 2518717,
  'destination': 'libjpeg-turbo'},
 {'name': 'curl',
  'url': 'https://codeload.github.com/curl/curl/tar.gz/01346829096c61b372692f6dc43ffa778c6caccd',
  'sha256': '64cd4fdf196bce8bc381f4ae6f8d0e4e4c4f3b93b73b69deb8d78dab24f2d9ff',
  'bytes': 3697870,
  'destination': 'curl'},
 {'name': 'mbedtls',
  'url': 'https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/068ff080b369adfac81509f9b57b2afabaf82dc5',
  'sha256': 'ca6bd316bbec49ef20088f39b8755fcaec0b7e45781506de55b17cd191ae6937',
  'bytes': 5681475,
  'destination': 'mbedtls'},
 {'name': 'freetype',
  'url': 'https://gitlab.freedesktop.org/freetype/freetype/-/archive/0a0221a1347e2f1e07c395263540026e9a0aa7c7/freetype-0a0221a1347e2f1e07c395263540026e9a0aa7c7.tar.gz',
  'sha256': '11cd478953fc1d382f20a233b8e2aed6c31b47cbf0568c4bdb3334bcc1550698',
  'bytes': 2517889,
  'destination': 'freetype'},
 {'name': 'fribidi',
  'url': 'https://codeload.github.com/fribidi/fribidi/tar.gz/refs/tags/v1.0.15',
  'sha256': '0db5f0621b6fbfae5960c30da4f132009fd72bf4687f1b04a87a4cfc2a08ea38',
  'bytes': 2113286,
  'destination': 'fribidi'},
 {'name': 'glib',
  'url': 'https://codeload.github.com/GNOME/glib/tar.gz/refs/tags/2.80.4',
  'sha256': '7be7c1bdd60e546624ce512697f057e86c904585cb8188532c9d5c3d9c33006b',
  'bytes': 9694438,
  'destination': 'glib'},
 {'name': 'libexpat',
  'url': 'https://codeload.github.com/libexpat/libexpat/tar.gz/R_2_5_0',
  'sha256': 'ab00ee05c7067fd10a35c5d2a4922ebba746ddd50ff83b79c828da17bbdf1757',
  'bytes': 8320988,
  'destination': 'libexpat'},
 {'name': 'fontconfig',
  'url': 'https://gitlab.freedesktop.org/fontconfig/fontconfig/-/archive/2.15.0/fontconfig-2.15.0.tar.gz',
  'sha256': 'cdebb4b805d33e9bdefcc0ef9743db638d2acb21139bbe1a6a85878d4c3e8c9e',
  'bytes': 572009,
  'destination': 'fontconfig'},
 {'name': 'pixman',
  'url': 'https://gitlab.freedesktop.org/pixman/pixman/-/archive/pixman-0.44.2/pixman-pixman-0.44.2.tar.gz',
  'sha256': '1ac33e229549f6341a677d530d1944431139d222ecad1554818788ae9ef32f6f',
  'bytes': 816343,
  'destination': 'pixman'},
 {'name': 'harfbuzz',
  'url': 'https://codeload.github.com/harfbuzz/harfbuzz/tar.gz/refs/tags/8.5.0',
  'sha256': '7ad8e4e23ce776efb6a322f653978b3eb763128fd56a90252775edb9fd327956',
  'bytes': 36326383,
  'destination': 'harfbuzz'},
 {'name': 'pango',
  'url': 'https://codeload.github.com/GNOME/pango/tar.gz/refs/tags/1.54.0',
  'sha256': '317f366bb255282d3e64ccf95b1d57cbea8636578b199c158235e1f257e5167f',
  'bytes': 2322790,
  'destination': 'pango'},
 {'name': 'libffi',
  'url': 'https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz',
  'sha256': 'b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e',
  'bytes': 1391684,
  'destination': 'libffi'},
 {'name': 'pcre2',
  'url': 'https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.45/pcre2-10.45.tar.gz',
  'sha256': '0e138387df7835d7403b8351e2226c1377da804e0737db0e071b48f07c9d12ee',
  'bytes': 2715958,
  'destination': 'pcre2'},
 {'name': 'cairo',
  'url': 'https://gitlab.freedesktop.org/cairo/cairo/-/archive/1.18.2/cairo-1.18.2.tar.gz',
  'sha256': '7bbfb469b89b1f60584b4f39a1366789b7625a5796905870e108b6c0e8ca8216',
  'bytes': 47817572,
  'destination': 'cairo'},
 {'name': 'mbedtls-framework',
  'url': 'https://codeload.github.com/Mbed-TLS/mbedtls-framework/tar.gz/dde0c4a0e448a0552f18817dcea633bb851fd288',
  'bytes': 1045083,
  'sha256': '3b0d4864fa877dc9165b90ff3d5b68505c6176c0b22d317d99fe8c89a993e9fd',
  'destination': 'mbedtls/framework'},
 {'name': 'gvdb',
  'url': 'https://gitlab.gnome.org/GNOME/gvdb/-/archive/0854af0fdb6d527a8d1999835ac2c5059976c210/gvdb-0854af0fdb6d527a8d1999835ac2c5059976c210.tar.gz',
  'bytes': 20837,
  'sha256': '08352e54e8216d9001820c627c62858585465b51dc557cc22f0f4770ed182ebd',
  'destination': 'glib/subprojects/gvdb'},
 {'name': 'proxy-libintl',
  'url': 'https://codeload.github.com/frida/proxy-libintl/tar.gz/c03e1a74b17fa7ec467e110130775409e4828a4c',
  'sha256': 'b2be1569c4e6b0131a446d857665a192674d36185e2fa534614eece0b0ea70c3',
  'bytes': 13034,
  'destination': 'glib/subprojects/proxy-libintl'},
 {'name': 'openssl',
  'url': 'https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz',
  'sha256': '603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a',
  'bytes': 53279637,
  'destination': 'openssl'}]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('work_dir', type=Path)
    args = parser.parse_args()
    root = args.work_dir.resolve()
    (root / 'downloads').mkdir(parents=True, exist_ok=True)
    (root / 'src').mkdir(exist_ok=True)
    for record in SOURCES:
        destination = root / 'src' / record['destination']
        marker = destination / '.ohos-source.json'
        if marker.exists():
            existing = json.loads(marker.read_text())
            if existing.get('sha256') != record['sha256']:
                raise SystemExit(f'Refusing unexpected source revision: {destination}')
            continue
        if destination.exists() and any(destination.iterdir()):
            raise SystemExit(f'Refusing unmarked existing source: {destination}; use a fresh CSGO_DEPS_WORK')
        archive = root / 'downloads' / (record['name'] + '.tar.gz')
        if not archive.exists():
            pending = archive.with_suffix('.download')
            subprocess.run(['curl', '-fLsS', '--retry', '3', '--connect-timeout', '20', '-o', str(pending), record['url']], check=True)
            pending.rename(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != record['sha256']:
            raise SystemExit(f'Source checksum mismatch: {archive}')
        with tempfile.TemporaryDirectory(dir=root / 'src') as temporary:
            with tarfile.open(archive) as tar:
                tar.extractall(temporary, filter='data')
            entries = list(Path(temporary).iterdir())
            if len(entries) != 1 or not entries[0].is_dir():
                raise SystemExit(f'Unexpected archive layout: {archive}')
            destination.parent.mkdir(parents=True, exist_ok=True)
            if destination.exists():
                destination.rmdir()  # only the previously verified empty dir
            entries[0].rename(destination)
        marker.write_text(json.dumps(record, indent=2) + '\n')
        print('Fetched', record['name'], flush=True)
    (root / 'source-receipts.json').write_text(json.dumps(SOURCES, indent=2) + '\n')


if __name__ == '__main__':
    main()
