package org.witchtime.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.*;
import java.io.IOException;
import java.io.InputStream;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** In-game driver chooser. It stages a next-process request; it never changes the live loader. */
public final class GpuDriverActivity extends Activity {
    public static final String EXTRA_RESTART_NONCE = "org.witchtime.android.extra.GPU_RESTART_NONCE";
    private static final int PICK_ZIP = 51;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private GpuDriverStore store;
    private LinearLayout content;
    private RadioGroup options;
    private TextView status;
    private Button restart;
    private String chosenId = "system", newImportId;
    private boolean busy;
    private final GpuDriverStageOwnership stageOwnership = new GpuDriverStageOwnership();
    private static final String TAG = "WitchTimeGpuDriverUi";

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        store = new GpuDriverStore(this);
        ScrollView scroll = new ScrollView(this);
        content = new LinearLayout(this); content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(24), dp(24), dp(24), dp(24)); content.setBackgroundColor(Color.rgb(247, 245, 239));
        scroll.addView(content); setContentView(scroll);
        TextView title = text("Graphics driver", 28, true); content.addView(title);
        content.addView(text("Choose a driver for the next game session. The running game is not changed.", 16, false), gap(8));
        status = text("Loading driver options…", 14, false); content.addView(status, gap(16));
        options = new RadioGroup(this); options.setOrientation(LinearLayout.VERTICAL); content.addView(options, gap(8));
        Button importButton = button("Import driver ZIP"); importButton.setOnClickListener(v -> confirm(
                "Imported drivers execute with Witch Time's app privileges, including access to game saves. Only import a ZIP from a source you trust.",
                () -> { Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("application/zip").addCategory(Intent.CATEGORY_OPENABLE); startActivityForResult(pick, PICK_ZIP); }));
        content.addView(importButton, gap(8));
        content.addView(text("The selected driver is a request. Android or the game may still fall back to its system driver.", 14, false), gap(12));
        restart = button("Choose and restart"); restart.setOnClickListener(v -> confirmRestart()); content.addView(restart, gap(8));
        Button cancel = button("Cancel"); cancel.setOnClickListener(v -> cancelChooser()); content.addView(cancel, gap(4));
        setBusy(true);
        worker.execute(() -> {
            String error = null;
            try { store.ensureBundledDriver(); } catch (IOException e) { error = e.getMessage(); }
            final String message = error;
            runOnUiThread(() -> { if (isFinishing() || isDestroyed()) return; setBusy(false); renderOptions(); status.setText(message == null ? "" : "Built-in Turnip is unavailable: " + message); });
        });
    }

    private void renderOptions() {
        options.removeAllViews();
        addOption("system", "System default", "Use the graphics driver provided by your device.");
        GpuDriverStore.Driver bundled = null;
        List<GpuDriverStore.Driver> drivers = store.listDrivers();
        try { for (GpuDriverStore.Driver d : drivers) if (GpuDriverStore.BUNDLED_ID.equals(d.id)) bundled = d; }
        catch (RuntimeException ignored) { }
        if (bundled != null) addOption(bundled.id, "Turnip · Adreno 830", "Experimental compatibility profile · " + bundled.version);
        for (GpuDriverStore.Driver d : drivers) {
            if (GpuDriverStore.BUNDLED_ID.equals(d.id)) continue;
            addOption(d.id, d.name + " · " + d.version, "Imported driver");
        }
        String selected = newImportId != null ? newImportId : store.selectedId();
        boolean found = false;
        for (int i = 0; i < options.getChildCount(); i++) {
            RadioButton b = (RadioButton) options.getChildAt(i);
            if (selected.equals(b.getTag())) { b.setChecked(true); found = true; }
        }
        chosenId = found ? selected : "system";
        if (!found) ((RadioButton) options.getChildAt(0)).setChecked(true);
        status.setText(store.hasRecoveryPending() ? "A previous driver start was quarantined. Selecting that driver again is an explicit retry." : "");
    }

    private void addOption(String id, String label, String detail) {
        RadioButton choice = new RadioButton(this); choice.setId(View.generateViewId()); choice.setTag(id);
        choice.setText(label + "\n" + detail); choice.setTextSize(16); choice.setMinHeight(dp(56)); choice.setPadding(dp(8), dp(8), dp(8), dp(8));
        options.addView(choice); choice.setOnClickListener(v -> chosenId = id);
    }

    private void confirmRestart() {
        if (busy) return;
        Runnable next = () -> confirm("The game will close and restart to apply this driver. Progress not yet saved by the game may be lost.", () -> stage(chosenId));
        if ("system".equals(chosenId)) next.run();
        else confirm("This driver runs with Witch Time's app privileges, including access to game saves. Continue only if you trust its source.", next);
    }

    private void stage(String id) {
        setBusy(true); status.setText("Preparing restart…");
        worker.execute(() -> {
            String nonce = null, error = null;
            try { nonce = store.stageRestart(id, !"system".equals(id)); }
            catch (IOException e) { error = e.getMessage(); }
            final String result = nonce, failure = error;
            String orphan = stageOwnership.stageCompleted(result);
            if (orphan != null) { discardUndelivered(orphan); return; }
            runOnUiThread(() -> {
                if (result != null && (isFinishing() || isDestroyed())) { discardUndeliveredAsync(stageOwnership.abandon()); return; }
                if (result != null && !stageOwnership.claimDelivery(result)) return;
                if (isFinishing() || isDestroyed()) return;
                setBusy(false);
                if (result == null) { status.setText("Could not prepare restart: " + failure); return; }
                setResult(RESULT_OK, new Intent().putExtra(EXTRA_RESTART_NONCE, result)); finish();
            });
        });
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ZIP || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData(); setBusy(true); status.setText("Checking and importing driver…");
        worker.execute(() -> {
            String error = null, id = null;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new IOException("Could not open selected file");
                GpuDriverStore.Driver d = store.importZip(in); id = d.id;
            } catch (Exception e) { error = e.getMessage(); }
            final String failure = error, imported = id;
            runOnUiThread(() -> {
                if (isFinishing() || isDestroyed()) return;
                setBusy(false); renderOptions();
                if (failure != null) status.setText("Import failed: " + failure);
                else { newImportId = imported; chosenId = imported; renderOptions(); status.setText("Driver imported. Review it above, then choose and restart."); }
            });
        });
    }

    private void setBusy(boolean value) {
        busy = value;
        if (restart != null) restart.setEnabled(!value);
        if (options != null) enableTree(options, !value);
        for (int i = 0; i < content.getChildCount(); i++) enableTree(content.getChildAt(i), !value);
    }
    private void enableTree(View v, boolean enabled) { v.setEnabled(enabled); if (v instanceof android.view.ViewGroup) for (int i = 0; i < ((android.view.ViewGroup)v).getChildCount(); i++) enableTree(((android.view.ViewGroup)v).getChildAt(i), enabled); }
    private void confirm(String message, Runnable yes) { new AlertDialog.Builder(this).setMessage(message).setNegativeButton("Cancel", null).setPositiveButton("Continue", (d, w) -> yes.run()).show(); }
    private void cancelChooser() { discardAbandonedStage(); setResult(RESULT_CANCELED); finish(); }
    @Override public void onBackPressed() { cancelChooser(); }
    private void discardUndeliveredAsync(String nonce) {
        if (nonce == null) return;
        new Thread(() -> discardUndelivered(nonce), "gpu-driver-discard-stage").start();
    }
    private void discardAbandonedStage() { discardUndeliveredAsync(stageOwnership.abandon()); }
    private void discardUndelivered(String nonce) {
        try { store.discardRestart(nonce); }
        catch (IOException e) {
            Log.e(TAG, "Could not discard an undelivered staged driver restart", e);
            runOnUiThread(() -> {
                if (status != null && !isDestroyed() && !isFinishing())
                    status.setText("Could not clear the undelivered restart request. Close and reopen Witch Time before changing drivers.");
                else Toast.makeText(getApplicationContext(), "Driver restart request could not be cleared. Close and reopen Witch Time before changing drivers.", Toast.LENGTH_LONG).show();
            });
        }
    }
    private TextView text(String value, int size, boolean bold) { TextView v = new TextView(this); v.setText(value); v.setTextSize(size); v.setTextColor(Color.rgb(32, 30, 27)); if (bold) v.setTypeface(null, 1); return v; }
    private Button button(String label) { Button b = new Button(this); b.setText(label); b.setTextSize(16); b.setMinHeight(dp(52)); return b; }
    private LinearLayout.LayoutParams gap(int top) { LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2); p.topMargin = dp(top); return p; }
    private int dp(int n) { return (int)(n * getResources().getDisplayMetrics().density + .5f); }
    @Override protected void onDestroy() { discardAbandonedStage(); worker.shutdown(); super.onDestroy(); }
}
