package com.crownparkcomputing.retro3do;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

/**
 * The Android entry point.
 *
 * Everything the emulator does lives in the native library; this class exists so
 * that Android-specific concerns — storage access, lifecycle, intents — have
 * somewhere to go that is not SDL's own activity. It is deliberately thin, and
 * should stay that way: logic that ends up here has to be written again for iOS.
 *
 * The methods below are called from C++ by name (see platform/android_storage.cpp).
 * They are static so the native side does not need an object reference, with a
 * single instance parked at creation — SDL only ever creates one activity.
 */
public class Retro3DOActivity extends SDLActivity {

    private static Retro3DOActivity instance;
    private DocumentAccess documents;

    @Override
    protected String[] getLibraries() {
        // Loaded in order. SDL first, because our library links against it.
        return new String[] {
            "SDL3",
            "main"
        };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        instance = this;
        documents = new DocumentAccess(this);
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == DocumentAccess.REQUEST_PICK_FOLDER) {
            if (resultCode == RESULT_OK && data != null) {
                Uri tree = data.getData();
                documents.onFolderGranted(tree);
            }
            // A cancelled pick is not an error; the browser simply shows no new
            // folder and the user can try again.
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    // --- called from native ------------------------------------------------

    public static void nativePickFolder() {
        final Retro3DOActivity self = instance;
        if (self == null) return;
        // The picker must be started from the UI thread; the emulator's thread
        // is what calls in here.
        self.runOnUiThread(new Runnable() {
            @Override public void run() { self.documents.pickFolder(); }
        });
    }

    public static String nativeGrantedRoots() {
        if (instance == null) return "";
        String[] roots = instance.documents.grantedRoots();
        StringBuilder out = new StringBuilder();
        for (String root : roots) {
            out.append(instance.documents.displayNameOf(root)).append('|')
               .append(root).append('\n');
        }
        return out.toString();
    }

    public static String nativeListFolder(String uri) {
        if (instance == null) return "";
        return instance.documents.listFolder(uri);
    }

    public static int nativeOpenDocument(String uri) {
        if (instance == null) return -1;
        return instance.documents.openDocument(uri);
    }

    public static void nativeForgetRoot(String uri) {
        if (instance == null) return;
        instance.documents.forgetRoot(uri);
    }
}
