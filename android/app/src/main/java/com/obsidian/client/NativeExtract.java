package com.obsidian.client;

import android.util.Log;

/**
 * JNI bridge to on-device classic MPQ extract ({@code libobsidian_extract.so}).
 */
public final class NativeExtract {
    private static final String TAG = "ObsidianExtract";
    private static boolean loaded;
    private static boolean available;

    static {
        try {
            System.loadLibrary("c++_shared");
            System.loadLibrary("obsidian_extract");
            loaded = true;
            available = nativeIsExtractAvailable();
            Log.i(TAG, "libobsidian_extract loaded, available=" + available);
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "Native extract unavailable: " + e.getMessage());
            loaded = false;
            available = false;
        }
    }

    private NativeExtract() {}

    public static boolean isAvailable() {
        return loaded && available;
    }

    /**
     * @param mpqDir absolute path to folder containing common.MPQ etc.
     * @param dataOut absolute path to app Data/ directory (writes expansions/classic)
     * @return null on success, else error message
     */
    public static String extractClassic(String mpqDir, String dataOut) {
        if (!isAvailable()) {
            return "On-device MPQ extract library failed to load.";
        }
        try {
            return nativeExtractClassic(mpqDir, dataOut);
        } catch (UnsatisfiedLinkError e) {
            return "Native extract missing: " + e.getMessage();
        } catch (Throwable t) {
            return "Extract failed: " + t.getMessage();
        }
    }

    private static native boolean nativeIsExtractAvailable();
    private static native String nativeExtractClassic(String mpqDir, String dataOut);
}
