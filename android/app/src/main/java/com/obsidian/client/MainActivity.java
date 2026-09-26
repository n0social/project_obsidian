package com.obsidian.client;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Build;
import android.os.Bundle;
import android.system.Os;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * Obsidian game activity — SDL2 Android host that boots WoWee via SDL_main
 * exported from libwowee.so.
 *
 * Implemented in Java (not Kotlin) so class loading against org.libsdl.app.SDLActivity
 * is as straightforward as possible for PackageManager / ART on OEM devices and emulators.
 */
public class MainActivity extends SDLActivity {
    private static final String TAG = "Obsidian";
    public static final String EXTRA_AUTO_LOGIN = "obsidian_auto_login";

    @Override
    protected String[] getLibraries() {
        // SDL2 (+ JNI / SDL_main glue) is statically linked into libwowee.so.
        return new String[] { "c++_shared", "vulkan", "wowee" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        CrashReporter.INSTANCE.install(this);
        CrashReporter.INSTANCE.writeBreadcrumb(this, "MainActivity.onCreate begin");

        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN);

        android.content.SharedPreferences prefs = ObsidianSettings.prefs(this);
        ObsidianSettings.applyDefaults(prefs);
        if (prefs.getBoolean(ObsidianSettings.KEY_KEEP_SCREEN_ON, true)) {
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
        String orientation = prefs.getString(ObsidianSettings.KEY_ORIENTATION, "landscape");
        setRequestedOrientation("sensorLandscape".equals(orientation)
                ? ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                : ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);

        File dataDir = ensureDataDirectory();
        File configDir = ObsidianSettings.configDir(this);
        extractBundledWardenCache(new File(dataDir.getParentFile(), "warden_cache"));
        ObsidianUiOverlay.apply(this);
        boolean autoLogin = getIntent() != null
                && getIntent().getBooleanExtra(EXTRA_AUTO_LOGIN, true);
        try {
            Os.setenv("WOW_DATA_PATH", dataDir.getAbsolutePath(), true);
            Os.setenv("WOWEE_CONFIG_ROOT", configDir.getAbsolutePath(), true);
            Os.setenv("WOWEE_LOG_STDOUT", "1", true);
            Os.setenv("WOWEE_LOG_LEVEL", "info", true);
            Os.setenv("WOWEE_CRASH_DIR", new File(getFilesDir(), "crash_reports").getAbsolutePath(), true);
            Os.setenv("OBSIDIAN_MUTE_LOGIN_MUSIC",
                    prefs.getBoolean(ObsidianSettings.KEY_MUTE_LOGIN, true) ? "1" : "0", true);
            Os.setenv("OBSIDIAN_RELATIVE_MOUSE",
                    prefs.getBoolean(ObsidianSettings.KEY_RELATIVE_MOUSE, false) ? "1" : "0", true);
            String realmHost = LauncherActivity.sanitizeHost(
                    prefs.getString(ObsidianSettings.KEY_REALM_HOST, ""));
            Os.setenv("OBSIDIAN_REALM_HOST", realmHost, true);
            Os.setenv("OBSIDIAN_REALM_PORT",
                    Integer.toString(prefs.getInt(ObsidianSettings.KEY_REALM_PORT, 3724)), true);
            Os.setenv("OBSIDIAN_ACCOUNT_USER",
                    prefs.getString(ObsidianSettings.KEY_ACCOUNT_USER, ""), true);
            Os.setenv("OBSIDIAN_ACCOUNT_PASS",
                    prefs.getString(ObsidianSettings.KEY_ACCOUNT_PASS, ""), true);
            Os.setenv("OBSIDIAN_NATIVE_LOGIN", autoLogin ? "1" : "0", true);
            // The original 1.12 Interface/FrameXML from the classic extract is
            // the in-game UI; touch <external files>/framexml_off to fall back to
            // WoWee's built-in HUD. Both variables must be set or the widget
            // tree is built and then not drawn.
            String frameXml = new File(dataDir.getParentFile(), "framexml_off").isFile() ? "0" : "1";
            Os.setenv("WOWEE_LOAD_FRAMEXML", frameXml, true);
            Os.setenv("WOWEE_FRAMEXML_UI", frameXml, true);
            File widgetDump = new File(dataDir.getParentFile(), "widget_dump");
            if (widgetDump.isFile()) Os.setenv("WOWEE_WIDGET_DUMP", "2", true);
            File terrainRadius = new File(dataDir.getParentFile(), "terrain_radius");
            if (terrainRadius.isFile()) {
                try (java.io.BufferedReader r = new java.io.BufferedReader(
                        new java.io.FileReader(terrainRadius))) {
                    String v = r.readLine();
                    if (v != null && v.trim().matches("[1-8]")) {
                        Os.setenv("WOWEE_TERRAIN_LOAD_RADIUS", v.trim(), true);
                    }
                } catch (java.io.IOException ignored) {
                }
            }
            Log.i(TAG, "Native login host=" + realmHost
                    + ":" + prefs.getInt(ObsidianSettings.KEY_REALM_PORT, 3724));

            // Tablet memory profile: downsample textures + smaller GPU caches.
            // Does not tear meshes — only softens fidelity / draw cost.
            int maxTex = ObsidianSettings.textureMaxDim(prefs);
            if (maxTex > 0) {
                Os.setenv("WOWEE_MAX_TEX_DIM", Integer.toString(maxTex), true);
            }
            // A 4 GB tablet has about 1.2 GB free with the game open. The old
            // 256/256/384/192 caps plus a 345 MB file cache let lmkd kill the
            // game mid-session, which the realm saw as a dropped connection.
            Os.setenv("WOWEE_TERRAIN_TEX_CACHE_MB", "96", true);
            Os.setenv("WOWEE_M2_TEX_CACHE_MB", "96", true);
            Os.setenv("WOWEE_WMO_TEX_CACHE_MB", "128", true);
            Os.setenv("WOWEE_CHARACTER_TEX_CACHE_MB", "64", true);
            Os.setenv("WOWEE_FILE_CACHE_MB", "128", true);
            // Each terrain worker holds a whole decoded ADT while it works.
            Os.setenv("WOWEE_TERRAIN_WORKERS", "2", true);
        } catch (Throwable t) {
            Log.w(TAG, "Failed to setenv", t);
        }

        CrashReporter.INSTANCE.writeBreadcrumb(
                this,
                "dataDir=" + dataDir.getAbsolutePath()
                        + " hasManifest=" + hasManifest(dataDir)
                        + " sdk=" + Build.VERSION.SDK_INT);

        try {
            super.onCreate(savedInstanceState);
            CrashReporter.INSTANCE.writeBreadcrumb(this, "MainActivity.onCreate super done");
        } catch (Throwable t) {
            CrashReporter.INSTANCE.writeBreadcrumb(this, "MainActivity.onCreate FAILED: " + t.getMessage());
            throw t;
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            lockLandscape();
            hideSystemUi();
        }
    }

    @Override
    public void onResume() {
        super.onResume();
        lockLandscape();
    }

    /**
     * SDL's setOrientationBis can pick portrait from the panel's natural
     * 1200x1920, which squashes the auth UI into a vertical stack. Always
     * keep Obsidian locked to landscape on this tablet build.
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        Log.v(TAG, "setOrientationBis ignored (force landscape) w=" + w + " h=" + h
                + " resizable=" + resizable + " hint=" + hint);
        lockLandscape();
    }

    private void lockLandscape() {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    public void onBackPressed() {
        // Return to landscape launcher Sign in (not a separate vertical page).
        Intent intent = new Intent(this, LauncherActivity.class);
        intent.putExtra(LauncherActivity.EXTRA_OPEN_SIGNIN, true);
        intent.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(intent);
        finish();
    }

    @Override
    protected void onDestroy() {
        CrashReporter.INSTANCE.writeBreadcrumb(this, "MainActivity.onDestroy");
        super.onDestroy();
    }

    private void hideSystemUi() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            //noinspection deprecation
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
    }

    private File ensureDataDirectory() {
        // Primary emulated storage (same internal userdata pool as Settings
        // storage), not the removable SD card. adb can write this path.
        File base = getExternalFilesDir(null);
        if (base == null) {
            base = getFilesDir();
        }
        File data = new File(base, "Data");
        if (!data.exists()) {
            //noinspection ResultOfMethodCallIgnored
            data.mkdirs();
        }
        //noinspection ResultOfMethodCallIgnored
        new File(getFilesDir(), "crash_reports").mkdirs();
        try {
            File readme = new File(data, "README_OBSIDIAN.txt");
            String text =
                    "project_obsidian expects extracted Vanilla 1.12.1 assets here.\n\n"
                            + "Copy the WoWee extraction output so that ONE of these exists:\n"
                            + "  " + data.getAbsolutePath() + "/manifest.json\n"
                            + "  " + data.getAbsolutePath() + "/expansions/classic/manifest.json\n\n"
                            + "Use Data → Select WoW client folder (built-in extractor) or Import extracted Data.\n";
            try (java.io.FileWriter w = new java.io.FileWriter(readme, false)) {
                w.write(text);
            }
        } catch (Throwable ignored) {
        }
        return data;
    }

    /**
     * Ship VMaNGOS/RetroWoW Warden challenge tables (.cr) from APK assets into
     * the writable files tree next to Data/. Native code loads
     * ./warden_cache/<moduleMd5>.cr for HASH_REQUEST replies.
     */
    private void extractBundledWardenCache(File destDir) {
        try {
            //noinspection ResultOfMethodCallIgnored
            destDir.mkdirs();
            String[] names = getAssets().list("warden_cache");
            if (names == null || names.length == 0) {
                Log.i(TAG, "No bundled warden_cache assets");
                return;
            }
            for (String name : names) {
                if (name == null || !name.endsWith(".cr")) continue;
                File out = new File(destDir, name);
                // Refresh if missing or smaller than packaged (upgrade path).
                long assetSize = -1;
                try (java.io.InputStream in = getAssets().open("warden_cache/" + name)) {
                    assetSize = in.available();
                } catch (Throwable ignored) {
                }
                if (out.isFile() && assetSize > 0 && out.length() >= assetSize) {
                    continue;
                }
                try (java.io.InputStream in = getAssets().open("warden_cache/" + name);
                     java.io.FileOutputStream fos = new java.io.FileOutputStream(out)) {
                    byte[] buf = new byte[16 * 1024];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        fos.write(buf, 0, n);
                    }
                }
                Log.i(TAG, "Extracted Warden CR: " + out.getAbsolutePath()
                        + " (" + out.length() + " bytes)");
            }
        } catch (Throwable t) {
            Log.w(TAG, "Failed to extract bundled warden_cache", t);
        }
    }

    private boolean hasManifest(File data) {
        return new File(data, "manifest.json").isFile()
                || new File(data, "expansions/classic/manifest.json").isFile();
    }
}
