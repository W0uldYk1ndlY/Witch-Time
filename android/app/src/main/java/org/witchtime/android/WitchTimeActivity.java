package org.witchtime.android;

import org.libsdl.app.SDLActivity;

/** SDL owns the native thread and surface lifecycle. No storage permission is needed. */
public final class WitchTimeActivity extends SDLActivity {
    @Override protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }
}
