#!/usr/bin/env python3
"""Run actual D3D9 BC upload code against a host CPU context (not a GPU test)."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
DXVK=ROOT/'deps/dxvk-ohos-legacy'
s=(DXVK/'src/d3d9/d3d9_device.cpp').read_text()
a=s.index('  void D3D9DeviceEx::UpdateTextureFromBuffer(')
b=s.index('    if (likely(convertFormat.FormatType == D3D9ConversionFormat_None))',a)
body=s[a:b].replace('D3D9DeviceEx::UpdateTextureFromBuffer','UpdateTextureFromBuffer')+'  }\n'
code=(ROOT/'scripts/tests/d3d9_upload_harness.cpp.in').read_text().replace('__UPLOAD__',body)
with tempfile.TemporaryDirectory(prefix='d3d9-bc-upload-') as d:
    cpp,exe=Path(d)/'test.cpp',Path(d)/'test'
    cpp.write_text(code)
    subprocess.run([os.environ.get('CXX','g++'),'-std=c++17','-O1','-g',*shlex.split(os.environ.get('BC_TEST_SANITIZERS','-fsanitize=address,undefined')),
        '-I'+str(DXVK/'include'),'-I'+str(DXVK/'src'),str(cpp),str(DXVK/'src/util/util_bc.cpp'),
        str(DXVK/'src/util/etcpak/ProcessRGB.cpp'),str(DXVK/'src/util/etcpak/Tables.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
