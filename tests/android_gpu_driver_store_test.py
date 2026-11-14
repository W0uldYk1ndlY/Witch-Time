"""Host execution of the real store, with Android filesystem/context and flat JSON stubs.

No device, native loading, Android JSON implementation, or actual fsync semantics tested.
"""
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'android/app/src/main/java/org/witchtime/android/GpuDriverStore.java'
STUBS = {
    'android/content/Context.java': '''package android.content;
public class Context {
 private final java.io.File root;
 public Context(java.io.File root) { this.root=root; }
 public java.io.File getFilesDir() { return root; }
  public android.content.pm.ApplicationInfo getApplicationInfo() { return new android.content.pm.ApplicationInfo(); }
 public android.content.res.AssetManager getAssets() { return new android.content.res.AssetManager(new java.io.File(root,"assets")); }
 }''',
    'android/content/res/AssetManager.java': '''package android.content.res;
public class AssetManager {
 private final java.io.File root;
 public AssetManager(java.io.File root) { this.root=root; }
 public java.io.InputStream open(String path) throws java.io.IOException { return new java.io.FileInputStream(new java.io.File(root,path)); }
}''',
    'android/content/pm/ApplicationInfo.java': '''package android.content.pm;
public class ApplicationInfo { public String nativeLibraryDir="/native"; }''',
    'android/content/Intent.java': '''package android.content;
public class Intent { public boolean hasExtra(String key) { return true; } }''',
    'android/app/Activity.java': '''package android.app;
public class Activity extends android.content.Context {
 public Activity(java.io.File root) { super(root); }
 public android.content.Intent getIntent() { return new android.content.Intent(); }
}''',
    'android/os/Build.java': '''package android.os;
public class Build { public static class VERSION { public static int SDK_INT=35; } }''',
    'android/system/OsConstants.java': '''package android.system;
public class OsConstants { public static int O_RDONLY=0; }''',
    'android/system/Os.java': '''package android.system;
public class Os {
 public static boolean failSync;
 public static void rename(String a,String b) throws java.io.IOException {
  java.nio.file.Files.move(java.nio.file.Path.of(a),java.nio.file.Path.of(b),java.nio.file.StandardCopyOption.REPLACE_EXISTING,java.nio.file.StandardCopyOption.ATOMIC_MOVE);
 }
 public static java.io.FileDescriptor open(String p,int f,int m) { return new java.io.FileDescriptor(); }
 public static void fsync(java.io.FileDescriptor f) throws java.io.IOException {
  if(failSync) throw new java.io.IOException("Injected sync failure");
 }
 public static void close(java.io.FileDescriptor f) {}
}''',
    'org/json/JSONObject.java': r'''package org.json;
public class JSONObject {
 private final java.util.Map<String,Object> values = new java.util.HashMap<>();
 public JSONObject(String text) throws Exception {
  String s=text.trim();
  if(!s.startsWith("{") || !s.endsWith("}")) throw new Exception("JSON");
  s=s.substring(1,s.length()-1).trim();
  if(s.isEmpty()) return;
    java.util.regex.Matcher fields=java.util.regex.Pattern.compile("\\\"([A-Za-z0-9]+)\\\"\\s*:\\s*(\\\"[^\\\"]*\\\"|-?[0-9]+(?:\\.[0-9]+)?|true|false|null|\\[\\]|\\{\\})").matcher(s);
   while(fields.find()) {
    String key=fields.group(1), v=fields.group(2);
    Object value=v.startsWith("\"") && v.endsWith("\"") ? v.substring(1,v.length()-1)
      : v.matches("-?[0-9]+(?:\\.[0-9]+)?") ? Double.valueOf(v) : new Object();
   values.put(key,value);
  }
 }
 public Object get(String k) throws Exception { if(!values.containsKey(k)) throw new Exception(k); return values.get(k); }
 public boolean has(String k) { return values.containsKey(k); }
}''',
    'Harness.java': '''import org.witchtime.android.GpuDriverStore;
import java.io.*;
public class Harness {
 public static void main(String[] args) throws Exception {
  File root=new File(args[1]); root.mkdirs();
  GpuDriverStore store=new GpuDriverStore(args[0].equals("diag") ? new android.app.Activity(root) : new android.content.Context(root));
  switch(args[0]) {
   case "import":
    try(FileInputStream in=new FileInputStream(args[2])) {
     GpuDriverStore.Driver d=store.importZip(in);
     if(!store.selectedId().equals("system")) throw new AssertionError("Import selected driver");
     store.select(d.id); System.out.print(d.id);
    } break;
   case "reject":
    String before=store.selectedId(); int count=store.listDrivers().size();
    try(FileInputStream in=new FileInputStream(args[2])) {
     try { store.importZip(in); throw new AssertionError("Accepted invalid ZIP"); }
     catch(IOException expected) {}
    }
    if(!before.equals(store.selectedId()) || count!=store.listDrivers().size()) throw new AssertionError("Changed state");
    break;
    case "prepare":
    String[] p=store.prepare();
     if(p==null || p.length!=5 || !p[4].isEmpty() || !p[1].endsWith(File.separator) || !new File(p[3]).isFile()) throw new AssertionError("Snapshot");
    try { store.select("system"); throw new AssertionError("Not frozen"); } catch(IOException expected) {}
    if(store.prepare()!=null) throw new AssertionError("Second attempt");
     System.out.print(p[3]); break;
    case "version":
     java.util.List<GpuDriverStore.Driver> drivers=store.listDrivers();
     if(drivers.size()!=1 || !drivers.get(0).version.equals(args[2])) throw new AssertionError("Listed version");
     break;
   case "quarantine":
    if(store.recoveryMessage().isEmpty() || store.prepare()!=null) throw new AssertionError("Not quarantined"); break;
   case "retry": store.select(store.selectedId());
    if(!store.recoveryMessage().isEmpty()) throw new AssertionError("Retry did not clear"); break;
   case "system": store.select("system"); if(store.prepare()!=null) throw new AssertionError(); break;
   case "diag": if(store.prepare()!=null) throw new AssertionError("Diagnostic custom driver"); break;
   case "write-failure": android.system.Os.failSync=true;
    if(store.prepare()!=null) throw new AssertionError("Returned unpersisted snapshot"); break;
   case "concurrent":
    String selected=store.selectedId();
    java.util.concurrent.CountDownLatch entered=new java.util.concurrent.CountDownLatch(1);
    java.util.concurrent.CountDownLatch release=new java.util.concurrent.CountDownLatch(1);
    java.util.concurrent.atomic.AtomicReference<Throwable> outcome=new java.util.concurrent.atomic.AtomicReference<>();
    Thread importer=new Thread(() -> {
     try(InputStream in=new FilterInputStream(open(args[2])) {
      @Override public int read(byte[] b,int off,int len) throws IOException {
       entered.countDown();
       try { release.await(); } catch(InterruptedException e) { throw new IOException(e); }
       return super.read(b,off,len);
      }
     }) {
       store.importZip(in);
     } catch(Throwable e) { outcome.set(e); }
    });
    importer.setDaemon(true); importer.start();
    java.util.concurrent.ExecutorService ui=java.util.concurrent.Executors.newSingleThreadExecutor(r -> {
     Thread t=new Thread(r); t.setDaemon(true); return t;
    });
    try {
     if(!entered.await(5,java.util.concurrent.TimeUnit.SECONDS)) throw new AssertionError("Provider not entered");
     ui.submit(() -> {
      if(store.listDrivers().size()!=1 || !store.selectedId().equals(selected) || !store.recoveryMessage().isEmpty())
       throw new AssertionError("Read exposed staging or changed selection");
      String[] snapshot=store.prepare();
      if(snapshot==null || !snapshot[1].endsWith(selected+File.separator)) throw new AssertionError("Previous driver unavailable");
     }).get(5,java.util.concurrent.TimeUnit.SECONDS);
    } finally { release.countDown(); ui.shutdownNow(); }
    importer.join(5000);
     if(importer.isAlive() || outcome.get()!=null) throw new AssertionError("Immutable next-launch import failed",outcome.get());
     if(!store.selectedId().equals(selected) || store.listDrivers().size()!=2) throw new AssertionError("Live selection changed");
    File[] staging=new File(root,"gpu-drivers").listFiles((dir,name) -> name.startsWith(".stage-"));
    if(staging==null || staging.length!=0) throw new AssertionError("Staging not cleaned");
     break;
    case "bundle":
     String previous=store.selectedId();
     GpuDriverStore.Driver bundled=store.ensureBundledDriver();
     if(!bundled.id.equals(GpuDriverStore.BUNDLED_ID) || !store.selectedId().equals(previous)) throw new AssertionError("Provision selected driver");
     if(store.listDrivers().stream().noneMatch(d -> d.id.equals(bundled.id))) throw new AssertionError("Missing bundle choice");
     store.select(bundled.id);
     String[] snapshot=store.prepare();
     if(snapshot==null || snapshot.length!=5 || !snapshot[4].equals(GpuDriverStore.BUNDLED_ID)) throw new AssertionError("Bundled profile snapshot");
     break;
    case "bundle-frozen":
     if(store.prepare()!=null) throw new AssertionError("Expected system");
     store.ensureBundledDriver();
     if(!store.selectedId().equals("system")) throw new AssertionError("Live selection changed");
     try { store.select(GpuDriverStore.BUNDLED_ID); throw new AssertionError("Unfrozen select"); } catch(IOException expected) {}
     break;
    case "bundle-reject":
     try { store.ensureBundledDriver(); throw new AssertionError("Accepted corrupt bundle"); } catch(IOException expected) {}
     break;
    case "stage": case "live-stage":
     String active=store.selectedId();
     String[] live=args[0].equals("live-stage") ? store.prepare() : null;
     String token=store.stageRestart(args[2],Boolean.parseBoolean(args[3]));
     try { store.stageRestart(args[2],true); throw new AssertionError("Duplicate stage"); } catch(IOException expected) {}
     try { store.sealRestart("00000000-0000-0000-0000-000000000000"); throw new AssertionError("Stale seal"); } catch(IOException expected) {}
     try { store.discardRestart("00000000-0000-0000-0000-000000000000"); throw new AssertionError("Stale discard claimed success"); } catch(IOException expected) {}
     if(store.sealedRestartNonce()!=null) throw new AssertionError("Unsealed dispatched");
     if(Boolean.parseBoolean(args[4])) {
      store.sealRestart(token);
      if(!token.equals(store.sealedRestartNonce())) throw new AssertionError("Seal not visible");
      try { store.sealRestart(token); throw new AssertionError("Duplicate seal"); } catch(IOException expected) {}
     }
     if(live==null) { if(store.consumeRestart(token)) throw new AssertionError("Creator consumed"); }
     else {
      try { store.consumeRestart(token); throw new AssertionError("Live consume"); } catch(IOException expected) {}
      if(!new File(live[3]).isFile()) throw new AssertionError("Old marker cleared");
     }
     if(!store.selectedId().equals(active)) throw new AssertionError("Stage changed active");
     System.out.print(token);
     break;
    case "stage-reject":
     try { store.stageRestart(args[2],false); throw new AssertionError("Implicit quarantine retry"); } catch(IOException expected) {}
     break;
    case "consume":
     boolean consumed=store.consumeRestart(args[2]);
     if(consumed!=Boolean.parseBoolean(args[3])) throw new AssertionError("Consume result");
     if(store.consumeRestart(args[2])) throw new AssertionError("Duplicate consume");
     System.out.print(store.selectedId()); break;
    case "consume-race": System.out.print(store.consumeRestart(args[2])); break;
    case "consume-reject":
     try { store.consumeRestart(args[2]); throw new AssertionError("Accepted invalid package/state"); } catch(IOException expected) {}
     break;
    case "discard": store.discardRestart(args[2]); break;
    case "discard-stale":
     try { store.discardRestart(args[2]); throw new AssertionError("Stale discard claimed success"); } catch(IOException expected) {}
     break;
    case "seal-discard-fsync":
     String unchanged=store.selectedId();
     String pending=store.stageRestart("system",false);
     android.system.Os.failSync=true;
     try { store.sealRestart(pending); throw new AssertionError("Seal directory sync failure hidden"); } catch(IOException expected) {}
     if(!pending.equals(store.sealedRestartNonce())) throw new AssertionError("Expected rename-before-fsync SEALED state");
     try { store.discardRestart(pending); throw new AssertionError("Discard directory sync failure hidden"); } catch(IOException expected) {}
     if(new File(root,"gpu-drivers/restart").exists()) throw new AssertionError("Expected delete-before-fsync absence");
     try { store.discardRestart(pending); throw new AssertionError("Absent retry skipped directory fsync"); } catch(IOException expected) {}
     android.system.Os.failSync=false;
     store.discardRestart(pending);
     if(store.sealedRestartNonce()!=null || !unchanged.equals(store.selectedId())) throw new AssertionError("Discard changed selection or left sealed token");
     break;
    case "recovery":
     if(store.hasRecoveryPending()!=Boolean.parseBoolean(args[2])) throw new AssertionError("Recovery flag");
     break;
  }
 }
 private static InputStream open(String path) throws IOException { return new FileInputStream(path); }
}''',
}


class StoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='android_gpu_driver_store_')
        cls.base = pathlib.Path(cls.temp.name)
        sources = []
        for name, text in STUBS.items():
            path = cls.base / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
            sources.append(str(path))
        subprocess.run(['javac', '-d', str(cls.base), str(SOURCE), *sources], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.work = pathlib.Path(tempfile.mkdtemp(dir=self.base))
        self.files = self.work / 'files'

    def run_java(self, mode, archive=None, *extra):
        result = subprocess.run(['java', '-cp', str(self.base), 'Harness', mode,
                                 str(self.files), *([str(archive)] if archive is not None else []), *map(str, extra)],
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def archive(self, entries=None, meta=None, omit=()):
        elf = bytearray(64)
        elf[:7] = b'\x7fELF\x02\x01\x01'
        elf[16:20] = b'\x03\x00\xb7\x00'
        metadata = dict(name='Driver', version='1', libraryName='driver.so', minApi=28)
        metadata.update(meta or {})
        for key in omit:
            metadata.pop(key, None)
        path = self.work / 'driver.zip'
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as out:
            for name, data in (entries if entries is not None else [
                ('meta.json', json.dumps(metadata)), ('driver.so', elf)]):
                out.writestr(name, data)
        return path

    def test_package_version_string_survives_import_list_and_prepare(self):
        self.run_java('import', self.archive(meta=dict(packageVersion='34'), omit=('version',)))
        self.run_java('version', '34')
        self.run_java('prepare')

    def test_version_takes_precedence_over_package_version(self):
        self.run_java('import', self.archive(meta=dict(version='primary', packageVersion='34')))
        self.run_java('version', 'primary')
        self.run_java('prepare')

    def test_invalid_version_is_not_masked_by_package_version(self):
        for version in ['', '   ', 34, True, None]:
            with self.subTest(version=version):
                self.run_java('reject', self.archive(meta=dict(version=version, packageVersion='34')))

    def test_reject_invalid_or_missing_package_version_when_version_absent(self):
        for version in ['', '   ', 34, True, None, [], {}]:
            with self.subTest(packageVersion=version):
                self.run_java('reject', self.archive(meta=dict(packageVersion=version), omit=('version',)))
        self.run_java('reject', self.archive(omit=('version',)))

    def test_import_recovery_retry_unique_markers_and_readonly_bytes(self):
        path = self.archive()
        ident = self.run_java('import', path)
        library = self.files / 'gpu-drivers' / ident / 'driver.so'
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(library.read_bytes(), archive.read('driver.so'))
        first = self.run_java('prepare')
        self.run_java('quarantine')
        self.run_java('retry')
        second = self.run_java('prepare')
        self.assertNotEqual(first, second)
        self.assertFalse(pathlib.Path(first).exists())
        pathlib.Path(first).unlink(missing_ok=True)  # stale native callback cannot delete second
        self.assertTrue(pathlib.Path(second).exists())
        self.run_java('system')
        self.assertTrue(pathlib.Path(second).exists())

    def test_diagnostics_never_arms_marker(self):
        self.run_java('import', self.archive())
        self.run_java('diag')
        self.assertFalse(list((self.files / 'gpu-drivers').glob('pending-*')))

    def test_blocked_provider_does_not_block_reads_and_only_immutable_next_launch_import_after_freeze(self):
        archive = self.archive()
        ident = self.run_java('import', archive)
        library = self.files / 'gpu-drivers' / ident / 'driver.so'
        before = library.read_bytes()
        self.run_java('concurrent', archive)
        self.assertEqual(library.read_bytes(), before)
        self.assertEqual((self.files / 'gpu-drivers' / 'selected').read_text(), ident)
        self.assertEqual(len(list((self.files / 'gpu-drivers').glob('pkg-*'))), 2)
        self.assertFalse(list((self.files / 'gpu-drivers').glob('.stage-*')))

    def test_reject_bad_architecture_metadata_paths_duplicates_and_truncation(self):
        for meta in [dict(minApi=99), dict(libraryName='../driver.so'), dict(libraryName='missing.so'), dict(name='')]:
            with self.subTest(meta=meta): self.run_java('reject', self.archive(meta=meta))
        for entries in [[('../escape', b'x')], [('/absolute', b'x')], [('a', b'x'), ('a', b'y')],
                        [('meta.json', b'{}'), ('driver.so', b'not elf')], [('meta.json', b'x' * 16385)]]:
            with self.subTest(entries=str(entries)[:80]): self.run_java('reject', self.archive(entries=entries))
        path = self.archive()
        path.write_bytes(path.read_bytes()[:-8])
        self.run_java('reject', path)

    def test_reject_symlink_and_crc_corruption(self):
        path = self.archive()
        with zipfile.ZipFile(path, 'a') as out:
            link = zipfile.ZipInfo('link.so')
            link.create_system = 3
            link.external_attr = 0o120777 << 16
            out.writestr(link, 'driver.so')
        self.run_java('reject', path)
        path = self.archive()
        data = bytearray(path.read_bytes())
        central = data.index(b'PK\x01\x02')
        data[central + 16] ^= 1
        path.write_bytes(data)
        self.run_java('reject', path)

    def test_source_limit_architecture_and_write_failure(self):
        path = self.archive()
        with zipfile.ZipFile(path) as archive:
            meta = archive.read('meta.json')
            elf = bytearray(archive.read('driver.so'))
        for index, value in [(4, 1), (5, 2), (16, 2), (18, 62)]:
            changed = bytearray(elf)
            changed[index] = value
            self.run_java('reject', self.archive(entries=[('meta.json', meta), ('driver.so', changed)]))
        large = self.work / 'source-too-large.zip'
        with large.open('wb') as stream:
            for _ in range(65): stream.write(b'\0' * (1 << 20))
        self.run_java('reject', large)
        self.run_java('import', self.archive())
        self.run_java('write-failure')
        self.run_java('quarantine')

    def test_reject_count_and_stream_expansion_limit(self):
        self.run_java('reject', self.archive(entries=[(f'f{i}', b'') for i in range(129)]))
        # Compressible 129 MiB source exercises streamed per-file limit without large host allocation.
        path = self.work / 'large.zip'
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as out:
            with out.open('huge.so', 'w') as stream:
                for _ in range(129): stream.write(b'\0' * (1 << 20))
        self.run_java('reject', path)

    def test_live_stage_does_not_change_selection_and_fresh_consume_is_once_only(self):
        ident = self.run_java('import', self.archive())
        token = self.run_java('live-stage', 'system', 'false', 'true')
        store = self.files / 'gpu-drivers'
        marker = next(store.glob('pending-*'))
        self.assertEqual((store / 'selected').read_text(), ident)
        self.assertEqual(self.run_java('consume', '00000000-0000-0000-0000-000000000000', 'false'), ident)
        self.assertEqual(self.run_java('consume', token, 'true'), 'system')
        self.assertTrue(marker.exists(), 'System switch must not delete old quarantine')
        self.assertEqual((store / 'selected').read_text().splitlines(), ['system', token])
        self.assertEqual(self.run_java('consume', token, 'false'), 'system')
        self.assertFalse((store / 'restart').exists())

    def test_unsealed_cancellation_and_wrong_nonce_do_not_select(self):
        ident = self.run_java('import', self.archive())
        token = self.run_java('stage', 'system', 'false', 'false')
        self.assertEqual(self.run_java('consume', token, 'false'), ident)
        self.run_java('discard-stale', '00000000-0000-0000-0000-000000000000')
        self.assertTrue((self.files / 'gpu-drivers/restart').exists())
        self.run_java('discard', token)
        self.assertFalse((self.files / 'gpu-drivers/restart').exists())
        self.assertEqual(self.run_java('consume', token, 'false'), ident)

    def test_abandoned_unsealed_request_can_be_replaced_but_stale_token_cannot_cancel_new_one(self):
        ident = self.run_java('import', self.archive())
        first = self.run_java('stage', 'system', 'false', 'false')
        second = self.run_java('stage', 'system', 'false', 'true')
        self.assertNotEqual(first, second)
        self.run_java('discard-stale', first)
        self.assertEqual(self.run_java('consume', first, 'false'), ident)
        self.assertEqual(self.run_java('consume', second, 'true'), 'system')

    def test_retry_consent_applied_only_in_fresh_consume_never_replayed(self):
        ident = self.run_java('import', self.archive())
        first = pathlib.Path(self.run_java('prepare'))
        self.run_java('recovery', 'true')
        self.run_java('stage-reject', ident)
        token = self.run_java('stage', ident, 'true', 'true')
        self.assertTrue(first.exists(), 'Stage/seal must not clear live quarantine')
        self.assertEqual(self.run_java('consume', token, 'true'), ident)
        self.assertFalse(first.exists())
        self.run_java('recovery', 'false')
        second = pathlib.Path(self.run_java('prepare'))
        self.run_java('consume', token, 'false')
        self.assertTrue(second.exists(), 'Duplicate consumption must never replay retry consent')
        # Simulate death after atomic selected+nonce commit but before request retirement.
        request = self.files / 'gpu-drivers/restart'
        request.write_text('SEALED\n' + token + '\n00000000-0000-0000-0000-000000000001\n' + ident + '\ntrue')
        self.run_java('consume', token, 'false')
        self.assertTrue(second.exists())

    def test_sealed_consume_revalidates_package_and_rejects_corrupt_state(self):
        ident = self.run_java('import', self.archive())
        token = self.run_java('stage', ident, 'false', 'true')
        library = self.files / 'gpu-drivers' / ident / 'driver.so'
        library.chmod(0o600)
        library.write_bytes(b'corrupt')
        self.run_java('consume-reject', token)
        self.assertEqual((self.files / 'gpu-drivers/selected').read_text(), ident)
        (self.files / 'gpu-drivers/restart').write_text('SEALED\ncorrupt')
        self.run_java('consume-reject', token)

    def test_cross_process_consumers_commit_once_under_file_lock(self):
        self.run_java('import', self.archive())
        token = self.run_java('stage', 'system', 'false', 'true')
        command = ['java', '-cp', str(self.base), 'Harness', 'consume-race', str(self.files), token]
        processes = [subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(2)]
        results = []
        for process in processes:
            out, err = process.communicate(timeout=20)
            self.assertEqual(process.returncode, 0, err)
            results.append(out)
        self.assertEqual(sorted(results), ['false', 'true'])
        self.assertEqual((self.files / 'gpu-drivers/selected').read_text().splitlines(), ['system', token])

    def test_seal_and_discard_directory_sync_failure_remains_observable_until_durable_retry(self):
        self.run_java('seal-discard-fsync')
        self.assertFalse((self.files / 'gpu-drivers/restart').exists())

    def bundle_assets(self, real=False):
        profile = pathlib.Path('bundled_driver/adreno830')
        source = ROOT / 'android/third_party/turnip_adreno830/assets' / profile
        destination = self.files / 'assets' / profile
        shutil.copytree(source, destination)
        library = destination / 'vulkan.mesa26_3_gralloc_a8xx.so'
        if real:
            binary = pathlib.Path(os.environ.get('WT_TURNIP_TEST_DRIVER', 'C:/Users/HomePC/AppData/Local/Temp/opencode/vulkan.mesa26_3_gralloc_a8xx.so'))
            if not binary.is_file():
                self.skipTest('Set WT_TURNIP_TEST_DRIVER to the exact approved binary for provisioning success')
            shutil.copyfile(binary, library)
        else:
            header = bytearray(64)
            header[:7] = b'\x7fELF\x02\x01\x01'
            header[16:20] = b'\x03\x00\xb7\x00'
            with library.open('wb') as stream:
                stream.write(header)
                stream.truncate(14495432)
        return destination, library

    def test_bundled_hash_failure_does_not_publish_or_select(self):
        assets, library = self.bundle_assets()
        self.run_java('bundle-reject')
        store = self.files / 'gpu-drivers'
        self.assertFalse((store / 'turnip-adreno830-unleashed-mesa26.3-gralloc').exists())
        self.assertFalse(list(store.glob('.stage-*')))
        self.assertFalse((store / 'selected').exists())
        with library.open('ab') as stream:
            stream.write(b'x')
        self.run_java('bundle-reject')
        self.assertFalse(list(store.glob('.stage-*')))

    def test_exact_bundle_provisioning_while_frozen_and_verified_profile(self):
        self.bundle_assets(real=True)
        self.run_java('bundle-frozen')
        store = self.files / 'gpu-drivers'
        bundled = store / 'turnip-adreno830-unleashed-mesa26.3-gralloc'
        self.assertTrue(bundled.is_dir())
        self.assertFalse((store / 'selected').exists())
        self.run_java('bundle')
        library = bundled / 'vulkan.mesa26_3_gralloc_a8xx.so'
        library.chmod(0o600)
        with library.open('r+b') as stream:
            stream.seek(100)
            value = stream.read(1)
            stream.seek(100)
            stream.write(bytes([value[0] ^ 1]))
        self.run_java('bundle-reject')
        self.assertTrue(library.exists(), 'Never replace/remove a corrupt published immutable package implicitly')


if __name__ == '__main__':
    unittest.main()
