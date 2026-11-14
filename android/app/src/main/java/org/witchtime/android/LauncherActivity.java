package org.witchtime.android;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import android.graphics.Color;
import android.widget.Toast;
import android.widget.Button;
import android.widget.LinearLayout;
import android.app.AlertDialog;
import android.content.Intent;
import java.io.IOException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Ordinary app entry: the graphics-driver chooser lives in PC Settings, not at startup. */
public final class LauncherActivity extends Activity {
    private static boolean gameStartedInProcess;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private GpuDriverStore store;
    private LinearLayout recoveryView;
    private TextView recoveryStatus;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (gameStartedInProcess || SaveExportActivity.gameLibrariesRequested()) {
            TextView message = new TextView(this);
            message.setText("Witch Time is already open. Return to the running game to change its graphics driver.");
            message.setTextSize(18); message.setTextColor(Color.rgb(32, 30, 27));
            message.setPadding(dp(24), dp(32), dp(24), dp(24)); setContentView(message); return;
        }
        store = new GpuDriverStore(this);
        Intent entry = getIntent();
        boolean hasRestartToken = entry != null && entry.hasExtra(GpuDriverActivity.EXTRA_RESTART_NONCE);
        String nonce = null;
        try { if (hasRestartToken) nonce = entry.getStringExtra(GpuDriverActivity.EXTRA_RESTART_NONCE); }
        catch (RuntimeException e) { showRecoveryError(store.sealedRestartNonce()); return; }
        if (hasRestartToken) {
            try {
                if (nonce == null || !store.consumeRestart(nonce)) { showRecoveryError(store.sealedRestartNonce()); return; }
            } catch (IOException e) { showRecoveryError(store.sealedRestartNonce()); return; }
        } else if (store.sealedRestartNonce() != null) {
            // A sealed request is consumed only from the helper's verified nonce handoff.
            showRecoveryError(store.sealedRestartNonce()); return;
        }
        if (store.hasRecoveryPending())
            Toast.makeText(this, "A previous driver start was quarantined. Witch Time will use the system driver; retry from PC Settings.", Toast.LENGTH_LONG).show();
        gameStartedInProcess = true;
        startActivity(new Intent(this, WitchTimeActivity.class));
        finish();
    }

    private void showRecoveryError(String sealedNonce) {
        recoveryView = new LinearLayout(this); recoveryView.setOrientation(LinearLayout.VERTICAL);
        recoveryView.setPadding(dp(24), dp(32), dp(24), dp(24)); recoveryView.setBackgroundColor(Color.rgb(247, 245, 239));
        TextView message = new TextView(this);
        message.setText(sealedNonce != null
                ? "The graphics-driver restart could not be verified, so the game was not started. You can explicitly cancel the pending change and continue with the current driver selection."
                : "The graphics-driver restart could not be verified, so the game was not started. No sealed pending change is available to cancel. Nothing was changed.");
        message.setTextSize(18); message.setTextColor(Color.rgb(32, 30, 27)); recoveryView.addView(message);
        recoveryStatus = new TextView(this); recoveryStatus.setTextSize(14); recoveryStatus.setTextColor(Color.rgb(32, 30, 27));
        LinearLayout.LayoutParams statusParams = new LinearLayout.LayoutParams(-1, -2); statusParams.topMargin = dp(12); recoveryView.addView(recoveryStatus, statusParams);
        if (sealedNonce != null) {
            Button recover = new Button(this); recover.setText("Cancel pending driver change"); recover.setMinHeight(dp(52));
            recover.setOnClickListener(v -> new AlertDialog.Builder(this)
                    .setMessage("This cancels only the pending restart. The game will use its current driver selection; a quarantined driver still falls back to System. Continue?")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Cancel change", (d, w) -> cancelPendingRestart(sealedNonce, recover)).show());
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2); params.topMargin = dp(20); recoveryView.addView(recover, params);
        }
        setContentView(recoveryView);
    }

    private void cancelPendingRestart(String nonce, Button button) {
        button.setEnabled(false);
        worker.execute(() -> {
            String error = null;
            try {
                store.discardRestart(nonce);
                if (store.sealedRestartNonce() != null) throw new IOException("A pending restart is still present");
            }
            catch (IOException e) { error = e.getMessage(); }
            final String failure = error;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (failure != null) { button.setEnabled(true); recoveryStatus.setText("Could not cancel the pending change: " + failure + ". No driver was changed. Try again."); return; }
                gameStartedInProcess = true;
                startActivity(new Intent(this, WitchTimeActivity.class));
                finish();
            });
        });
    }

    private int dp(int n) { return (int)(n * getResources().getDisplayMetrics().density + .5f); }
    @Override protected void onDestroy() { worker.shutdown(); super.onDestroy(); }
}
