package org.witchtime.android;

import android.os.Bundle;
import android.view.WindowManager;
import org.libsdl.app.SDLActivity;

/** SDL owns the native thread and surface lifecycle. No storage permission is needed. */
public final class WitchTimeActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
