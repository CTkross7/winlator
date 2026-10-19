package com.winlator.cmod.core;

import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The prebuilt imagefs.txz has the original package's data path baked into its text config files
 * (Vulkan ICD json, pulse, dbus, fonts, scripts). After extraction, rewrite them to this app's path.
 * Compiled binaries cannot be patched in place because the new path is longer, so ELF files are skipped.
 */
public final class ImageFsPathRewriter {
    private static final String TAG = "ImageFsPathRewriter";
    private static final long MAX_FILE_SIZE = 4L * 1024 * 1024;

    private static final String[] SCAN_DIRS = {"usr/etc", "usr/share/vulkan", "usr/share/dbus-1", "usr/bin"};

    private static final String[][] REPLACEMENTS = {
        {AppPaths.LEGACY_DATA_DIR, AppPaths.DATA_DIR},
        {"/data/user/0/com.winlator.cmod", AppPaths.DATA_DIR},
        {"/data/data/com.winlator/", AppPaths.DATA_DIR + "/"},
        {"/data/user/0/com.winlator/", AppPaths.DATA_DIR + "/"},
    };

    private ImageFsPathRewriter() {}

    public static int rewrite(File rootDir) {
        int rewritten = 0;
        for (String dir : SCAN_DIRS) rewritten += rewriteDir(new File(rootDir, dir));
        Log.i(TAG, "Rewrote package paths in " + rewritten + " files");
        return rewritten;
    }

    private static int rewriteDir(File dir) {
        File[] children = dir.listFiles();
        if (children == null) return 0;

        int count = 0;
        for (File file : children) {
            if (Files.isSymbolicLink(file.toPath())) continue;
            if (file.isDirectory()) count += rewriteDir(file);
            else if (rewriteFile(file)) count++;
        }
        return count;
    }

    private static boolean rewriteFile(File file) {
        if (file.length() == 0 || file.length() > MAX_FILE_SIZE) return false;

        try {
            byte[] data;
            try (FileInputStream in = new FileInputStream(file)) {
                data = new byte[(int)file.length()];
                int offset = 0;
                while (offset < data.length) {
                    int read = in.read(data, offset, data.length - offset);
                    if (read < 0) break;
                    offset += read;
                }
            }

            if (isBinary(data)) return false;

            String text = new String(data, StandardCharsets.ISO_8859_1);
            String result = text;
            for (String[] pair : REPLACEMENTS) result = result.replace(pair[0], pair[1]);
            if (result.equals(text)) return false;

            if (!file.canWrite()) file.setWritable(true, true);
            try (FileOutputStream out = new FileOutputStream(file)) {
                out.write(result.getBytes(StandardCharsets.ISO_8859_1));
            }
            return true;
        }
        catch (IOException e) {
            Log.w(TAG, "Unable to rewrite " + file, e);
            return false;
        }
    }

    private static boolean isBinary(byte[] data) {
        int limit = Math.min(data.length, 8192);
        for (int i = 0; i < limit; i++) if (data[i] == 0) return true;
        return false;
    }
}
