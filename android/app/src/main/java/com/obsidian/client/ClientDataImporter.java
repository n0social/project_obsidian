package com.obsidian.client;

import android.content.Context;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;

import androidx.documentfile.provider.DocumentFile;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Copies a user-picked Vanilla client or extracted classic tree into app storage.
 * Raw client folders are unpacked with the in-app StormLib extractor
 * ({@link NativeExtract}). After extract/import, {@link ObsidianUiOverlay}
 * restores Obsidian UI files so stock WoWee Interface art cannot replace them.
 */
public final class ClientDataImporter {
    private static final String TAG = "ObsidianData";

    public interface Progress {
        void onProgress(int percent, String message);
    }

    public static final class Result {
        public final boolean success;
        public final String message;
        public final boolean needsNativeExtract;
        public final File mpqStagingDir;

        Result(boolean success, String message, boolean needsNativeExtract, File mpqStagingDir) {
            this.success = success;
            this.message = message;
            this.needsNativeExtract = needsNativeExtract;
            this.mpqStagingDir = mpqStagingDir;
        }

        static Result ok(String message) {
            return new Result(true, message, false, null);
        }

        static Result fail(String message) {
            return new Result(false, message, false, null);
        }

        static Result stageMpqs(String message, File staging) {
            return new Result(false, message, true, staging);
        }
    }

    private ClientDataImporter() {}

    public static File dataDirectory(Context context) {
        File base = context.getExternalFilesDir(null);
        if (base == null) base = context.getFilesDir();
        File data = new File(base, "Data");
        //noinspection ResultOfMethodCallIgnored
        data.mkdirs();
        return data;
    }

    public static boolean hasManifest(File data) {
        return new File(data, "manifest.json").isFile()
                || new File(data, "expansions/classic/manifest.json").isFile();
    }

    public static Result importTreeUri(Context context, Uri treeUri, boolean expectExtracted,
                                       AtomicBoolean cancelled, Progress progress) {
        if (treeUri == null) return Result.fail("No folder selected.");
        DocumentFile root = DocumentFile.fromTreeUri(context, treeUri);
        if (root == null || !root.isDirectory()) {
            return Result.fail("Could not open the selected folder.");
        }

        progress.onProgress(2, "Scanning selected folder…");
        DocumentFile dataDir = findDataDir(root);
        DocumentFile classicRoot = findClassicExtractRoot(root);

        if (expectExtracted || classicRoot != null) {
            DocumentFile source = classicRoot != null ? classicRoot : root;
            if (!looksLikeExtract(source)) {
                return Result.fail("Selected folder is not an extracted classic tree (need manifest.json).");
            }
            File dest = new File(dataDirectory(context), "expansions/classic");
            return copyDocumentTree(context, source, dest, cancelled, progress);
        }

        // Treat as vanilla client: prefer Data/ with MPQs.
        DocumentFile mpqDir = dataDir != null ? dataDir : root;
        List<DocumentFile> mpqs = listMpqs(mpqDir);
        if (mpqs.isEmpty() && dataDir == null) {
            // Maybe user picked the Data folder itself.
            mpqs = listMpqs(root);
            mpqDir = root;
        }
        if (mpqs.isEmpty()) {
            return Result.fail(
                    "No .MPQ files found. Select the WoW 1.12.1 install (or its Data folder), "
                            + "or use Import extracted Data if you already ran extract on PC.");
        }

        File staging = new File(context.getFilesDir(), "mpq_staging");
        deleteRecursive(staging);
        //noinspection ResultOfMethodCallIgnored
        staging.mkdirs();

        progress.onProgress(5, "Copying " + mpqs.size() + " MPQ archives…");
        int i = 0;
        for (DocumentFile mpq : mpqs) {
            if (cancelled.get()) return Result.fail("Cancelled.");
            String name = mpq.getName();
            if (name == null) continue;
            File out = new File(staging, name);
            if (!copyDocumentFile(context, mpq, out)) {
                return Result.fail("Failed copying " + name);
            }
            i++;
            int pct = 5 + (int) ((i * 55.0) / mpqs.size());
            progress.onProgress(pct, "Copied " + name + " (" + i + "/" + mpqs.size() + ")");
        }

        // Also copy common nested locale MPQs one level deep if present.
        DocumentFile[] children = mpqDir.listFiles();
        if (children != null) {
            for (DocumentFile child : children) {
                if (cancelled.get()) return Result.fail("Cancelled.");
                if (!child.isDirectory()) continue;
                List<DocumentFile> nested = listMpqs(child);
                if (nested.isEmpty()) continue;
                File nestedDir = new File(staging, child.getName());
                //noinspection ResultOfMethodCallIgnored
                nestedDir.mkdirs();
                for (DocumentFile mpq : nested) {
                    String name = mpq.getName();
                    if (name == null) continue;
                    if (!copyDocumentFile(context, mpq, new File(nestedDir, name))) {
                        return Result.fail("Failed copying " + child.getName() + "/" + name);
                    }
                }
            }
        }

        progress.onProgress(65, "Built-in extractor unpacking MPQs (keep the app open)…");
        File classicOut = new File(dataDirectory(context), "expansions/classic");
        //noinspection ResultOfMethodCallIgnored
        classicOut.mkdirs();

        String nativeMsg = NativeExtract.extractClassic(
                staging.getAbsolutePath(),
                dataDirectory(context).getAbsolutePath());
        if (NativeExtract.isAvailable() && nativeMsg == null) {
            progress.onProgress(95, "Extract finished — applying Obsidian UI overlay…");
            ObsidianUiOverlay.apply(context);
            if (hasManifest(dataDirectory(context))) {
                progress.onProgress(100, "Classic data ready.");
                return Result.ok("Classic client extracted. Obsidian UI overlay applied.");
            }
            return Result.fail("Extract finished but manifest.json was not found under expansions/classic.");
        }

        String detail = nativeMsg != null ? nativeMsg : "Native MPQ extractor not available in this build.";
        progress.onProgress(100, detail);
        return Result.stageMpqs(
                detail + " MPQs are staged under " + staging.getAbsolutePath()
                        + ". You can still use Import extracted Data with a PC extract.",
                staging);
    }

    private static boolean looksLikeExtract(DocumentFile dir) {
        DocumentFile manifest = dir.findFile("manifest.json");
        if (manifest != null && manifest.isFile()) return true;
        DocumentFile expansions = dir.findFile("expansions");
        if (expansions != null && expansions.isDirectory()) {
            DocumentFile classic = expansions.findFile("classic");
            if (classic != null && classic.isDirectory()) {
                DocumentFile m = classic.findFile("manifest.json");
                return m != null && m.isFile();
            }
        }
        return false;
    }

    private static DocumentFile findClassicExtractRoot(DocumentFile root) {
        if (root.findFile("manifest.json") != null) return root;
        DocumentFile expansions = root.findFile("expansions");
        if (expansions != null && expansions.isDirectory()) {
            DocumentFile classic = expansions.findFile("classic");
            if (classic != null && classic.isDirectory() && classic.findFile("manifest.json") != null) {
                return classic;
            }
        }
        DocumentFile data = findDataDir(root);
        if (data != null) {
            DocumentFile exp = data.findFile("expansions");
            if (exp != null && exp.isDirectory()) {
                DocumentFile classic = exp.findFile("classic");
                if (classic != null && classic.isDirectory() && classic.findFile("manifest.json") != null) {
                    return classic;
                }
            }
        }
        return null;
    }

    private static DocumentFile findDataDir(DocumentFile root) {
        DocumentFile data = root.findFile("Data");
        if (data != null && data.isDirectory()) return data;
        data = root.findFile("data");
        if (data != null && data.isDirectory()) return data;
        return null;
    }

    private static List<DocumentFile> listMpqs(DocumentFile dir) {
        List<DocumentFile> out = new ArrayList<>();
        DocumentFile[] files = dir.listFiles();
        if (files == null) return out;
        for (DocumentFile f : files) {
            if (!f.isFile()) continue;
            String name = f.getName();
            if (name == null) continue;
            String lower = name.toLowerCase(Locale.US);
            if (lower.endsWith(".mpq")) out.add(f);
        }
        return out;
    }

    private static Result copyDocumentTree(Context context, DocumentFile source, File dest,
                                           AtomicBoolean cancelled, Progress progress) {
        deleteRecursive(dest);
        //noinspection ResultOfMethodCallIgnored
        dest.mkdirs();
        List<DocumentFile> files = new ArrayList<>();
        collectFiles(source, files);
        if (files.isEmpty()) return Result.fail("Selected extract folder is empty.");

        int done = 0;
        for (DocumentFile file : files) {
            if (cancelled.get()) return Result.fail("Cancelled.");
            String rel = relativePath(source, file);
            if (rel == null || rel.isEmpty()) continue;
            File out = new File(dest, rel);
            File parent = out.getParentFile();
            if (parent != null) {
                //noinspection ResultOfMethodCallIgnored
                parent.mkdirs();
            }
            if (!copyDocumentFile(context, file, out)) {
                return Result.fail("Failed copying " + rel);
            }
            done++;
            int pct = 5 + (int) ((done * 90.0) / files.size());
            if (done == 1 || done == files.size() || done % 50 == 0) {
                progress.onProgress(Math.min(pct, 99), "Copied " + done + "/" + files.size() + " files");
            }
        }
        progress.onProgress(100, "Import complete.");
        if (!hasManifest(dataDirectory(context)) && !new File(dest, "manifest.json").isFile()) {
            return Result.fail("Copy finished but manifest.json is missing.");
        }
        // Ensure expected layout: expansions/classic/manifest.json
        File classicManifest = new File(dest, "manifest.json");
        File dataRoot = dataDirectory(context);
        File topManifest = new File(dataRoot, "manifest.json");
        if (classicManifest.isFile() && !topManifest.isFile()) {
            // Optional convenience copy for engines that look at Data/manifest.json
            try {
                copyFile(classicManifest, topManifest);
            } catch (Exception ignored) {
            }
        }
        ObsidianUiOverlay.apply(context);
        return Result.ok("Classic data imported (" + done + " files). Obsidian UI overlay applied.");
    }

    private static void collectFiles(DocumentFile dir, List<DocumentFile> out) {
        DocumentFile[] children = dir.listFiles();
        if (children == null) return;
        for (DocumentFile child : children) {
            if (child.isDirectory()) collectFiles(child, out);
            else if (child.isFile()) out.add(child);
        }
    }

    private static String relativePath(DocumentFile root, DocumentFile file) {
        String rootUri = root.getUri().toString();
        String fileUri = file.getUri().toString();
        if (!fileUri.startsWith(rootUri)) {
            // Fallback: use display name only (lossy for nested). Prefer document id path.
            String docId = null;
            try {
                docId = DocumentsContract.getDocumentId(file.getUri());
                String rootId = DocumentsContract.getTreeDocumentId(root.getUri());
                if (docId != null && rootId != null && docId.startsWith(rootId)) {
                    String rel = docId.substring(rootId.length());
                    if (rel.startsWith("/")) rel = rel.substring(1);
                    return rel.replace('\\', '/');
                }
            } catch (Exception ignored) {
            }
            return file.getName();
        }
        // Build path by walking parents.
        List<String> parts = new ArrayList<>();
        DocumentFile cur = file;
        while (cur != null && !cur.getUri().equals(root.getUri())) {
            String name = cur.getName();
            if (name != null) parts.add(0, name);
            cur = cur.getParentFile();
            if (parts.size() > 64) break;
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < parts.size(); i++) {
            if (i > 0) sb.append('/');
            sb.append(parts.get(i));
        }
        return sb.toString();
    }

    private static boolean copyDocumentFile(Context context, DocumentFile src, File dest) {
        try (InputStream in = context.getContentResolver().openInputStream(src.getUri());
             OutputStream out = new FileOutputStream(dest)) {
            if (in == null) return false;
            byte[] buf = new byte[256 * 1024];
            int n;
            while ((n = in.read(buf)) >= 0) {
                if (n > 0) out.write(buf, 0, n);
            }
            return true;
        } catch (Exception e) {
            Log.w(TAG, "copy failed " + src.getUri(), e);
            return false;
        }
    }

    private static void copyFile(File src, File dest) throws Exception {
        try (InputStream in = new java.io.FileInputStream(src);
             OutputStream out = new FileOutputStream(dest)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) >= 0) {
                if (n > 0) out.write(buf, 0, n);
            }
        }
    }

    private static void deleteRecursive(File f) {
        if (f == null || !f.exists()) return;
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) deleteRecursive(k);
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
