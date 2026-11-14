package org.witchtime.android;

import android.app.Activity;
import android.app.Application;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/** Visible :restart process; no SDL loading and no parent-killing capability. */
public final class GpuDriverRestartActivity extends Activity {
    private static final int WAITING = -1, OFFERED = 0, REVOKED = 1, COMMIT_GRANTED = 2;
    private final AtomicInteger admission = new AtomicInteger(WAITING);
    private final AtomicBoolean sent = new AtomicBoolean();
    private volatile boolean resumed;
    private volatile boolean dead, protocolDone;
    private boolean launched, destroyed;
    private String nonce;
    private IBinder parent;
    private final IBinder witness = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (Binder.getCallingUid() != android.os.Process.myUid() || code != GpuDriverRestartCoordinator.CHECK) return false;
            // This CAS is the irrevocable commit point; lifecycle revocation races it, never fsync.
            reply.writeInt(nonce.equals(data.readString()) &&
                    admission.compareAndSet(OFFERED, COMMIT_GRANTED) ? 1 : 0);
            return true;
        }
    };
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        nonce = getIntent().getStringExtra(GpuDriverActivity.EXTRA_RESTART_NONCE);
        Bundle extras = getIntent().getExtras();
        parent = extras == null ? null : extras.getBinder(GpuDriverRestartCoordinator.EXTRA_PARENT);
        if (nonce == null || parent == null) { protocolDone = true; finish(); return; }
        GpuDriverRestartUi.showProgress(this, "Restarting with the selected graphics driver…");
        try { parent.linkToDeath(() -> { dead = true; runOnUiThread(this::launch); }, 0); }
        catch (RemoteException e) { dead = true; }
    }
    @Override protected void onResume() {
        super.onResume();
        resumed = true;
        admission.compareAndSet(WAITING, OFFERED);
        if (parent == null) return;
        if (dead) { if (!sent.get()) protocolDone = true; launch(); return; }
        beginProtocol();
    }
    private void beginProtocol() {
        if (!sent.compareAndSet(false, true)) return;
        new Thread(() -> {
            boolean committed = false;
            if (admission.get() == OFFERED && !dead) committed = send(GpuDriverRestartCoordinator.READY);
            if (!committed && parent.isBinderAlive()) send(GpuDriverRestartCoordinator.CANCEL);
            protocolDone = true;
            final boolean accepted = committed;
            runOnUiThread(() -> {
                if (!parent.isBinderAlive()) { dead = true; launch(); }
                else if (!accepted) {
                    GpuDriverRestartUi.showError(this, "Restart was cancelled. Return to the game and try again.");
                    finish();
                }
                stopEmptyHelper();
            });
        }, "gpu-restart-ready").start();
    }
    @Override protected void onPause() {
        admission.compareAndSet(OFFERED, REVOKED);
        admission.compareAndSet(WAITING, REVOKED);
        resumed = false;
        if (!dead && parent != null) beginProtocol();
        super.onPause();
    }
    private boolean send(int code) {
        Parcel request = Parcel.obtain(), response = Parcel.obtain();
        try {
            request.writeString(nonce);
            if (code == GpuDriverRestartCoordinator.READY) request.writeStrongBinder(witness);
            return parent.transact(code, request, response, 0) && response.readInt() == 1;
        } catch (RemoteException | RuntimeException e) { return false; }
        finally { request.recycle(); response.recycle(); }
    }
    private void launch() {
            if (!resumed || !dead || launched || destroyed) return;
            try {
                if (!nonce.equals(new GpuDriverStore(this).sealedRestartNonce())) { finish(); return; }
                Intent intent = new Intent(this, LauncherActivity.class);
                intent.putExtra(GpuDriverActivity.EXTRA_RESTART_NONCE, nonce);
                startActivity(intent);
                launched = true;
                finish();
            } catch (Exception e) { GpuDriverRestartUi.showError(this, "Could not restart the game."); }
    }
    @Override protected void onDestroy() {
        admission.compareAndSet(OFFERED, REVOKED);
        resumed = false;
        destroyed = true;
        super.onDestroy();
        stopEmptyHelper();
    }
    private void stopEmptyHelper() {
        // Only our empty :restart process may be retired, and never with IPC still outstanding.
        if (destroyed && isFinishing() && !isChangingConfigurations() && protocolDone &&
                (getPackageName() + ":restart").equals(Application.getProcessName())) {
            int self = android.os.Process.myPid();
            if (self > 0) android.os.Process.killProcess(self);
        }
    }
}
