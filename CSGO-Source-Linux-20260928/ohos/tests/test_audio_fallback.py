#!/usr/bin/env python3
"""Focused host regression for the OHOS standard-mixer fallback.

Compiles the actual OHOS-only implementation with minimal stand-ins for engine
interface types. This does not replace a full OHOS engine build or device test.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
AUDIO = ROOT / "src/engine/audio/private"


class AudioFallbackTest(unittest.TestCase):
    def test_standard_mixer_defaults_cannot_enable_unsupported_hrtf(self):
        source = (AUDIO / "snd_dma.cpp").read_text()
        self.assertIn('ConVar snd_hwcompat( "snd_hwcompat", "1",', source)
        self.assertIn('"OHOS uses the standard audio mixer.", true, 1.0f, true, 1.0f', source)
        self.assertIn('"Steam Audio occlusion is unavailable on OHOS.", true, 0.0f, true, 0.0f', source)
        self.assertIn('bool IsUsingHRTF()\n{\n#if defined( __OHOS__ )\n\treturn false;', source)
        self.assertIn('#if !defined( ANDROID ) && !defined( __OHOS__ )\n\t// The older SDK', source)

    def test_passthrough_ownership_lengths_and_invalid_arguments(self):
        source = (AUDIO / "snd_wave_data.cpp").read_bytes().decode("latin1")
        start = source.index('#if defined( __OHOS__ )\n// No ABI-compatible Steam Audio implementation')
        end = source.index('#else\n// set this to zero', start)
        implementation = source[start:].split('\n', 1)[1][:end - start - len('#if defined( __OHOS__ )\n')]
        self.assertNotIn('iplCreate', implementation)
        self.assertNotIn('ThreadExecute', implementation)
        test = '''#include <cassert>
#include <cstring>
#include <cstddef>
struct IWaveData {}; struct hrtf_info_t {}; struct Vector {};
''' + implementation + '''
int main() {
    IWaveData wave; hrtf_info_t dir;
    assert(CreateWaveDataHRTF(&wave, &dir) == &wave);
    assert(CreateWaveDataHRTFForVoice(&wave, &dir) == &wave);
    assert(CreateWaveDataHRTF(nullptr, nullptr) == nullptr);
    short input[] = {1, 2, 3, 4, -5, -6};
    short output[] = {0, 0, 0, 0, 99, 99};
    assert(RunHRTFEffect(input, output, 2, Vector{}, 0, 0));
    assert(output[0] == 1 && output[3] == 4 && output[4] == 99 && output[5] == 99);
    assert(!RunHRTFEffect(nullptr, output, 2, Vector{}, 0, 0));
    assert(!RunHRTFEffect(input, nullptr, 2, Vector{}, 0, 0));
    assert(!RunHRTFEffect(input, output, -1, Vector{}, 0, 0));
    assert(RunHRTFEffect(input, input, 3, Vector{}, 0, 0));
    assert(RunHRTFEffect(input, output, 0, Vector{}, 0, 0));
    StartPhononThread(); ShutdownPhononThread();
}
'''
        compiler = shutil.which(os.environ.get('HOST_CXX', 'g++'))
        if not compiler:
            self.skipTest('Host C++ compiler unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'audio.cpp'
            binary = Path(tmp) / 'audio-test'
            src.write_text(test)
            subprocess.run([compiler, '-std=c++11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
