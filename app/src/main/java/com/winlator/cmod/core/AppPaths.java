package com.winlator.cmod.core;

/**
 * Single source of truth for the absolute data paths that used to be hardcoded as
 * "/data/data/com.winlator.cmod/...". Keep DATA_DIR in sync with applicationId in app/build.gradle.
 */
public final class AppPaths {
    private AppPaths() {}

    /** Package-name prefix that is baked into the prebuilt imagefs.txz. */
    public static final String LEGACY_DATA_DIR = "/data/data/com.winlator.cmod";

    public static final String DATA_DIR = "/data/data/com.ctkross.studiolauncher";
    public static final String FILES_DIR = DATA_DIR + "/files";
    public static final String IMAGEFS = FILES_DIR + "/imagefs";
    public static final String STORAGE = DATA_DIR + "/storage";
}
