package com.obsidian.client;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.Locale;

/**
 * Shared launcher preferences + settings.cfg writer for the native client.
 */
public final class ObsidianSettings {
    public static final String PREFS = "obsidian_launcher";

    public static final String KEY_PRESET = "graphics_preset";
    public static final String KEY_SHADOWS = "shadows";
    public static final String KEY_SHADOW_DISTANCE = "shadow_distance";
    public static final String KEY_VIEW_DISTANCE = "view_distance";
    public static final String KEY_AA = "antialiasing";
    public static final String KEY_FXAA = "fxaa";
    public static final String KEY_NORMAL = "normal_mapping";
    public static final String KEY_POM = "pom";
    public static final String KEY_WATER = "water_refraction";
    public static final String KEY_CLUTTER = "ground_clutter";
    public static final String KEY_VSYNC = "vsync";
    public static final String KEY_MASTER = "master_volume";
    public static final String KEY_MUSIC = "music_volume";
    public static final String KEY_MUTE_LOGIN = "mute_login_music";
    public static final String KEY_RELATIVE_MOUSE = "relative_mouse";
    public static final String KEY_KEEP_SCREEN_ON = "keep_screen_on";
    public static final String KEY_ORIENTATION = "orientation";
    public static final String KEY_REALM_HOST = "realm_host";
    public static final String KEY_REALM_PORT = "realm_port";
    /** One-shot so old RetroWoW/Kronos presets are cleared and not reapplied. */
    private static final String KEY_REALM_PRESETS_REMOVED = "realm_presets_removed";
    public static final String KEY_ACCOUNT_USER = "account_user";
    public static final String KEY_ACCOUNT_PASS = "account_pass";
    public static final String KEY_AUTH_ERROR = "auth_error";
    /** 0=Full, 1=High(1024), 2=Medium(512), 3=Low(256) — maps to WOWEE_MAX_TEX_DIM */
    public static final String KEY_TEXTURE_QUALITY = "texture_quality";

    private ObsidianSettings() {}

    public static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public static void applyDefaults(SharedPreferences p) {
        SharedPreferences.Editor e = p.edit();
        if (!p.contains(KEY_PRESET)) e.putInt(KEY_PRESET, 2);
        if (!p.contains(KEY_SHADOWS)) e.putBoolean(KEY_SHADOWS, true);
        if (!p.contains(KEY_SHADOW_DISTANCE)) e.putFloat(KEY_SHADOW_DISTANCE, 150f);
        if (!p.contains(KEY_VIEW_DISTANCE)) e.putFloat(KEY_VIEW_DISTANCE, 1000f);
        if (!p.contains(KEY_AA)) e.putInt(KEY_AA, 0);
        if (!p.contains(KEY_FXAA)) e.putBoolean(KEY_FXAA, false);
        if (!p.contains(KEY_NORMAL)) e.putBoolean(KEY_NORMAL, true);
        if (!p.contains(KEY_POM)) e.putBoolean(KEY_POM, true);
        if (!p.contains(KEY_WATER)) e.putBoolean(KEY_WATER, true);
        if (!p.contains(KEY_CLUTTER)) e.putInt(KEY_CLUTTER, 100);
        if (!p.contains(KEY_VSYNC)) e.putBoolean(KEY_VSYNC, true);
        if (!p.contains(KEY_MASTER)) e.putInt(KEY_MASTER, 80);
        if (!p.contains(KEY_MUSIC)) e.putInt(KEY_MUSIC, 30);
        if (!p.contains(KEY_MUTE_LOGIN)) e.putBoolean(KEY_MUTE_LOGIN, true);
        if (!p.contains(KEY_RELATIVE_MOUSE)) e.putBoolean(KEY_RELATIVE_MOUSE, false);
        if (!p.contains(KEY_KEEP_SCREEN_ON)) e.putBoolean(KEY_KEEP_SCREEN_ON, true);
        if (!p.contains(KEY_ORIENTATION)) e.putString(KEY_ORIENTATION, "landscape");
        // Default Medium textures on tablets — softens look, cuts VRAM hard.
        if (!p.contains(KEY_TEXTURE_QUALITY)) e.putInt(KEY_TEXTURE_QUALITY, 2);
        if (!p.contains(KEY_REALM_HOST)) e.putString(KEY_REALM_HOST, "");
        if (!p.contains(KEY_REALM_PORT)) e.putInt(KEY_REALM_PORT, 3724);

        if (!p.getBoolean(KEY_REALM_PRESETS_REMOVED, false)) {
            String host = p.getString(KEY_REALM_HOST, "");
            String lower = host == null ? "" : host.trim().toLowerCase(Locale.US);
            if (lower.contains("retro-wow")
                    || lower.contains("twinstar")
                    || lower.contains("kronos")
                    || "localhost".equals(lower)
                    || "127.0.0.1".equals(lower)) {
                e.putString(KEY_REALM_HOST, "");
            }
            e.putBoolean(KEY_REALM_PRESETS_REMOVED, true);
        }
        e.apply();
    }

    public static void applyPreset(SharedPreferences p, int preset) {
        SharedPreferences.Editor e = p.edit().putInt(KEY_PRESET, preset);
        switch (preset) {
            case 1: // Low
                e.putBoolean(KEY_SHADOWS, false)
                        .putFloat(KEY_SHADOW_DISTANCE, 75f)
                        .putFloat(KEY_VIEW_DISTANCE, 600f)
                        .putInt(KEY_AA, 0)
                        .putBoolean(KEY_FXAA, false)
                        .putBoolean(KEY_NORMAL, false)
                        .putBoolean(KEY_POM, false)
                        .putBoolean(KEY_WATER, false)
                        .putInt(KEY_CLUTTER, 25)
                        .putInt(KEY_TEXTURE_QUALITY, 3);
                break;
            case 3: // High
                e.putBoolean(KEY_SHADOWS, true)
                        .putFloat(KEY_SHADOW_DISTANCE, 250f)
                        .putFloat(KEY_VIEW_DISTANCE, 1600f)
                        .putInt(KEY_AA, 1)
                        .putBoolean(KEY_FXAA, true)
                        .putBoolean(KEY_NORMAL, true)
                        .putBoolean(KEY_POM, true)
                        .putBoolean(KEY_WATER, true)
                        .putInt(KEY_CLUTTER, 130)
                        .putInt(KEY_TEXTURE_QUALITY, 1);
                break;
            case 4: // Ultra
                e.putBoolean(KEY_SHADOWS, true)
                        .putFloat(KEY_SHADOW_DISTANCE, 400f)
                        .putFloat(KEY_VIEW_DISTANCE, 2400f)
                        .putInt(KEY_AA, 2)
                        .putBoolean(KEY_FXAA, true)
                        .putBoolean(KEY_NORMAL, true)
                        .putBoolean(KEY_POM, true)
                        .putBoolean(KEY_WATER, true)
                        .putInt(KEY_CLUTTER, 150)
                        .putInt(KEY_TEXTURE_QUALITY, 0);
                break;
            case 2: // Medium
            default:
                e.putBoolean(KEY_SHADOWS, true)
                        .putFloat(KEY_SHADOW_DISTANCE, 150f)
                        .putFloat(KEY_VIEW_DISTANCE, 1000f)
                        .putInt(KEY_AA, 0)
                        .putBoolean(KEY_FXAA, false)
                        .putBoolean(KEY_NORMAL, true)
                        .putBoolean(KEY_POM, true)
                        .putBoolean(KEY_WATER, true)
                        .putInt(KEY_CLUTTER, 100)
                        .putInt(KEY_TEXTURE_QUALITY, 2);
                break;
        }
        e.apply();
    }

    public static File configDir(Context context) {
        File dir = new File(context.getFilesDir(), "config");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        return dir;
    }

    public static File settingsCfg(Context context) {
        return new File(configDir(context), "settings.cfg");
    }

    public static void writeNativeSettings(Context context) throws IOException {
        SharedPreferences p = prefs(context);
        File out = settingsCfg(context);
        try (FileWriter w = new FileWriter(out, false)) {
            w.write("graphics_preset=" + p.getInt(KEY_PRESET, 2) + "\n");
            w.write("shadows=" + (p.getBoolean(KEY_SHADOWS, true) ? "1" : "0") + "\n");
            w.write("shadow_distance=" + p.getFloat(KEY_SHADOW_DISTANCE, 150f) + "\n");
            w.write("view_distance=" + p.getFloat(KEY_VIEW_DISTANCE, 1000f) + "\n");
            w.write("antialiasing=" + p.getInt(KEY_AA, 0) + "\n");
            w.write("fxaa=" + (p.getBoolean(KEY_FXAA, false) ? "1" : "0") + "\n");
            w.write("normal_mapping=" + (p.getBoolean(KEY_NORMAL, true) ? "1" : "0") + "\n");
            w.write("pom=" + (p.getBoolean(KEY_POM, true) ? "1" : "0") + "\n");
            w.write("pom_quality=1\n");
            w.write("upscaling_mode=0\n");
            w.write("water_refraction=" + (p.getBoolean(KEY_WATER, true) ? "1" : "0") + "\n");
            // Native GameScreen reads ground_clutter_density (not ground_clutter).
            w.write("ground_clutter_density=" + p.getInt(KEY_CLUTTER, 100) + "\n");
            w.write("ground_clutter=" + p.getInt(KEY_CLUTTER, 100) + "\n");
            w.write("brightness=50\n");
            w.write("vsync=" + (p.getBoolean(KEY_VSYNC, true) ? "1" : "0") + "\n");
            w.write("fullscreen=1\n");
            w.write("master_volume=" + p.getInt(KEY_MASTER, 80) + "\n");
            w.write("music_volume=" + p.getInt(KEY_MUSIC, 30) + "\n");
            w.write("mute_login_music=" + (p.getBoolean(KEY_MUTE_LOGIN, true) ? "1" : "0") + "\n");
            w.write("relative_mouse=" + (p.getBoolean(KEY_RELATIVE_MOUSE, true) ? "1" : "0") + "\n");
            w.write("texture_quality=" + p.getInt(KEY_TEXTURE_QUALITY, 2) + "\n");
            w.write("realm_host=" + p.getString(KEY_REALM_HOST, "") + "\n");
            w.write("realm_port=" + p.getInt(KEY_REALM_PORT, 3724) + "\n");
        }
    }

    /** Longest texture edge for WOWEE_MAX_TEX_DIM; 0 = full resolution. */
    public static int textureMaxDim(SharedPreferences p) {
        switch (p.getInt(KEY_TEXTURE_QUALITY, 2)) {
            case 0: return 0;      // Full
            case 1: return 1024;   // High
            case 3: return 256;    // Low
            case 2:
            default: return 512;   // Medium — Tab A9+ default
        }
    }

    public static String describeDevice() {
        return String.format(Locale.US, "%s %s · Android %s",
                android.os.Build.MANUFACTURER,
                android.os.Build.MODEL,
                android.os.Build.VERSION.RELEASE);
    }
}
