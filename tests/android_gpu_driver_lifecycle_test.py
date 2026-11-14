"""Execute real coordinator/helper Java with host Android/Binder stubs, not device IPC."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PKG = 'org/witchtime/android/'
STUBS = {
 'android/os/RemoteException.java': 'package android.os; public class RemoteException extends Exception {}',
 'android/os/IBinder.java': '''package android.os; public interface IBinder {
 int FIRST_CALL_TRANSACTION=1; interface DeathRecipient { void binderDied(); }
 boolean transact(int c,Parcel p,Parcel r,int f) throws RemoteException;
 void linkToDeath(DeathRecipient d,int f) throws RemoteException; boolean isBinderAlive(); }''',
 'android/os/Binder.java': '''package android.os; public class Binder implements IBinder {
 boolean alive=true; java.util.List<DeathRecipient> deaths=new java.util.ArrayList<>();
 public static int getCallingUid(){return 1;}
 protected boolean onTransact(int c,Parcel p,Parcel r,int f){return false;}
 public boolean transact(int c,Parcel p,Parcel r,int f)throws RemoteException {
 if(!alive)throw new RemoteException(); return onTransact(c,p,r,f); }
 public void linkToDeath(DeathRecipient d,int f)throws RemoteException {if(!alive)throw new RemoteException();deaths.add(d);}
 public boolean isBinderAlive(){return alive;} public void die(){alive=false;for(DeathRecipient d:deaths)d.binderDied();} }''',
 'android/os/Parcel.java': '''package android.os; public class Parcel {
 java.util.List<Object> a=new java.util.ArrayList<>(); int i;
 public static Parcel obtain(){return new Parcel();} public void recycle(){}
 public void writeString(String s){a.add(s);} public String readString(){return (String)a.get(i++);}
 public void writeInt(int n){a.add(n);}public int readInt(){return (Integer)a.get(i++);}
 public void writeStrongBinder(IBinder b){a.add(b);}public IBinder readStrongBinder(){return (IBinder)a.get(i++);} }''',
 'android/os/Process.java': 'package android.os; public class Process {public static volatile int killed;public static int myUid(){return 1;}public static int myPid(){return 42;}public static void killProcess(int pid){killed=pid;}}',
 'android/os/Bundle.java': '''package android.os; public class Bundle extends java.util.HashMap<String,Object>{
 public void putBinder(String k,IBinder b){put(k,b);}public IBinder getBinder(String k){return (IBinder)get(k);} }''',
 'android/os/Looper.java': 'package android.os; public class Looper {public static Looper getMainLooper(){return new Looper();}}',
 'android/os/Handler.java': '''package android.os; public class Handler {
 public static Runnable timeout; public Handler(Looper l){} public void postDelayed(Runnable r,long t){timeout=r;} }''',
 'android/content/Intent.java': '''package android.content; public class Intent {
 android.os.Bundle b=new android.os.Bundle(); public Intent(){}public Intent(Object a,Class<?> c){}
 public Intent putExtras(android.os.Bundle x){b.putAll(x);return this;}
 public Intent putExtra(String k,String v){b.put(k,v);return this;}
 public String getStringExtra(String k){return (String)b.get(k);}public android.os.Bundle getExtras(){return b;} }''',
 'android/app/Dialog.java': 'package android.app; public class Dialog {public boolean dismissed;public void dismiss(){dismissed=true;}}',
 'android/app/Application.java': 'package android.app; public class Application {public static String process="org.witchtime.android:restart";public static String getProcessName(){return process;}}',
 'android/app/Activity.java': '''package android.app; public class Activity {
 public volatile android.content.Intent launched;public android.content.Intent intent;public volatile boolean finished;
 public void onCreate(android.os.Bundle b){}protected void onResume(){}protected void onPause(){}protected void onDestroy(){}
 public String getPackageName(){return "org.witchtime.android";}
 public boolean isFinishing(){return finished;}public boolean isChangingConfigurations(){return false;}
 public android.content.Intent getIntent(){return intent;}public void finish(){finished=true;}
 public void runOnUiThread(Runnable r){r.run();}public void startActivity(android.content.Intent i){launched=i;}
 public void startActivityForResult(android.content.Intent i,int n){launched=i;} }''',
 PKG+'GpuDriverActivity.java': 'package org.witchtime.android; public class GpuDriverActivity {public static final String EXTRA_RESTART_NONCE="nonce";}',
 PKG+'LauncherActivity.java': 'package org.witchtime.android; public class LauncherActivity {}',
 PKG+'GpuDriverRestartUi.java': '''package org.witchtime.android; public class GpuDriverRestartUi {
 static android.app.Dialog latest;static String message;
 public static android.app.Dialog showProgress(android.app.Activity a,String s){message=s;latest=new android.app.Dialog();return latest;}
 public static void showError(android.app.Activity a,String s){} }''',
 PKG+'GpuDriverStore.java': '''package org.witchtime.android; public class GpuDriverStore {
 static volatile String sealed;static boolean fail,failDiscard;static volatile int discarded;
 static java.util.concurrent.CountDownLatch sealGate,sealEntered;
 public GpuDriverStore(Object a){}public void discardRestart(String n)throws java.io.IOException{if(failDiscard)throw new java.io.IOException();discarded++;sealed=null;}
 public void sealRestart(String n)throws java.io.IOException{if(sealEntered!=null)sealEntered.countDown();WitchTimeActivity.await(sealGate);sealed=n;if(fail)throw new java.io.IOException();}
 public String sealedRestartNonce(){return sealed;} }''',
 PKG+'WitchTimeActivity.java': '''package org.witchtime.android; public class WitchTimeActivity extends android.app.Activity {
 GpuDriverRestartCoordinator c;boolean drain=true,failAfterSeal;volatile boolean lease;volatile int cancelled,commits,prepares;
 static java.util.concurrent.CountDownLatch cancelGate,prepareGate,commitGate;
 boolean nativePrepareDriverRestart(String n){prepares++;await(prepareGate);lease=true;return drain;}
 static void await(java.util.concurrent.CountDownLatch gate){try{if(gate!=null)gate.await();}catch(InterruptedException e){throw new RuntimeException(e);}}
 void nativeCancelDriverRestart(String n){if(GpuDriverStore.discarded==0)throw new AssertionError("Release before durable discard");cancelled++;await(cancelGate);lease=false;}
 boolean nativeCommitDriverRestart(String n){commits++;await(commitGate);return c.seal(n)&&!failAfterSeal;} }''',
 PKG+'Harness.java': '''package org.witchtime.android;
 import android.os.*;
 public class Harness {
 static void check(boolean b){if(!b)throw new AssertionError();}
 static void waitFor(java.util.function.BooleanSupplier s)throws Exception {
 long end=System.nanoTime()+2000000000L;while(!s.getAsBoolean()&&System.nanoTime()<end)Thread.sleep(1);check(s.getAsBoolean());}
 static boolean done(GpuDriverRestartActivity h){try{java.lang.reflect.Field f=GpuDriverRestartActivity.class.getDeclaredField("protocolDone");f.setAccessible(true);return f.getBoolean(h);}catch(Exception e){throw new RuntimeException(e);}}
 public static void main(String[] args)throws Exception {run(args);System.exit(0);}
 static void run(String[] args)throws Exception {
 WitchTimeActivity a=new WitchTimeActivity();a.c=new GpuDriverRestartCoordinator(a);a.c.resumed();
 if(args[0].equals("cancel-before-prepare")){
 java.lang.reflect.Field f=GpuDriverRestartCoordinator.class.getDeclaredField("worker");f.setAccessible(true);
 java.util.concurrent.ExecutorService executor=(java.util.concurrent.ExecutorService)f.get(a.c);
 java.util.concurrent.CountDownLatch entered=new java.util.concurrent.CountDownLatch(1),release=new java.util.concurrent.CountDownLatch(1);
 executor.execute(()->{entered.countDown();WitchTimeActivity.await(release);});entered.await();
 a.c.beginStaged("one");a.c.paused();check(a.prepares==0&&a.cancelled==0);check(!a.c.seal("one"));
 release.countDown();waitFor(()->a.cancelled==1&&!a.lease);check(a.prepares==1);check(a.commits==0);check(a.launched==null);return;
 }
 if(args[0].startsWith("blocked-")){
 WitchTimeActivity.cancelGate=new java.util.concurrent.CountDownLatch(1);
 WitchTimeActivity.prepareGate=new java.util.concurrent.CountDownLatch(1);
 a.c.beginStaged("one");waitFor(()->a.prepares==1);
 try {
 long start=System.nanoTime();
 if(args[0].equals("blocked-pause"))a.c.paused();else Handler.timeout.run();
 check(System.nanoTime()-start<500000000L);check(a.cancelled==0);
 WitchTimeActivity.prepareGate.countDown();waitFor(()->a.cancelled==1);
 final boolean[] ran={false};a.runOnUiThread(()->ran[0]=true);check(ran[0]);
 a.c.resumed();check(a.c.isResumed());check(!a.c.seal("one"));
 check(GpuDriverStore.discarded==1);a.c.beginStaged("one");check(a.prepares==1);
 check(GpuDriverRestartUi.message.startsWith("Cancelling"));check(!GpuDriverRestartUi.latest.dismissed);
 } finally {WitchTimeActivity.cancelGate.countDown();WitchTimeActivity.prepareGate.countDown();}
 waitFor(()->!a.lease);return;
 }
 if(args[0].equals("not-resumed")){
 a.c.paused();a.c.beginStaged("one");waitFor(()->a.cancelled==1&&GpuDriverStore.discarded==1);check(a.prepares==0);return;
 }
 if(args[0].equals("drain-error"))a.drain=false;
 a.c.beginStaged("one");
 if(args[0].equals("drain-error")){waitFor(()->a.cancelled==1);check(a.launched==null);return;}
 waitFor(()->a.launched!=null);
 if(args[0].equals("timeout")){Handler.timeout.run();waitFor(()->a.cancelled==1);check(!a.c.seal("one"));return;}
 check(!a.c.seal("stale"));
 GpuDriverRestartActivity h=new GpuDriverRestartActivity();h.intent=a.launched;h.onCreate(null);
 java.lang.reflect.Field witnessField=GpuDriverRestartActivity.class.getDeclaredField("witness");witnessField.setAccessible(true);
 Parcel early=Parcel.obtain(),earlyReply=Parcel.obtain();early.writeString("one");
 check(((IBinder)witnessField.get(h)).transact(GpuDriverRestartCoordinator.CHECK,early,earlyReply,0));check(earlyReply.readInt()==0);
 if(args[0].equals("parent-hung-commit")||args[0].equals("parent-hung-seal")){
 boolean seal=args[0].equals("parent-hung-seal");
 if(seal){GpuDriverStore.fail=true;GpuDriverStore.sealGate=new java.util.concurrent.CountDownLatch(1);GpuDriverStore.sealEntered=new java.util.concurrent.CountDownLatch(1);}
 else WitchTimeActivity.commitGate=new java.util.concurrent.CountDownLatch(1);
 h.onResume();
 if(seal)GpuDriverStore.sealEntered.await();else waitFor(()->a.commits==1);
 long start=System.nanoTime();a.c.paused();Handler.timeout.run();a.c.resumed();check(a.c.isResumed());
 check(System.nanoTime()-start<500000000L);
 check(GpuDriverStore.discarded==0&&a.cancelled==0&&a.lease);check(!a.c.seal("one"));
 final boolean[] runnable={false};a.runOnUiThread(()->runnable[0]=true);check(runnable[0]);
 check(GpuDriverRestartUi.message.startsWith("Cancelling"));
 if(seal)GpuDriverStore.sealGate.countDown();else WitchTimeActivity.commitGate.countDown();
 waitFor(()->done(h)&&a.cancelled==1&&!a.lease);
 check(GpuDriverStore.discarded==1&&GpuDriverStore.sealed==null);check(((android.app.Activity)h).launched==null);return;
 }
 if(args[0].equals("hung-ready")||args[0].equals("destroy-during-ipc")){
 WitchTimeActivity.commitGate=new java.util.concurrent.CountDownLatch(1);h.onResume();waitFor(()->a.commits==1);
 long start=System.nanoTime();h.onPause();check(System.nanoTime()-start<500000000L);
 check(((android.app.Activity)h).launched==null);check(!done(h));
 if(args[0].equals("destroy-during-ipc")){h.onDestroy();check(android.os.Process.killed==0);}
 WitchTimeActivity.commitGate.countDown();waitFor(()->done(h)&&a.cancelled==1&&!a.lease);
 check(GpuDriverStore.sealed==null);check(((android.app.Activity)h).launched==null);
 waitFor(()->h.finished);h.onDestroy();check(android.os.Process.killed==42);return;
 }
 if(args[0].equals("hung-seal")){
 GpuDriverStore.sealGate=new java.util.concurrent.CountDownLatch(1);GpuDriverStore.sealEntered=new java.util.concurrent.CountDownLatch(1);
 h.onResume();GpuDriverStore.sealEntered.await();
 long start=System.nanoTime();h.onPause();check(System.nanoTime()-start<500000000L);check(!done(h));
 java.lang.reflect.Field f=GpuDriverRestartActivity.class.getDeclaredField("witness");f.setAccessible(true);
 IBinder witness=(IBinder)f.get(h);Parcel p=Parcel.obtain(),r=Parcel.obtain();p.writeString("one");
 check(witness.transact(GpuDriverRestartCoordinator.CHECK,p,r,0));check(r.readInt()==0);
 GpuDriverStore.sealGate.countDown();waitFor(()->done(h));check("one".equals(GpuDriverStore.sealed));
 ((Binder)a.launched.getExtras().getBinder(GpuDriverRestartCoordinator.EXTRA_PARENT)).die();
 check(((android.app.Activity)h).launched==null);h.onResume();waitFor(()->((android.app.Activity)h).launched!=null);
 h.onDestroy();check(android.os.Process.killed==42);return;
 }
 if(args[0].equals("ready-loss")){h.onPause();h.onResume();waitFor(()->a.cancelled==1&&done(h)&&h.finished);check(a.commits==0);check(GpuDriverStore.sealed==null);h.onDestroy();check(android.os.Process.killed==42);return;}
 if(args[0].equals("unsealed-death")){
 ((Binder)a.launched.getExtras().getBinder(GpuDriverRestartCoordinator.EXTRA_PARENT)).die();
 h.onResume();check(((android.app.Activity)h).launched==null);check(h.finished);return;}
 if(args[0].equals("seal-error"))GpuDriverStore.fail=true;
 if(args[0].equals("native-post-seal-error"))a.failAfterSeal=true;
 if(args[0].equals("discard-error")){GpuDriverStore.fail=true;GpuDriverStore.failDiscard=true;}
 h.onResume();
 if(args[0].equals("native-post-seal-error")){waitFor(()->done(h)&&a.cancelled==1&&!a.lease);check(GpuDriverStore.sealed==null&&GpuDriverStore.discarded==1);check(((android.app.Activity)h).launched==null);return;}
 if(args[0].equals("discard-error")){
 waitFor(()->a.commits==1&&GpuDriverStore.sealed!=null);
 java.lang.reflect.Field f=GpuDriverRestartCoordinator.class.getDeclaredField("worker");f.setAccessible(true);
 ((java.util.concurrent.ExecutorService)f.get(a.c)).submit(()->{}).get();
 check(a.cancelled==0&&a.lease);check(GpuDriverStore.discarded==0);check(!a.c.seal("one"));
 a.c.beginStaged("one");check(a.prepares==1);check(((android.app.Activity)h).launched==null);return;
 }
 if(args[0].equals("seal-error")){waitFor(()->a.cancelled==1);check(GpuDriverStore.sealed==null);check(((android.app.Activity)h).launched==null);return;}
 waitFor(()->GpuDriverStore.sealed!=null);check(a.commits==1);check(!a.c.seal("one"));
 Binder parent=(Binder)a.launched.getExtras().getBinder(GpuDriverRestartCoordinator.EXTRA_PARENT);
 Parcel stale=Parcel.obtain(), reply=Parcel.obtain();stale.writeString("stale");
 check(!parent.transact(GpuDriverRestartCoordinator.READY,stale,reply,0));
 Parcel duplicate=Parcel.obtain();duplicate.writeString("one");
 check(!parent.transact(GpuDriverRestartCoordinator.READY,duplicate,reply,0));check(a.commits==1);
 if(args[0].equals("background"))h.onPause();
 parent.die();
 android.app.Activity view=h;
 if(args[0].equals("background")){check(view.launched==null);h.onResume();}
 waitFor(()->view.launched!=null);check("one".equals(view.launched.getStringExtra(GpuDriverActivity.EXTRA_RESTART_NONCE)));
 android.content.Intent first=view.launched;h.onResume();check(view.launched==first);
 waitFor(()->done(h));
 if(args[0].equals("not-helper-process"))android.app.Application.process="org.witchtime.android";
 h.onDestroy();check(android.os.Process.killed==(args[0].equals("not-helper-process")?0:42));
 }
 }''',
}


class LifecycleTest(unittest.TestCase):
    def test_actual_java_lifecycle(self):
        self.assertIsNotNone(shutil.which('javac'), 'JDK required')
        approved = pathlib.Path.home() / 'AppData/Local/Temp/opencode'
        with tempfile.TemporaryDirectory(prefix='gpu-lifecycle-', dir=approved) as tmp:
            root = pathlib.Path(tmp)
            for name, text in STUBS.items():
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text)
            for name in ('GpuDriverRestartCoordinator.java', 'GpuDriverRestartActivity.java'):
                shutil.copy(ROOT / 'android/app/src/main/java' / PKG / name, root / PKG / name)
            subprocess.run(['javac', '-d', str(root), *map(str, root.rglob('*.java'))], check=True)
            for case in ('ready-seal-death', 'background', 'seal-error', 'drain-error', 'timeout', 'ready-loss', 'unsealed-death', 'blocked-pause', 'blocked-deadline', 'not-resumed', 'cancel-before-prepare', 'discard-error', 'hung-ready', 'hung-seal', 'destroy-during-ipc', 'not-helper-process', 'parent-hung-commit', 'parent-hung-seal', 'native-post-seal-error'):
                with self.subTest(case=case):
                    subprocess.run(['java', '-cp', str(root), 'org.witchtime.android.Harness', case], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
