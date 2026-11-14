package org.witchtime.android;

import android.app.Dialog;
import android.content.Intent;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Owns one restart; the helper Binder supplies the foreground lease. */
final class GpuDriverRestartCoordinator {
    static final String EXTRA_PARENT = "gpu_restart_parent";
    static final int READY = IBinder.FIRST_CALL_TRANSACTION, CANCEL = READY + 1, CHECK = READY + 2;
    private final WitchTimeActivity activity;
    private String nonce;
    private IBinder helper;
    private boolean resumed, handedOff, sealed, cancelling, committing, cancelRequested, sealing;
    private Dialog progress;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    GpuDriverRestartCoordinator(WitchTimeActivity activity) { this.activity = activity; }
    private final Binder parent = new Binder() {
        @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) {
            if (Binder.getCallingUid() != android.os.Process.myUid()) return false;
            String token = data.readString();
            IBinder witness;
            synchronized (GpuDriverRestartCoordinator.this) {
                if (nonce == null || !nonce.equals(token) || sealed || cancelling) return false;
                if (code == CANCEL) { cancel(); reply.writeInt(1); return true; }
                if (code != READY || !handedOff || helper != null || committing) return false;
                helper = data.readStrongBinder();
                witness = helper;
                committing = true;
            }
            boolean committed = false;
            try {
                witness.linkToDeath(() -> {
                    synchronized (GpuDriverRestartCoordinator.this) {
                        if (token.equals(nonce)) cancel();
                    }
                }, 0);
                committed = activity.nativeCommitDriverRestart(token);
            } catch (RemoteException | RuntimeException e) { /* Roll back only after commit has returned. */ }
            synchronized (GpuDriverRestartCoordinator.this) {
                committing = false;
                if (!committed) { sealed = false; cancel(); }
            }
            reply.writeInt(committed ? 1 : 0);
            return true;
        }
    };
    synchronized void resumed() { resumed = true; }
    synchronized boolean isResumed() { return resumed; }
    synchronized void paused() { resumed = false; if (nonce != null && !handedOff) cancel(); }
    synchronized void beginStaged(String token) {
        if (token == null) return;
        if (token.equals(nonce)) return;
        if (nonce != null) {
            new Thread(() -> {
                try { new GpuDriverStore(activity).discardRestart(token); } catch (IOException ignored) { }
            }, "gpu-restart-reject").start();
            return;
        }
        nonce = token;
        cancelRequested = false; sealing = false;
        handedOff = false; helper = null;
        if (!resumed) { cancel(); return; }
        new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(() -> {
            synchronized (GpuDriverRestartCoordinator.this) {
                if (token.equals(nonce) && !sealed) cancel();
            }
        }, 15000);
        progress = GpuDriverRestartUi.showProgress(activity, "Preparing graphics driver restart…");
        worker.execute(() -> {
            boolean ready = false;
            try { ready = activity.nativePrepareDriverRestart(token); }
            catch (RuntimeException e) { /* Failed drain never authorizes exit. */ }
            final boolean drained = ready;
            activity.runOnUiThread(() -> {
                synchronized (GpuDriverRestartCoordinator.this) {
                    if (!token.equals(nonce) || cancelling) {
                        // FIFO cleanup is queued/completed for stale tokens; a live token must request it.
                        if (token.equals(nonce)) cancel();
                        return;
                    }
                    if (!drained || !resumed) { cancel(); return; }
                    Bundle extras = new Bundle(); extras.putBinder(EXTRA_PARENT, parent);
                    Intent intent = new Intent(activity, GpuDriverRestartActivity.class).putExtras(extras);
                    intent.putExtra(GpuDriverActivity.EXTRA_RESTART_NONCE, token);
                    handedOff = true;
                    try { activity.startActivity(intent); } catch (RuntimeException e) { cancel(); }
                }
            });
        });
    }
    boolean seal(String token) {
        IBinder witness;
        synchronized (this) {
            if (sealed || sealing || cancelling || cancelRequested || !committing || nonce == null ||
                    !nonce.equals(token) || !handedOff || helper == null) return false;
            sealing = true;
            witness = helper;
        }
        Parcel request = Parcel.obtain(), response = Parcel.obtain();
        try {
            request.writeString(token);
            if (!witness.transact(CHECK, request, response, 0) || response.readInt() != 1) return false;
            new GpuDriverStore(activity).sealRestart(token);
            synchronized (this) { sealed = true; }
            return true;
        } catch (IOException | RemoteException | RuntimeException e) { return false; }
        finally { request.recycle(); response.recycle(); }
    }
    synchronized void cancel() {
        if (nonce == null || sealed || cancelling) return;
        String token = nonce;
        cancelRequested = true;
        activity.runOnUiThread(() -> {
            synchronized (GpuDriverRestartCoordinator.this) {
                if (progress != null) progress.dismiss();
                progress = GpuDriverRestartUi.showProgress(activity,
                        "Cancelling restart; waiting for graphics work to finish…");
            }
        });
        // A granted commit owns the lease until native commit returns; never race its seal with rollback.
        if (committing) return;
        cancelling = true;
        // FIFO prevents cancellation from finishing before a delayed prepare acquires its lease.
        worker.execute(() -> {
            try {
                new GpuDriverStore(activity).discardRestart(token);
                activity.nativeCancelDriverRestart(token);
            } catch (IOException | RuntimeException e) {
                activity.runOnUiThread(() -> GpuDriverRestartUi.showError(activity,
                        "Restart cancellation could not complete. Another restart is unavailable."));
                return;
            }
            activity.runOnUiThread(() -> {
                synchronized (GpuDriverRestartCoordinator.this) {
                    nonce = null; cancelling = false;
                    if (progress != null) { progress.dismiss(); progress = null; }
                }
            });
        });
    }
}
