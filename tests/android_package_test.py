"""Dump-free packaging regression checks. Run with Python 3."""
import importlib.util
import tempfile
from unittest.mock import patch
import pathlib
import subprocess
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[1]
ANDROID = '{http://schemas.android.com/apk/res/android}'

spec = importlib.util.spec_from_file_location('verify', ROOT / 'scripts/android-verify-libs.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)

class AndroidPackageTests(unittest.TestCase):
    def test_reject_bad_native_libraries(self):
        with tempfile.TemporaryDirectory() as directory:
            pathlib.Path(directory, 'libexample.so').touch()
            valid = 'Machine: AArch64\n LOAD 0 0 0 0 0 R 0x4000\n (SONAME) [libexample.so]\n (NEEDED) [libc.so]'
            with patch.object(verify.subprocess, 'check_output', return_value=valid):
                self.assertEqual(verify.verify(directory, 'readelf'), 1)
            for bad in [valid.replace('AArch64', 'X86-64'), valid.replace('0x4000', '0x1000'), valid.replace('[libc.so]', '[libmissing.so]'), valid.replace('[libexample.so]', '[libexample.so.1]')]:
                with patch.object(verify.subprocess, 'check_output', return_value=bad):
                    with self.assertRaises(ValueError):
                        verify.verify(directory, 'readelf')

    def test_no_broad_storage_or_network_permissions(self):
        root = ET.parse(ROOT / 'android/app/src/main/AndroidManifest.xml').getroot()
        self.assertEqual(root.findall('uses-permission'), [])
        activity = root.find('application/activity')
        self.assertEqual(activity.get(ANDROID + 'resizeableActivity'), 'true')

    def test_private_paths_and_missing_game_check_precede_loading(self):
        source = (ROOT / 'android/app/src/main/cpp/main.c').read_text()
        self.assertIn('SDL_GetAndroidExternalStoragePath()', source)
        self.assertLess(source.index('freopen("runtime.log"'), source.index('dlopen('))
        self.assertLess(source.index('access("targets/exefs/main.elf"'), source.index('dlopen('))
        for name in ['WT_USERDATA', 'WT_ROMFS', 'WT_CONFIG', 'WT_GRAPHICS_CONFIG']:
            self.assertIn('setenv("' + name + '"', source)

    def test_packaging_script_parses(self):
        subprocess.run(['bash', '-n', str(ROOT / 'scripts/android-package.sh')], check=True)

    def test_arm64_and_16k_alignment(self):
        gradle = (ROOT / 'android/app/build.gradle').read_text()
        cmake = (ROOT / 'android/app/src/main/cpp/CMakeLists.txt').read_text()
        self.assertIn("abiFilters 'arm64-v8a'", gradle)
        self.assertIn('max-page-size=16384', cmake)
        self.assertIn('common-page-size=16384', cmake)
        self.assertNotIn('add_library(SDL3 SHARED IMPORTED)', cmake)

if __name__ == '__main__':
    unittest.main()
