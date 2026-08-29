package com.fourdo.android;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.provider.OpenableColumns;

import androidx.documentfile.provider.DocumentFile;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;

public final class SafFileImporter {

    // Tidy external-card layout:  <root>/{ bios, appdata, games, bezels }
    //   bios    — 3DO BIOS/font roms
    //   games   — imported game discs (was "library")
    //   appdata — runtime data: drivers, nvram, save states  (was scattered)
    //   bezels  — bezel overlays (managed by BezelResolver)
    private static final String BIOS_DIR_NAME = "bios";
    private static final String LIBRARY_DIR_NAME = "games";
    private static final String APPDATA_DIR_NAME = "appdata";
    private static final String DRIVER_DIR_NAME = "drivers";   // under appdata/

    private SafFileImporter() {
    }

    public static final class ImportResult {
        public final String path;
        public final int importedFileCount;

        ImportResult(String path, int importedFileCount) {
            this.path = path;
            this.importedFileCount = importedFileCount;
        }
    }

    public static Intent createOpenDocumentIntent() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        intent.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        return intent;
    }

    public static Intent createOpenDocumentTreeIntent() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        intent.addFlags(Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        intent.addFlags(Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        intent.putExtra("android.content.extra.SHOW_ADVANCED", true);
        return intent;
    }

    public static String importVulkanDriver(Context context, Uri uri) throws IOException {
        takePersistableReadPermission(context, uri);

        String displayName = getDisplayName(context, uri);
        String lowerName = displayName.toLowerCase();

        // The renderer's dlopen() runs in the app's classloader namespace, which
        // can only load .so files from inside the app's permitted_paths
        // (/data/data/<pkg>/, plus a few system locations). External storage and
        // SAF tree paths are NOT in that list, so an .so placed under
        // getExternalFilesDir() or a user-picked SD card path will fail with
        // "library ... is not accessible for the namespace classloader-namespace".
        //
        // Copy into context.getFilesDir()/drivers/ (always permitted) and return
        // THAT path — that's what the renderer should dlopen. The original .so
        // stays in the user's managed folder for reference; we don't need to
        // delete it.
        File driverDir = new File(context.getFilesDir(), DRIVER_DIR_NAME);
        if (!driverDir.exists() && !driverDir.mkdirs()) {
            throw new IOException("Failed to create drivers directory");
        }

        if (lowerName.endsWith(".so")) {
            File destFile = new File(driverDir, sanitizeName(displayName));
            copyUriToFile(context, uri, destFile);
            return destFile.getAbsolutePath();
        } else if (lowerName.endsWith(".zip")) {
            // Extract .so from ZIP
            android.util.Log.d("SafFileImporter", "Attempting to extract driver from ZIP: " + displayName);
            try (InputStream is = context.getContentResolver().openInputStream(uri);
                 java.util.zip.ZipInputStream zis = new java.util.zip.ZipInputStream(is)) {

                java.util.zip.ZipEntry entry;
                while ((entry = zis.getNextEntry()) != null) {
                    android.util.Log.d("SafFileImporter", "ZIP entry: " + entry.getName());
                    if (!entry.isDirectory() && entry.getName().toLowerCase().endsWith(".so")) {
                        // Found a driver, extract it
                        String fileName = new File(entry.getName()).getName();
                        File destFile = new File(driverDir, sanitizeName(fileName));
                        android.util.Log.d("SafFileImporter", "Extracting driver to: " + destFile.getAbsolutePath());
                        
                        try (FileOutputStream fos = new FileOutputStream(destFile)) {
                            byte[] buffer = new byte[8192];
                            int len;
                            while ((len = zis.read(buffer)) != -1) {
                                fos.write(buffer, 0, len);
                            }
                        }
                        zis.closeEntry();
                        return destFile.getAbsolutePath();
                    }
                    zis.closeEntry();
                }
            } catch (Exception e) {
                android.util.Log.e("SafFileImporter", "ZIP extraction failed", e);
                throw new IOException("ZIP extraction failed: " + e.getMessage());
            }
            throw new IOException("No .so driver file found in ZIP");
        } else {
            throw new IOException("Selected file is not a Vulkan driver (.so) or ZIP containing one");
        }
    }

    public static String importBios(Context context, Uri uri) throws IOException {
        takePersistableReadPermission(context, uri);

        String displayName = getDisplayName(context, uri);
        if (!isBiosFileName(displayName)) {
            throw new IOException("Selected file is not a BIOS image");
        }

        File biosDir = getManagedBiosDirectory(context);
        if (!biosDir.exists() && !biosDir.mkdirs()) {
            throw new IOException("Failed to create BIOS directory");
        }

        File destFile = new File(biosDir, sanitizeName(displayName));
        copyUriToFile(context, uri, destFile);
        return destFile.getAbsolutePath();
    }

    static String importGameFile(Context context, Uri uri) throws IOException {
        takePersistableReadPermission(context, uri);

        String displayName = getDisplayName(context, uri);
        if (!isSupportedLibraryFileName(displayName)) {
            throw new IOException("Selected file is not a supported game image or archive");
        }

        File libraryDir = getManagedLibraryDirectory(context);
        if (!libraryDir.exists() && !libraryDir.mkdirs()) {
            throw new IOException("Failed to create library directory");
        }

        File destFile = new File(libraryDir, sanitizeName(displayName));
        copyUriToFile(context, uri, destFile);
        return destFile.getAbsolutePath();
    }

    public static ImportResult importLibraryTree(Context context, Uri uri) throws IOException {
        takePersistableReadPermission(context, uri);

        DocumentFile tree = DocumentFile.fromTreeUri(context, uri);
        if (tree == null || !tree.isDirectory()) {
            throw new IOException("Selected location is not a readable folder");
        }

        String treeName = tree.getName();
        if (treeName == null || treeName.trim().isEmpty()) {
            treeName = "Imported Library";
        }

        File libraryRoot = getManagedLibraryRoot(context);
        if (!libraryRoot.exists() && !libraryRoot.mkdirs()) {
            throw new IOException("Failed to create managed library root");
        }

        File destinationDir = new File(libraryRoot, sanitizeName(treeName));
        int importedCount = copySupportedTreeFiles(context, tree, destinationDir);
        if (importedCount == 0) {
            throw new IOException("No supported game files or archives were found in the selected folder");
        }

        return new ImportResult(destinationDir.getAbsolutePath(), importedCount);
    }

    /**
     * Adopt a pre-built "Retro-3DO" folder (containing bios/ and optionally
     * games/) picked via SAF, copying its contents into app-specific storage.
     * Returns the imported BIOS path and (optional) library path. Uses
     * DocumentFile/content URIs — no raw /storage access or all-files permission.
     */
    public static AdoptResult adoptExistingTree(Context context, Uri treeUri) throws IOException {
        takePersistableReadPermission(context, treeUri);
        DocumentFile tree = DocumentFile.fromTreeUri(context, treeUri);
        if (tree == null || !tree.isDirectory()) {
            throw new IOException("Selected location is not a readable folder");
        }
        DocumentFile biosDir = tree.findFile("bios");
        if (biosDir == null || !biosDir.isDirectory()) {
            throw new IOException("That folder has no 'bios' subfolder — pick a folder containing bios/ and games/");
        }
        DocumentFile chosenBios = null;
        DocumentFile[] biosChildren = biosDir.listFiles();
        for (DocumentFile f : biosChildren) {
            if (!f.isFile()) continue;
            String n = f.getName() == null ? "" : f.getName().toLowerCase();
            if (n.endsWith(".bin") || n.endsWith(".rom")) {
                if (n.contains("panafz10")) { chosenBios = f; break; }
                if (chosenBios == null) chosenBios = f;
            }
        }
        if (chosenBios == null) {
            throw new IOException("No BIOS (.bin/.rom) found in the folder's bios/");
        }
        File biosDestDir = getManagedBiosDirectory(context);
        if (!biosDestDir.exists() && !biosDestDir.mkdirs()) {
            throw new IOException("Failed to create BIOS directory");
        }
        File biosDest = new File(biosDestDir, sanitizeName(
                chosenBios.getName() == null ? "bios.bin" : chosenBios.getName()));
        copyUriToFile(context, chosenBios.getUri(), biosDest);

        String libraryPath = null;
        DocumentFile gamesDir = tree.findFile("games");
        if (gamesDir != null && gamesDir.isDirectory()) {
            String name = gamesDir.getName();
            File destDir = new File(getManagedAppRoot(context),
                    sanitizeName(name == null || name.isEmpty() ? "games" : name));
            int imported = copySupportedTreeFiles(context, gamesDir, destDir);
            if (imported > 0) {
                libraryPath = destDir.getAbsolutePath();
            }
        }
        return new AdoptResult(biosDest.getAbsolutePath(), libraryPath);
    }

    /** Result of {@link #adoptExistingTree}. */
    public static final class AdoptResult {
        public final String biosPath;
        public final String libraryPath; // null if no games/ folder
        AdoptResult(String biosPath, String libraryPath) {
            this.biosPath = biosPath;
            this.libraryPath = libraryPath;
        }
    }

    public static File getManagedLibraryDirectory(Context context) {
        return new File(getManagedAppRoot(context), LIBRARY_DIR_NAME);
    }

    static File getManagedBiosDirectory(Context context) {
        return new File(getManagedAppRoot(context), BIOS_DIR_NAME);
    }

    static File getManagedAppDataDirectory(Context context) {
        return new File(getManagedAppRoot(context), APPDATA_DIR_NAME);
    }

    static File getManagedDriverDirectory(Context context) {
        return new File(getManagedAppDataDirectory(context), DRIVER_DIR_NAME);
    }

    static File getManagedAppRoot(Context context) {
        SharedStorageRoot sharedStorageRoot = getSharedStorageRoot(context);
        return sharedStorageRoot.root;
    }

    private static File getManagedLibraryRoot(Context context) {
        return getManagedAppRoot(context);
    }

    /**
     * The managed root is always app-specific external storage (falling back to
     * internal). App-specific dirs are readable/writable by raw {@link File}
     * path on every Android version WITHOUT the MANAGE_EXTERNAL_STORAGE /
     * "All files access" permission, so all imported BIOS/library/game copies
     * live here and the library can be scanned and loaded by path. A custom
     * arbitrary /storage path is intentionally NOT honoured — that is what used
     * to require all-files access.
     */
    private static SharedStorageRoot getSharedStorageRoot(Context context) {
        File root = context.getExternalFilesDir(null);
        if (root == null) {
            root = context.getFilesDir();
        }
        return new SharedStorageRoot(root);
    }

    /** Absolute path of the managed (app-specific) storage root. */
    static String getManagedAppRootPath(Context context) {
        return getManagedAppRoot(context).getAbsolutePath();
    }

    private static int copySupportedTreeFiles(Context context, DocumentFile sourceDir, File destDir) throws IOException {
        DocumentFile[] children = sourceDir.listFiles();
        int importedCount = 0;

        for (DocumentFile child : children) {
            String childName = child.getName();
            if (childName == null || childName.startsWith(".")) {
                continue;
            }

            if (child.isDirectory()) {
                File childDest = new File(destDir, sanitizeName(childName));
                importedCount += copySupportedTreeFiles(context, child, childDest);
                continue;
            }

            if (!child.isFile() || !isSupportedLibraryFileName(childName)) {
                continue;
            }

            if (!destDir.exists() && !destDir.mkdirs()) {
                throw new IOException("Failed to create directory: " + destDir.getAbsolutePath());
            }

            File destFile = new File(destDir, sanitizeName(childName));
            copyUriToFile(context, child.getUri(), destFile);
            importedCount++;
        }

        return importedCount;
    }

    private static void copyUriToFile(Context context, Uri uri, File destFile) throws IOException {
        ContentResolver resolver = context.getContentResolver();
        File parent = destFile.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            throw new IOException("Failed to create directory: " + parent.getAbsolutePath());
        }

        if (parent != null) {
            File canonicalParent = parent.getCanonicalFile();
            File canonicalDest = destFile.getCanonicalFile();
            String parentPath = canonicalParent.getPath() + File.separator;
            if (!canonicalDest.getPath().startsWith(parentPath)) {
                throw new IOException("Invalid destination file path");
            }
        }

        try (InputStream inputStream = resolver.openInputStream(uri);
             FileOutputStream outputStream = new FileOutputStream(destFile)) {
            if (inputStream == null) {
                throw new IOException("Could not open selected file");
            }

            byte[] buffer = new byte[8192];
            int bytesRead;
            while ((bytesRead = inputStream.read(buffer)) != -1) {
                outputStream.write(buffer, 0, bytesRead);
            }
        }
    }

    private static String getDisplayName(Context context, Uri uri) {
        ContentResolver resolver = context.getContentResolver();
        try (android.database.Cursor cursor = resolver.query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (nameIndex >= 0) {
                    String displayName = cursor.getString(nameIndex);
                    if (displayName != null && !displayName.trim().isEmpty()) {
                        return displayName;
                    }
                }
            }
        }

        String fallback = uri.getLastPathSegment();
        if (fallback == null || fallback.trim().isEmpty()) {
            fallback = "imported-file";
        }
        return fallback;
    }

    private static void takePersistableReadPermission(Context context, Uri uri) {
        try {
            context.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
        }
        try {
            context.getContentResolver().takePersistableUriPermission(uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        } catch (SecurityException ignored) {
        }
    }

    private static boolean isBiosFileName(String name) {
        String lower = safeLowerName(name);
        return lower.endsWith(".bin") || lower.endsWith(".rom");
    }

    private static boolean isSupportedLibraryFileName(String name) {
        String lower = safeLowerName(name);
        return lower.endsWith(".cue")
                || lower.endsWith(".iso")
                || lower.endsWith(".chd")
                || lower.endsWith(".bin")
                || lower.endsWith(".img")
                || lower.endsWith(".sub")
                || lower.endsWith(".ccd")
                || lower.endsWith(".mdf")
                || lower.endsWith(".mds")
                || lower.endsWith(".wav")
                || lower.endsWith(".wv")
                || lower.endsWith(".zip")
                || lower.endsWith(".7z")
                || lower.endsWith(".rar");
    }

    private static String safeLowerName(String name) {
        return name == null ? "" : name.toLowerCase(Locale.ROOT);
    }

    private static String sanitizeName(String name) {
        String raw = name == null ? "" : name.trim();
        if (raw.contains("..") || raw.contains("/") || raw.contains("\\")) {
            throw new IllegalArgumentException("Invalid file name");
        }
        String sanitized = raw.replaceAll("[\\\\/:*?\"<>|]", "_").trim();
        return sanitized.isEmpty() ? "imported" : sanitized;
    }

    private static final class SharedStorageRoot {
        final File root;

        SharedStorageRoot(File root) {
            this.root = root;
        }
    }
}
