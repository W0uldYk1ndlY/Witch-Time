package org.witchtime.android;

import android.os.Bundle;
import android.util.Log;
import android.view.WindowManager;
import org.libsdl.app.SDLActivity;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Set;
import java.util.regex.Pattern;

/** SDL owns the native thread and surface lifecycle. No storage permission is needed. */
public final class WitchTimeActivity extends SDLActivity {
    private static final int GPU_DRIVER_REQUEST = 0x4750;
    private final GpuDriverRestartCoordinator driverRestart = new GpuDriverRestartCoordinator(this);
    private String stagedDriverNonce;
    native boolean nativePrepareDriverRestart(String nonce);
    native void nativeCancelDriverRestart(String nonce);
    native boolean nativeCommitDriverRestart(String nonce);
    private native void nativeGpuDriverChooserCancelled();

    public void showGpuDriverSettings() {
        runOnUiThread(() -> {
            if (!driverRestart.isResumed()) { nativeGpuDriverChooserCancelled(); return; }
            try { startActivityForResult(new android.content.Intent(this, GpuDriverActivity.class), GPU_DRIVER_REQUEST); }
            catch (RuntimeException e) { nativeGpuDriverChooserCancelled(); }
        });
    }
    public boolean sealGpuDriverRestart(String nonce) { return driverRestart.seal(nonce); }
    @Override protected void onActivityResult(int request, int result, android.content.Intent data) {
        super.onActivityResult(request, result, data);
        if (request != GPU_DRIVER_REQUEST) return;
        stagedDriverNonce = result == RESULT_OK && data != null
                ? data.getStringExtra(GpuDriverActivity.EXTRA_RESTART_NONCE) : null;
        if (stagedDriverNonce == null) nativeGpuDriverChooserCancelled();
    }
    @Override protected void onResume() {
        super.onResume(); driverRestart.resumed();
        if (stagedDriverNonce != null) {
            String nonce = stagedDriverNonce; stagedDriverNonce = null;
            driverRestart.beginStaged(nonce);
        }
    }
    @Override protected void onPause() { driverRestart.paused(); super.onPause(); }

    public String[] prepareGpuDriver() {
        return new GpuDriverStore(this).prepare();
    }

    private static final String TAG = "WitchTimeDiag";
    private static final int MAX_REQUEST_SIZE = 65536;
    private static final Pattern RUN_ID_PATTERN = Pattern.compile("^[a-zA-Z0-9_-]{1,64}$");

    private static final Set<String> ALLOWED_TOP_KEYS = new HashSet<>(Arrays.asList(
        "schema_version", "run_id", "created_utc", "build", "scenario",
        "profile", "configuration", "policy", "treatments"
    ));

    private static final Set<String> ALLOWED_BUILD_KEYS = new HashSet<>(Arrays.asList(
        "variant", "target", "native_optimization", "git_commit", "build_id"
    ));

    private static final Set<String> ALLOWED_SCENARIO_KEYS = new HashSet<>(Arrays.asList(
        "scenario_id", "route_id", "fixture_id", "save_fixture", "replay_fixture", "deadlines"
    ));

    private static final Set<String> ALLOWED_DEADLINE_KEYS = new HashSet<>(Arrays.asList(
        "startup_deadline_secs", "scenario_deadline_secs", "recovery_deadline_secs"
    ));

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setShowWhenLocked(true);
        setTurnScreenOn(true);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    public void loadLibraries() {
        if (!SaveExportActivity.beginGameLibraryLoad())
            throw new UnsatisfiedLinkError("Committed save export is active; SDL startup refused.");
        super.loadLibraries();
    }

    @Override
    protected String[] getArguments() {
        // Only an explicitly enabled diagnostic build accepts automated launch configuration.
        // Ordinary release/debug builds ignore any diagnostic intent extras.
        if (!BuildConfig.WT_DIAGNOSTIC_ENABLED) {
            return new String[0];
        }

        if (getIntent() == null) {
            return new String[0];
        }

        String runId = getIntent().getStringExtra("diag_run_id");
        if (runId == null || runId.trim().isEmpty()) {
            runId = getIntent().getStringExtra("run_id");
        }
        if (runId == null || runId.trim().isEmpty()) {
            return new String[0];
        }

        runId = runId.trim();
        if (!RUN_ID_PATTERN.matcher(runId).matches()) {
            Log.e(TAG, "Rejected malformed run_id: " + runId);
            return new String[0];
        }

        File extDir = getExternalFilesDir(null);
        File rootDir = (extDir != null) ? extDir : getFilesDir();
        File diagRoot = new File(rootDir, "diagnostics");
        File runDir = new File(diagRoot, runId);

        try {
            String diagRootPath = diagRoot.getCanonicalPath();
            String runDirPath = runDir.getCanonicalPath();
            if (!runDirPath.startsWith(diagRootPath + File.separator)) {
                Log.e(TAG, "Path traversal attempt rejected for run_id: " + runId);
                return new String[0];
            }
        } catch (IOException e) {
            Log.e(TAG, "Failed to resolve canonical path for run_id: " + runId, e);
            return new String[0];
        }

        File reqFile = new File(runDir, "request.json");
        if (!reqFile.exists() || !reqFile.isFile()) {
            Log.e(TAG, "Missing request.json in " + runDir.getAbsolutePath());
            return new String[0];
        }

        long fileLen = reqFile.length();
        if (fileLen <= 0 || fileLen > MAX_REQUEST_SIZE) {
            Log.e(TAG, "request.json size out of bounds (" + fileLen + " bytes)");
            return new String[0];
        }

        byte[] reqBytes = new byte[(int) fileLen];
        try (FileInputStream fis = new FileInputStream(reqFile)) {
            int read = 0;
            while (read < reqBytes.length) {
                int r = fis.read(reqBytes, read, reqBytes.length - read);
                if (r < 0) break;
                read += r;
            }
            if (read != reqBytes.length) {
                Log.e(TAG, "Incomplete read of request.json");
                return new String[0];
            }
        } catch (IOException e) {
            Log.e(TAG, "I/O error reading request.json", e);
            return new String[0];
        }

        String digestHex;
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            byte[] hash = md.digest(reqBytes);
            StringBuilder sb = new StringBuilder(64);
            for (byte b : hash) {
                sb.append(String.format("%02x", b));
            }
            digestHex = sb.toString();
        } catch (Exception e) {
            Log.e(TAG, "SHA-256 digest failed", e);
            return new String[0];
        }

        String reqJsonStr = new String(reqBytes, java.nio.charset.StandardCharsets.UTF_8);
        try {
            JSONObject root = new JSONObject(reqJsonStr);

            // Allowlisted top-level keys
            Iterator<String> keys = root.keys();
            while (keys.hasNext()) {
                String k = keys.next();
                if (!ALLOWED_TOP_KEYS.contains(k)) {
                    Log.e(TAG, "Unknown top-level key rejected: " + k);
                    return new String[0];
                }
            }

            // Schema version: major must be 1
            String schemaVersion = root.getString("schema_version");
            if (!schemaVersion.startsWith("1.")) {
                Log.e(TAG, "Unsupported schema version major: " + schemaVersion);
                return new String[0];
            }

            // Run ID match
            String reqRunId = root.getString("run_id");
            if (!runId.equals(reqRunId)) {
                Log.e(TAG, "run_id mismatch: intent=" + runId + ", request=" + reqRunId);
                return new String[0];
            }

            // Build allowlist
            JSONObject build = root.getJSONObject("build");
            Iterator<String> bKeys = build.keys();
            while (bKeys.hasNext()) {
                String k = bKeys.next();
                if (!ALLOWED_BUILD_KEYS.contains(k)) {
                    Log.e(TAG, "Unknown build key rejected: " + k);
                    return new String[0];
                }
            }
            if (!"diagnostic".equals(build.getString("variant"))) {
                Log.e(TAG, "Build variant must be diagnostic");
                return new String[0];
            }
            if (!"android-arm64".equals(build.getString("target"))) {
                Log.e(TAG, "Build target must be android-arm64");
                return new String[0];
            }
            if (!"release".equals(build.getString("native_optimization"))) {
                Log.e(TAG, "Native optimization must be release");
                return new String[0];
            }

            // Scenario allowlist
            JSONObject scenario = root.getJSONObject("scenario");
            Iterator<String> sKeys = scenario.keys();
            while (sKeys.hasNext()) {
                String k = sKeys.next();
                if (!ALLOWED_SCENARIO_KEYS.contains(k)) {
                    Log.e(TAG, "Unknown scenario key rejected: " + k);
                    return new String[0];
                }
            }
            if (!RUN_ID_PATTERN.matcher(scenario.getString("scenario_id")).matches() ||
                !RUN_ID_PATTERN.matcher(scenario.getString("route_id")).matches() ||
                !RUN_ID_PATTERN.matcher(scenario.getString("fixture_id")).matches()) {
                Log.e(TAG, "Malformed scenario identifiers");
                return new String[0];
            }

            // Deadlines
            JSONObject deadlines = scenario.getJSONObject("deadlines");
            Iterator<String> dKeys = deadlines.keys();
            while (dKeys.hasNext()) {
                String k = dKeys.next();
                if (!ALLOWED_DEADLINE_KEYS.contains(k)) {
                    Log.e(TAG, "Unknown deadline key: " + k);
                    return new String[0];
                }
                double val = deadlines.getDouble(k);
                if (Double.isNaN(val) || Double.isInfinite(val) || val <= 0.0) {
                    Log.e(TAG, "Invalid deadline value for " + k + ": " + val);
                    return new String[0];
                }
            }

        } catch (JSONException e) {
            Log.e(TAG, "Malformed request JSON", e);
            return new String[0];
        }

        Log.i(TAG, "Diagnostic request verified for run " + runId + ", digest " + digestHex);
        return new String[] { "--diag-run-id", runId, "--diag-digest", digestHex };
    }
}
