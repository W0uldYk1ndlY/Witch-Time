"""Source lifecycle checks everywhere; native trust-boundary fixtures on a POSIX host.

Run: python tests/save_export_test.py
No Android build, device, production directory or additional dependency is used.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / 'android/app/src/main/cpp'
JAVA = ROOT / 'android/app/src/main/java/org/witchtime/android'


class Wiring(unittest.TestCase):
    def test_actual_activity_gate_with_mocked_android(self):
        javac, java = shutil.which('javac'), shutil.which('java')
        if not javac or not java:
            self.skipTest('host JDK unavailable')
        stubs = {
            'android/os/Bundle.java': 'package android.os; public class Bundle {}',
            'android/os/Process.java': 'package android.os; public class Process { public static int myUid(){return 1;} public static int myPid(){return 2;} }',
            'android/content/Intent.java': 'package android.content; public class Intent { public String getAction(){return null;} public String getStringExtra(String key){return null;} }',
            'android/util/Log.java': 'package android.util; public class Log { public static int e(String t,String m){return 0;} public static int i(String t,String m){return 0;} }',
            'android/app/ActivityManager.java': 'package android.app; import java.util.List; public class ActivityManager { public static class RunningAppProcessInfo { public int uid,pid; } public List<RunningAppProcessInfo> getRunningAppProcesses(){return null;} }',
            'android/app/Activity.java': '''package android.app;
                import android.os.Bundle; import android.content.Intent; import java.io.File;
                public class Activity {
                    public static final String ACTIVITY_SERVICE="activity";
                    public boolean finished;
                    public void onCreate(Bundle state){}
                    public Intent getIntent(){return new Intent();}
                    public void finish(){finished=true;}
                    public File getExternalFilesDir(String type){throw new AssertionError("invalid action accessed storage");}
                    public Object getSystemService(String key){return null;}
                    public void runOnUiThread(Runnable r){r.run();}
                }''',
            'org/witchtime/android/BuildConfig.java': 'package org.witchtime.android; public class BuildConfig { public static final boolean WT_DIAGNOSTIC_ENABLED=true; }',
            'org/witchtime/android/ExportGateTest.java': '''package org.witchtime.android;
                import java.lang.reflect.Method;
                public class ExportGateTest {
                    public static void main(String[] args) throws Exception {
                        SaveExportActivity rejected=new SaveExportActivity();
                        rejected.onCreate(null);
                        if(!rejected.finished) throw new AssertionError("invalid action did not stop");
                        Method begin=SaveExportActivity.class.getDeclaredMethod("beginExport");
                        Method end=SaveExportActivity.class.getDeclaredMethod("endExport");
                        begin.setAccessible(true); end.setAccessible(true);
                        if(!(Boolean)begin.invoke(null)) throw new AssertionError();
                        if((Boolean)begin.invoke(null)) throw new AssertionError("duplicate export");
                        if(SaveExportActivity.beginGameLibraryLoad()) throw new AssertionError("SDL allowed during export");
                        end.invoke(null);
                        if(!SaveExportActivity.beginGameLibraryLoad()) throw new AssertionError();
                        if((Boolean)begin.invoke(null)) throw new AssertionError("export allowed after game loading");
                    }
                }''',
        }
        with tempfile.TemporaryDirectory(prefix='wt-export-java-') as tmp:
            directory = Path(tmp)
            sources = []
            for name, text in stubs.items():
                path = directory / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(text)
                sources.append(str(path))
            subprocess.run([javac, '-d', str(directory), *sources, str(JAVA / 'SaveExportActivity.java')], check=True)
            subprocess.run([java, '-cp', str(directory), 'org.witchtime.android.ExportGateTest'], check=True)

    def test_bootstrap_typed_authority(self):
        cc = shutil.which('gcc') or shutil.which('cc')
        if not cc:
            self.skipTest('host compiler unavailable')
        with tempfile.TemporaryDirectory(prefix='wt-export-bootstrap-') as tmp:
            exe = Path(tmp) / ('check.exe' if os.name == 'nt' else 'check')
            for diagnostic in (0, 1):
                subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', f'-DWT_DIAGNOSTIC_ENABLED={diagnostic}',
                                '-I', str(CPP), str(ROOT / 'tests/save_export_bootstrap_test.c'),
                                str(CPP / 'diag_bootstrap.c'), '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True)

    def test_no_sdl_export_entry(self):
        activity = (JAVA / 'SaveExportActivity.java').read_text()
        self.assertIn('extends Activity', activity)
        self.assertNotIn('extends SDLActivity', activity)
        self.assertIn('BuildConfig.WT_DIAGNOSTIC_ENABLED', activity)
        self.assertIn('getExternalFilesDir(null)', activity)
        self.assertIn('EXPORT_COMMITTED_SAVE', activity)
        self.assertNotIn('getStringExtra("root', activity)
        cmake = (CPP / 'CMakeLists.txt').read_text()
        self.assertIn('add_library(save_export SHARED save_export.c save_export_jni.c diag_bootstrap.c)', cmake)
        self.assertNotIn('target_link_libraries(save_export', cmake)
        self.assertIn('libSDL3.so', (CPP / 'save_export_jni.c').read_text())
        ordinary = (JAVA / 'WitchTimeActivity.java').read_text()
        gate = ordinary[ordinary.index('public void loadLibraries()'):ordinary.index('protected String[] getArguments()')]
        self.assertLess(gate.index('beginGameLibraryLoad()'), gate.index('super.loadLibraries()'))
        self.assertIn('gameLibrariesRequested', activity)
        self.assertIn('finally {', activity)

    def test_game_entry_fails_closed(self):
        main = (CPP / 'main.c').read_text()
        self.assertLess(main.index('wt_save_export_in_progress'), main.index('SDL_SetHint'))
        self.assertLess(main.index('diag_bootstrap_save_export_requested()'), main.index('diag_bootstrap_apply_session_environment'))
        self.assertLess(main.index('diag_bootstrap_save_export_requested()'), main.index('dlopen("libwitchtime.so"'))

    def test_no_source_permissions_or_runtime(self):
        code = (CPP / 'save_export.c').read_text()
        self.assertNotIn('dlopen(', code)
        self.assertNotIn('chmod(', code.replace('fchmod(', 'descriptor_mode('))
        self.assertNotIn('journal_recover(', code)
        self.assertIn('O_NOFOLLOW', code)
        self.assertIn('O_EXCL', code)
        self.assertIn('diag_bootstrap_validate_request_data(', code)


@unittest.skipUnless(os.name == 'posix' and shutil.which('cc'), 'POSIX cc host required; not a device test')
class Native(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='wt-save-export-test-')
        cls.exe = Path(cls.build.name) / 'export-test'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-DWT_DIAGNOSTIC_ENABLED=1',
                        str(ROOT / 'tests/save_export_driver.c'), str(CPP / 'diag_bootstrap.c'),
                        '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='wt-export-fixture-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.save = self.root / 'userdata/save'
        self.save.mkdir(parents=True)
        (self.save / 'slot.bin').write_bytes(b'private synthetic test only')
        self.run = self.root / 'diagnostics/run_test'
        self.run.mkdir(parents=True)
        self.request = {'schema_version': '1.0', 'run_id': 'run_test',
                        'build': {'variant': 'diagnostic', 'target': 'android-arm64', 'native_optimization': 'release'},
                        'profile': {'profile_name': 'committed_save_export', 'collectors': []}}
        self.stage()

    def stage(self):
        data = json.dumps(self.request).encode()
        (self.run / 'request.json').write_bytes(data)
        self.digest = hashlib.sha256(data).hexdigest()

    def invoke(self, success, run='run_test', digest=None, mutate=False):
        manifest = self.run / 'committed-save-export/manifest.json'
        old_manifest = manifest.read_bytes() if manifest.exists() else None
        env = os.environ.copy()
        if mutate:
            env['WT_EXPORT_TEST_MUTATE'] = '1'
        result = subprocess.run([str(self.exe), str(self.root), run, digest or self.digest], env=env)
        self.assertEqual(result.returncode, 0 if success else 1)
        if old_manifest is not None:
            self.assertEqual(manifest.read_bytes(), old_manifest)
        elif not success and manifest.exists():
            self.assertFalse(json.loads(manifest.read_text())['complete'])

    def test_complete_tree_hashes_and_original_unchanged(self):
        (self.save / 'empty').mkdir()
        (self.save / 'sub').mkdir()
        (self.save / 'sub/quote".bin').write_bytes(bytes(range(256)))
        before = {p.relative_to(self.save): (p.read_bytes(), p.stat().st_mtime_ns, p.stat().st_mode)
                  for p in self.save.rglob('*') if p.is_file()}
        self.invoke(True)
        exported = self.run / 'committed-save-export'
        manifest = json.loads((exported / 'manifest.json').read_text())
        self.assertTrue(manifest['complete'])
        self.assertEqual(manifest['file_count'], 2)
        self.assertTrue((exported / 'save/empty').is_dir())
        for name, (data, mtime, mode) in before.items():
            self.assertEqual((exported / 'save' / name).read_bytes(), data)
            self.assertEqual((self.save / name).read_bytes(), data)
            self.assertEqual((self.save / name).stat().st_mtime_ns, mtime)
            self.assertEqual((self.save / name).stat().st_mode, mode)
        for entry in manifest['entries']:
            if not entry['directory']:
                expected = hashlib.sha256((self.save / entry['path']).read_bytes()).hexdigest()
                self.assertEqual(entry['source_sha256'], expected)
                self.assertEqual(entry['copied_sha256'], expected)
        self.invoke(False)  # Existing destination cannot be overwritten.

    def test_bad_authority(self):
        self.invoke(False, run='../run_test')
        self.invoke(False, digest='0' * 64)
        self.request['profile']['profile_name'] = 'lightweight'
        self.stage()
        self.invoke(False)
        self.request['profile']['profile_name'] = 'committed_save_export'
        self.request['profile']['collectors'] = ['events']
        self.stage()
        self.invoke(False)

    def test_symlinks_and_special_files(self):
        for kind in ('file', 'directory', 'fifo', 'hardlink'):
            p = self.save / 'bad'
            if kind == 'fifo': os.mkfifo(p)
            elif kind == 'hardlink': os.link(self.save / 'slot.bin', p)
            else: p.symlink_to(self.root if kind == 'directory' else self.save / 'slot.bin')
            self.invoke(False)
            p.unlink()
            shutil.rmtree(self.run / 'committed-save-export', ignore_errors=True)

    def test_symlink_ancestors(self):
        real = self.root / 'real'
        self.save.rename(real)
        self.save.symlink_to(real, target_is_directory=True)
        self.invoke(False)

    def test_pending_journal_is_not_recovered(self):
        for name in ('save.commit', 'save.work.123', 'save.journal.123'):
            path = self.root / 'userdata' / name
            path.write_bytes(b'pending')
            self.invoke(False)
            self.assertEqual(path.read_bytes(), b'pending')
            path.unlink()

    def test_limits_and_partial_failure(self):
        with (self.save / 'oversize').open('wb') as f:
            f.truncate(32 * 1024 * 1024 + 1)
        self.invoke(False)
        (self.save / 'oversize').unlink()
        shutil.rmtree(self.run / 'committed-save-export')
        for i in range(1024): (self.save / str(i)).touch()
        self.invoke(False)

    def test_depth_limit(self):
        p = self.save
        for _ in range(17):
            p /= 'sub'
            p.mkdir()
        self.invoke(False)

    def test_total_limit(self):
        for i in range(5):
            with (self.save / str(i)).open('wb') as f:
                f.truncate(32 * 1024 * 1024)
        self.invoke(False)

    def test_source_change_is_incomplete(self):
        self.invoke(False, mutate=True)


if __name__ == '__main__':
    unittest.main()
