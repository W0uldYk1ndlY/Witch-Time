"""Execute the packaging-only Gradle task with small fixture bytes, never load a driver."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'android/third_party/turnip_adreno830'
PROFILE = Path('bundled_driver/adreno830')
LIBRARY = 'vulkan.mesa26_3_gralloc_a8xx.so'
SHA256 = '64ad4fbab81e787eb4373bccc4d02604e1b8a87bd2dc7e850bc9f7e91f881221'


class TurnipBundleTest(unittest.TestCase):
    def test_pinned_contract_and_notices(self):
        assets = SOURCE / 'assets' / PROFILE
        manifest = json.loads((assets / 'bundle-manifest.json').read_text())
        self.assertEqual(manifest['sha256'], SHA256)
        self.assertEqual(manifest['sizeBytes'], 14495432)
        self.assertEqual(manifest['assetPath'], (PROFILE / LIBRARY).as_posix())
        self.assertEqual(manifest['profileId'], 'turnip-adreno830-unleashed-mesa26.3-gralloc')
        self.assertIn('/f65c40ba2788ebd7318361670bcbb82cfdef6ae8/driver/' + LIBRARY, manifest['sourceUrl'])
        self.assertFalse(manifest['binaryRebuildReproducible'])
        self.assertFalse(manifest['publicRedistributionApproved'])
        self.assertEqual(manifest['supportedProfileMinApi'], 35)
        self.assertEqual(manifest['referenceProfileEnvironment'], {'TU_DEBUG': 'sysmem,nobin'})
        meta = json.loads((assets / 'meta.json').read_text(encoding='utf8'))
        self.assertEqual(meta, dict(name='Turnip — Adreno 830', version='Mesa 26.3.0-devel (Unleashed)', libraryName=LIBRARY, minApi=35))
        for name in ('PROVENANCE.md', 'reference-driver-README.md', 'mesa-license.rst', 'mesa-MIT.txt', 'u_gralloc_fallback.LICENSE.txt', '0010-gralloc-qcom-ubwc-detect.patch'):
            self.assertTrue((assets / 'notices' / name).is_file(), name)
        self.assertIn('Google Inc.', (assets / 'notices/u_gralloc_fallback.LICENSE.txt').read_text(encoding='utf8'))
        self.assertIn('Permission is hereby granted', (assets / 'notices/mesa-MIT.txt').read_text())
        for path in (assets / 'notices').iterdir():
            self.assertNotIn('<!doctype html>', path.read_text(encoding='utf8').lower())
        self.assertFalse(list(SOURCE.rglob('*.so')), 'Binary must remain generated/ignored')
        gradle = (ROOT / 'android/app/build.gradle').read_text()
        self.assertIn("assets.srcDir '../third_party/gpu_driver'", gradle)
        self.assertIn("assets.srcDir layout.buildDirectory.dir('generated/turnipAssets')", gradle)
        self.assertIn("tasks.named('preBuild').configure { dependsOn tasks.named('generateTurnipBundleAssets') }", gradle)

    def test_task_hash_bound_atomic_publication_and_offline_cache(self):
        gradle = os.environ.get('WT_GRADLE') or shutil.which('gradle')
        if not gradle and Path('D:/gradle-8.11.1/bin/gradle.bat').exists():
            gradle = 'D:/gradle-8.11.1/bin/gradle.bat'
        self.assertIsNotNone(gradle, 'Set WT_GRADLE to Gradle 8.11.1')
        env = os.environ.copy()
        env.pop('WT_TURNIP_DRIVER_FILE', None)
        if Path('D:/jdk-21.0.6+7').exists():
            env['JAVA_HOME'] = 'D:/jdk-21.0.6+7'
            env['PATH'] = env['JAVA_HOME'] + '/bin' + os.pathsep + env['PATH']
        preferred = Path('C:/Users/HomePC/AppData/Local/Temp/opencode')
        with tempfile.TemporaryDirectory(prefix='wt-turnip-task-', dir=preferred if preferred.exists() else None) as directory:
            work = Path(directory)
            fixture = work / 'source'
            shutil.copytree(SOURCE, fixture)
            payload = b'fixture driver bytes, not executable'
            manifest_path = fixture / 'assets' / PROFILE / 'bundle-manifest.json'
            manifest = json.loads(manifest_path.read_text())
            manifest.update(sha256=hashlib.sha256(payload).hexdigest(), sizeBytes=len(payload))
            manifest_path.write_text(json.dumps(manifest))
            (work / 'settings.gradle').write_text("rootProject.name='turnip-task-test'\n")
            (work / 'build.gradle').write_text("ext.wtTurnipBundleSource = file('source')\napply from: 'source/bundle.gradle'\n")
            local = work / 'input.so'
            local.write_bytes(payload)
            destination = work / 'build/generated/turnipAssets' / PROFILE / LIBRARY

            def run(label, use_input, expected):
                command = [gradle, '--offline', '--no-daemon', '--console=plain', 'generateTurnipBundleAssets']
                if use_input:
                    command.append('-PwtTurnipDriverFile=' + str(local))
                result = subprocess.run(command, cwd=work, env=env, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode == 0, expected, label + '\n' + result.stdout + result.stderr)
                self.assertFalse(list(destination.parent.glob('.turnip-*.tmp')), label)

            run('verified local input', True, True)
            self.assertEqual(destination.read_bytes(), payload)
            self.assertEqual(json.loads((destination.parent / 'meta.json').read_text(encoding='utf8'))['libraryName'], LIBRARY)
            self.assertTrue((destination.parent / 'notices/mesa-MIT.txt').is_file())
            run('offline verified cache', False, True)
            local.write_bytes(b'x' * len(payload))
            run('same-size bad hash rejected', True, False)
            self.assertEqual(destination.read_bytes(), payload, 'failed input must not replace valid cache')
            local.write_bytes(payload + b'x')
            run('oversized input rejected', True, False)
            self.assertEqual(destination.read_bytes(), payload)
            destination.write_bytes(b'x' * len(payload))
            run('bad cached hash rejected offline', False, False)


if __name__ == '__main__':
    unittest.main()
