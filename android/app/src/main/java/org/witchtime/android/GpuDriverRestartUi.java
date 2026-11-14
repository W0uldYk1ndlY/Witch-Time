package org.witchtime.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

/** Shared native-theme dialogs for the restart drain and its isolated helper. */
public final class GpuDriverRestartUi {
    private GpuDriverRestartUi() {}

    public static Dialog showProgress(Activity activity, String message) {
        LinearLayout body = new LinearLayout(activity);
        body.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(activity, 24);
        body.setPadding(pad, dp(activity, 8), pad, pad);
        ProgressBar progress = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        progress.setIndeterminate(true);
        body.addView(progress, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(activity, 4)));
        TextView detail = new TextView(activity);
        detail.setText(message == null || message.isEmpty() ? "Preparing a safe restart…" : message);
        detail.setTextSize(16);
        body.addView(detail, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        AlertDialog dialog = new AlertDialog.Builder(activity)
                .setTitle("Restarting Witch Time")
                .setView(body)
                .setCancelable(false)
                .create();
        dialog.show();
        return dialog;
    }

    public static void showError(Activity activity, String message) {
        new AlertDialog.Builder(activity)
                .setTitle("Restart could not continue")
                .setMessage(message == null || message.isEmpty()
                        ? "The game remains open. No graphics driver was changed."
                        : message)
                .setPositiveButton("OK", null)
                .show();
    }

    private static int dp(Activity activity, int value) {
        return (int) (value * activity.getResources().getDisplayMetrics().density + .5f);
    }
}
