package org.witchtime.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.os.Bundle;
import android.util.Log;
import java.io.File;
import java.util.List;

/** Deliberately not an SDLActivity: no SDL/native game lifecycle is entered. */
public final class SaveExportActivity extends Activity {
    private static boolean exporting, gameLibrariesRequested;
    static synchronized boolean beginGameLibraryLoad() {
        if (exporting) return false;
        gameLibrariesRequested = true;
        return true;
    }
    private static synchronized boolean beginExport() {
        if (exporting || gameLibrariesRequested) return false;
        exporting = true;
        return true;
    }
    private static synchronized void endExport() { exporting = false; }
    private static native int exportCommittedSave(String root, String runId, String digest);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (!BuildConfig.WT_DIAGNOSTIC_ENABLED || state != null ||
                !"org.witchtime.android.EXPORT_COMMITTED_SAVE".equals(getIntent().getAction())) {
            finish();
            return;
        }
        String runId = getIntent().getStringExtra("diag_run_id");
        String digest = getIntent().getStringExtra("request_sha256");
        File root = getExternalFilesDir(null);
        if (root == null || runId == null || !runId.matches("[a-zA-Z0-9_-]{1,64}") ||
                digest == null || !digest.matches("[0-9a-f]{64}")) {
            finish();
            return;
        }
        ActivityManager manager = (ActivityManager)getSystemService(ACTIVITY_SERVICE);
        List<ActivityManager.RunningAppProcessInfo> processes = manager == null ? null : manager.getRunningAppProcesses();
        boolean foundSelf = false;
        if (processes == null) { finish(); return; }
        for (ActivityManager.RunningAppProcessInfo process : processes) {
            if (process.uid != android.os.Process.myUid()) continue;
            if (process.pid != android.os.Process.myPid()) { finish(); return; }
            foundSelf = true;
        }
        if (!foundSelf || !beginExport()) { finish(); return; }
        // The library has no SDL, game or runtime dependency and rechecks loaded mappings.
        try { System.loadLibrary("save_export"); }
        catch (UnsatisfiedLinkError error) { endExport(); Log.e("WitchTimeSaveExport", "Export unavailable"); finish(); return; }
        new Thread(() -> {
            try {
                int result = exportCommittedSave(root.getAbsolutePath(), runId, digest);
                Log.i("WitchTimeSaveExport", result == 0 ? "Export complete" : "Export refused or incomplete");
            } finally {
                endExport();
                runOnUiThread(this::finish);
            }
        }, "committed-save-export").start();
    }
}
