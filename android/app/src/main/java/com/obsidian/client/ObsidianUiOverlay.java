package com.obsidian.client;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Pins Obsidian UI on top of extracted Vanilla data.
 *
 * Extract unpacks Interface/GlueXML from the client MPQ (WoWee's default look).
 * This copies APK-bundled files plus a user-writable folder into
 * {@code Data/override} and {@code Data/expansions/classic/override} so those
 * files always win. The engine also keeps tablet ImGui chrome in C++ — extract
 * cannot revert that.
 *
 * Drop custom BLP/PNG/XML under {@code files/obsidian_ui/} using the same
 * relative paths as the extract (e.g. {@code Interface/Buttons/UI-Quickslot2.blp}).
 */
public final class ObsidianUiOverlay {
    private static final String TAG = "ObsidianUi";
    private static final String ASSET_ROOT = "obsidian_override";

    private ObsidianUiOverlay() {}

    public static File userUiDir(Context context) {
        File dir = new File(context.getFilesDir(), "obsidian_ui");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        return dir;
    }

    public static void apply(Context context) {
        File data = ClientDataImporter.dataDirectory(context);
        File user = userUiDir(context);
        File userReadme = new File(user, "README.txt");
        if (!userReadme.isFile()) {
            try (FileOutputStream out = new FileOutputStream(userReadme)) {
                String text = "Drop custom Interface files here using extract-relative paths.\n"
                        + "Example: Interface/Buttons/UI-Quickslot2.blp\n"
                        + "Obsidian copies this folder onto Data/override after every extract.\n";
                out.write(text.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            } catch (Exception ignored) {
            }
        }
        File[] dests = new File[] {
                new File(data, "override"),
                new File(data, "expansions/classic/override")
        };
        for (File dest : dests) {
            //noinspection ResultOfMethodCallIgnored
            dest.mkdirs();
            copyBundledAssets(context.getAssets(), ASSET_ROOT, dest);
            copyTree(userUiDir(context), dest);
        }
        Log.i(TAG, "Applied Obsidian UI overlay under " + data.getAbsolutePath());
    }

    private static void copyBundledAssets(AssetManager am, String assetPath, File dest) {
        String[] kids;
        try {
            kids = am.list(assetPath);
        } catch (Exception e) {
            return;
        }
        if (kids == null) return;
        if (kids.length == 0) {
            copyAssetFile(am, assetPath, dest);
            return;
        }
        //noinspection ResultOfMethodCallIgnored
        dest.mkdirs();
        for (String kid : kids) {
            if (kid == null || kid.isEmpty()) continue;
            copyBundledAssets(am, assetPath + "/" + kid, new File(dest, kid));
        }
    }

    private static void copyAssetFile(AssetManager am, String assetPath, File dest) {
        if (assetPath.endsWith("README.txt") || assetPath.endsWith(".keep")) return;
        File parent = dest.getParentFile();
        if (parent != null) {
            //noinspection ResultOfMethodCallIgnored
            parent.mkdirs();
        }
        try (InputStream in = am.open(assetPath);
             OutputStream out = new FileOutputStream(dest)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        } catch (Exception e) {
            Log.w(TAG, "Skip overlay asset " + assetPath + ": " + e.getMessage());
        }
    }

    private static void copyTree(File src, File dest) {
        if (src == null || !src.exists()) return;
        if (src.isFile()) {
            File parent = dest.getParentFile();
            if (parent != null) {
                //noinspection ResultOfMethodCallIgnored
                parent.mkdirs();
            }
            try (InputStream in = new FileInputStream(src);
                 OutputStream out = new FileOutputStream(dest)) {
                byte[] buf = new byte[64 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            } catch (Exception e) {
                Log.w(TAG, "Skip user UI file " + src + ": " + e.getMessage());
            }
            return;
        }
        File[] kids = src.listFiles();
        if (kids == null) return;
        //noinspection ResultOfMethodCallIgnored
        dest.mkdirs();
        for (File kid : kids) {
            copyTree(kid, new File(dest, kid.getName()));
        }
    }
}
