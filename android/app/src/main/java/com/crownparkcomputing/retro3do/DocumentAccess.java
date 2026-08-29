package com.crownparkcomputing.retro3do;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;

/**
 * Scoped folder access through the Storage Access Framework.
 *
 * The user picks the folders their games live in and the app gets read access to
 * exactly those, persisted across restarts. Deliberately NOT all-files access:
 * that is a sensitive permission requiring a Play declaration and review, and
 * this app does not need it — it needs the folder the user points at.
 *
 * The consequence, which shapes the native side: SAF hands out content URIs, not
 * paths. Nothing here can be fopen'd. Files are read by asking the system for a
 * descriptor and passing that down, which is why {@code Disc} grew an
 * open-by-descriptor path.
 */
public final class DocumentAccess {

    private static final String TAG = "Retro3DO";
    private static final String PREFS = "retro3do.storage";
    private static final String KEY_ROOTS = "granted_roots";

    public static final int REQUEST_PICK_FOLDER = 0x3D0F;

    private final Activity activity;

    public DocumentAccess(Activity activity) {
        this.activity = activity;
    }

    /** Show the system folder picker. The result arrives in onActivityResult. */
    public void pickFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        activity.startActivityForResult(intent, REQUEST_PICK_FOLDER);
    }

    /**
     * Record a granted folder. The permission must be taken persistably here and
     * now: without takePersistableUriPermission the grant dies with the process,
     * and the app silently forgets the user's library on every restart — which
     * looks like a bug in the app rather than a missing call.
     */
    public boolean onFolderGranted(Uri treeUri) {
        if (treeUri == null) {
            return false;
        }
        try {
            activity.getContentResolver().takePersistableUriPermission(
                    treeUri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            Log.w(TAG, "Could not persist access to " + treeUri, e);
            return false;
        }

        SharedPreferences prefs =
                activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String stored = prefs.getString(KEY_ROOTS, "");
        String encoded = treeUri.toString();
        if (!stored.contains(encoded)) {
            stored = stored.isEmpty() ? encoded : stored + "\n" + encoded;
            prefs.edit().putString(KEY_ROOTS, stored).apply();
        }
        return true;
    }

    /** The folders the user has granted, as tree URI strings. */
    public String[] grantedRoots() {
        SharedPreferences prefs =
                activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String stored = prefs.getString(KEY_ROOTS, "");
        if (stored.isEmpty()) {
            return new String[0];
        }

        // A grant can be revoked from system settings, or the folder removed.
        // Only report the ones still actually held, or the browser offers
        // entries that fail the moment they are opened.
        List<String> live = new ArrayList<>();
        for (String candidate : stored.split("\n")) {
            if (candidate.isEmpty()) continue;
            for (android.content.UriPermission held :
                    activity.getContentResolver().getPersistedUriPermissions()) {
                if (held.getUri().toString().equals(candidate) && held.isReadPermission()) {
                    live.add(candidate);
                    break;
                }
            }
        }
        return live.toArray(new String[0]);
    }

    public void forgetRoot(String treeUri) {
        SharedPreferences prefs =
                activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        StringBuilder kept = new StringBuilder();
        for (String candidate : prefs.getString(KEY_ROOTS, "").split("\n")) {
            if (candidate.isEmpty() || candidate.equals(treeUri)) continue;
            if (kept.length() > 0) kept.append('\n');
            kept.append(candidate);
        }
        prefs.edit().putString(KEY_ROOTS, kept.toString()).apply();

        try {
            activity.getContentResolver().releasePersistableUriPermission(
                    Uri.parse(treeUri), Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception e) {
            Log.w(TAG, "Could not release " + treeUri, e);
        }
    }

    /** A human-readable name for a granted folder, for the browser's shortcuts. */
    public String displayNameOf(String uri) {
        try {
            Uri tree = Uri.parse(uri);
            String documentId = DocumentsContract.getTreeDocumentId(tree);
            // Document ids look like "primary:Games/3DO"; the part after the
            // colon is what a person recognises.
            int colon = documentId.indexOf(':');
            String readable = colon >= 0 ? documentId.substring(colon + 1) : documentId;
            if (readable.isEmpty()) {
                return "Storage";
            }
            return readable;
        } catch (Exception e) {
            return "Folder";
        }
    }

    /**
     * List one folder. Each entry is "D|name|documentUri" or "F|name|documentUri",
     * newline separated — flat text because it crosses to C++ and a string is
     * far less to get wrong than a structured JNI return.
     *
     * @param uri a tree URI (a granted root) or a document URI of a subfolder.
     */
    public String listFolder(String uri) {
        StringBuilder out = new StringBuilder();
        try {
            Uri parsed = Uri.parse(uri);

            // A tree URI and a document URI need different starting points, and
            // using the wrong one returns an empty list rather than an error.
            String documentId;
            if (DocumentsContract.isDocumentUri(activity, parsed)) {
                documentId = DocumentsContract.getDocumentId(parsed);
            } else {
                documentId = DocumentsContract.getTreeDocumentId(parsed);
            }

            Uri children =
                    DocumentsContract.buildChildDocumentsUriUsingTree(parsed, documentId);

            List<String[]> rows = new ArrayList<>();
            try (Cursor cursor = activity.getContentResolver().query(children,
                    new String[] {
                            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE,
                    }, null, null, null)) {
                if (cursor == null) {
                    return "";
                }
                while (cursor.moveToNext()) {
                    String childId = cursor.getString(0);
                    String name = cursor.getString(1);
                    String mime = cursor.getString(2);
                    boolean isDirectory =
                            DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
                    Uri childUri =
                            DocumentsContract.buildDocumentUriUsingTree(parsed, childId);
                    rows.add(new String[] {
                            isDirectory ? "D" : "F", name, childUri.toString() });
                }
            }

            // Folders first, then files, each alphabetically.
            Collections.sort(rows, new Comparator<String[]>() {
                @Override
                public int compare(String[] a, String[] b) {
                    if (!a[0].equals(b[0])) return a[0].equals("D") ? -1 : 1;
                    return a[1].compareToIgnoreCase(b[1]);
                }
            });

            for (String[] row : rows) {
                out.append(row[0]).append('|').append(row[1]).append('|')
                   .append(row[2]).append('\n');
            }
        } catch (Exception e) {
            Log.w(TAG, "Could not list " + uri, e);
        }
        return out.toString();
    }

    /**
     * Open a document for reading and hand back a raw descriptor.
     *
     * The descriptor is detached, so the native side owns it and must close it.
     * Returning the ParcelFileDescriptor's fd without detaching would let Java
     * close it underneath C++ at the next garbage collection — an intermittent
     * read failure that would be extremely hard to attribute.
     */
    public int openDocument(String documentUri) {
        try {
            ParcelFileDescriptor pfd = activity.getContentResolver()
                    .openFileDescriptor(Uri.parse(documentUri), "r");
            if (pfd == null) {
                return -1;
            }
            return pfd.detachFd();
        } catch (Exception e) {
            Log.w(TAG, "Could not open " + documentUri, e);
            return -1;
        }
    }
}
