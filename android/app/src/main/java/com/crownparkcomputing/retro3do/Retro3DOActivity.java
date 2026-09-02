package com.crownparkcomputing.retro3do;

import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;
import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

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

    private static final String TAG = "Retro3DO";
    private static final float MAX_PRESENTATION_HZ = 120.0f;
    private static final int REQUEST_GPU_DRIVER = 7302;
    private static final long MAX_GPU_PACKAGE_BYTES = 512L * 1024L * 1024L;
    private static final String RETROMEDIA_URL =
        "https://media.crownparkcomputing.com";
    private static final String RETROMEDIA_PREFS = "retromedia_private";
    private static final String RETROMEDIA_KEY_ALIAS =
        "com.crownparkcomputing.retro3do.retromedia.session";
    private static final int MAX_RETROMEDIA_REPLY_BYTES = 4 * 1024 * 1024;
    private static final int MAX_RETROMEDIA_ART_BYTES = 32 * 1024 * 1024;
    private static final Set<String> RETROMEDIA_CARD_TYPES = new HashSet<String>();
    static {
        RETROMEDIA_CARD_TYPES.add("box2d");
        RETROMEDIA_CARD_TYPES.add("images");
        RETROMEDIA_CARD_TYPES.add("thumbnails");
        RETROMEDIA_CARD_TYPES.add("titles");
    }

    private static Retro3DOActivity instance;
    private DocumentAccess documents;
    // Consumed once by the native setup wizard after the asynchronous system
    // picker returns. Keeping the actual URI avoids guessing which persisted
    // grant is new when the user selects a folder that was already granted.
    private String pickedFolder;
    private volatile String gpuDriverImportResult;
    private volatile String retroMediaResult;
    private volatile String retroMediaCatalogueResult = "";
    private volatile boolean retroMediaBusy;

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
        requestFastestDisplayMode();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Android may restore its default mode while the activity is paused.
        // Reassert the request when returning to the foreground.
        requestFastestDisplayMode();
    }

    /**
     * Ask Android for the fastest native panel mode no higher than 120 Hz.
     *
     * The emulated 3DO remains at its real 50/60 Hz field rate. A faster panel
     * makes presentation and touch/UI response smoother; it must not make the
     * machine or its audio run fast. SDL's renderer stays vsynced to whichever
     * mode Android grants, so this also retains tear-free presentation.
     */
    @SuppressWarnings("deprecation") // Required below API 30; minSdk is 24.
    private void requestFastestDisplayMode() {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        Display.Mode best = current;

        for (Display.Mode candidate : display.getSupportedModes()) {
            // Do not trade resolution for refresh rate. Rotation does not
            // change these physical mode dimensions.
            if (candidate.getPhysicalWidth() != current.getPhysicalWidth() ||
                candidate.getPhysicalHeight() != current.getPhysicalHeight()) {
                continue;
            }
            final float hz = candidate.getRefreshRate();
            if (hz <= MAX_PRESENTATION_HZ + 0.01f &&
                hz > best.getRefreshRate() + 0.01f) {
                best = candidate;
            }
        }

        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredDisplayModeId = best.getModeId();
        attributes.preferredRefreshRate = best.getRefreshRate();
        getWindow().setAttributes(attributes);
        Log.i(TAG, "Requested display mode " + best.getPhysicalWidth() + "x" +
                   best.getPhysicalHeight() + " @ " + best.getRefreshRate() + " Hz");
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == DocumentAccess.REQUEST_PICK_FOLDER) {
            if (resultCode == RESULT_OK && data != null) {
                Uri tree = data.getData();
                documents.onFolderGranted(tree);
                pickedFolder = tree != null ? tree.toString() : null;
            }
            // A cancelled pick is not an error; the browser simply shows no new
            // folder and the user can try again.
            return;
        }
        if (requestCode == REQUEST_GPU_DRIVER) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                final Uri packageUri = data.getData();
                gpuDriverImportResult = null;
                new Thread(new Runnable() {
                    @Override public void run() { importGpuDriver(packageUri); }
                }, "Retro3DO-driver-import").start();
            }
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

    public static String nativeConsumePickedFolder() {
        if (instance == null || instance.pickedFolder == null) return "";
        String uri = instance.pickedFolder;
        instance.pickedFolder = null;
        return instance.documents.displayNameOf(uri) + "|" + uri;
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

    public static void nativePickGpuDriver() {
        final Retro3DOActivity self = instance;
        if (self == null) return;
        self.runOnUiThread(new Runnable() {
            @Override public void run() {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("application/zip");
                intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {
                    "application/zip", "application/x-zip-compressed",
                    "application/octet-stream"
                });
                self.startActivityForResult(intent, REQUEST_GPU_DRIVER);
            }
        });
    }

    public static String nativeConsumeGpuDriverImport() {
        if (instance == null || instance.gpuDriverImportResult == null) return "";
        String result = instance.gpuDriverImportResult;
        instance.gpuDriverImportResult = null;
        return result;
    }

    public static String nativeLibraryDirectory() {
        if (instance == null) return "";
        return instance.getApplicationInfo().nativeLibraryDir;
    }

    public static void nativeRetroMediaStatus() {
        final Retro3DOActivity self = instance;
        if (self == null) return;
        self.startRetroMediaTask("STATUS", new RetroMediaWork() {
            @Override public void run() throws Exception { self.retroMediaStatus(); }
        });
    }

    public static void nativeRetroMediaLogin(final String email,
                                             final String password) {
        final Retro3DOActivity self = instance;
        if (self == null) return;
        self.startRetroMediaTask("LOGIN", new RetroMediaWork() {
            @Override public void run() throws Exception {
                self.retroMediaLogin(email, password);
            }
        });
    }

    public static void nativeRetroMediaLogout() {
        final Retro3DOActivity self = instance;
        if (self == null) return;
        self.startRetroMediaTask("LOGOUT", new RetroMediaWork() {
            @Override public void run() throws Exception { self.retroMediaLogout(); }
        });
    }

    public static void nativeRetroMediaSync(final String gameNames,
                                            final String mediaType) {
        final Retro3DOActivity self = instance;
        if (self == null) return;
        self.startRetroMediaTask("SYNC", new RetroMediaWork() {
            @Override public void run() throws Exception {
                self.retroMediaSync(gameNames, mediaType);
            }
        });
    }

    public static void nativeRetroMediaCatalogue(final String search,
                                                 String ignored) {
        final Retro3DOActivity self = instance;
        if (self == null) return;
        self.startRetroMediaTask("CATALOGUE", new RetroMediaWork() {
            @Override public void run() throws Exception {
                self.retroMediaBrowseCatalogue(search);
            }
        });
    }

    public static String nativeRetroMediaCatalogueResult() {
        return instance == null ? "" : instance.retroMediaCatalogueResult;
    }

    public static void nativeRetroMediaDownload(final String slug,
                                                final String gamesFolder) {
        final Retro3DOActivity self = instance;
        if (self == null) return;
        self.startRetroMediaTask("DOWNLOAD", new RetroMediaWork() {
            @Override public void run() throws Exception {
                self.retroMediaDownloadRom(slug, gamesFolder);
            }
        });
    }

    public static String nativeConsumeRetroMediaResult() {
        if (instance == null || instance.retroMediaResult == null) return "";
        String result = instance.retroMediaResult;
        instance.retroMediaResult = null;
        return result;
    }

    public static String nativeRetroMediaArtwork(String mediaType) {
        if (instance == null) return "";
        return instance.retroMediaArtwork(mediaType);
    }

    public static String nativeRetroMediaSavedEmail() {
        if (instance == null) return "";
        return instance.retroMediaPreferences().getString("email", "");
    }

    private static String safeResultField(String text) {
        if (text == null) return "";
        return text.replace('|', ' ').replace('\n', ' ').replace('\r', ' ');
    }

    private static void deleteTree(File file) {
        if (file == null || !file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) deleteTree(child);
        }
        // Only ever called for the newly-created private import directory.
        if (!file.delete()) Log.w(TAG, "Could not remove failed driver import " + file);
    }

    private static boolean isArm64Elf(File file) {
        byte[] header = new byte[20];
        try (FileInputStream input = new FileInputStream(file)) {
            if (input.read(header) != header.length) return false;
        } catch (Exception error) {
            return false;
        }
        // ELF64, little endian, EM_AARCH64 (183 / 0xB7).
        return (header[0] & 0xff) == 0x7f && header[1] == 'E' &&
               header[2] == 'L' && header[3] == 'F' && header[4] == 2 &&
               header[5] == 1 && (header[18] & 0xff) == 0xb7 && header[19] == 0;
    }

    private void importGpuDriver(Uri packageUri) {
        if (Build.VERSION.SDK_INT < 28) {
            gpuDriverImportResult = "ERROR|Custom drivers require Android 9 or newer";
            return;
        }

        File destination = new File(new File(getFilesDir(), "gpu-drivers"),
                                    "driver-" + System.currentTimeMillis());
        try {
            if (!destination.mkdirs()) throw new Exception("Cannot create private driver folder");

            JSONObject metadata = null;
            ArrayList<String> libraries = new ArrayList<>();
            long totalBytes = 0;
            int entryCount = 0;
            byte[] buffer = new byte[64 * 1024];

            try (InputStream source = getContentResolver().openInputStream(packageUri);
                 ZipInputStream zip = new ZipInputStream(source)) {
                if (source == null) throw new Exception("Cannot open selected package");
                ZipEntry entry;
                while ((entry = zip.getNextEntry()) != null) {
                    if (++entryCount > 128) throw new Exception("Driver package has too many files");
                    if (entry.isDirectory()) continue;

                    String name = entry.getName();
                    if (name == null || name.isEmpty() || name.contains("/") ||
                        name.contains("\\") || name.contains("..")) {
                        throw new Exception("Driver package contains an unsafe path");
                    }
                    boolean isMetadata = name.equals("meta.json");
                    boolean isLibrary = name.endsWith(".so");
                    if (!isMetadata && !isLibrary) continue;

                    ByteArrayOutputStream metadataBytes =
                        isMetadata ? new ByteArrayOutputStream() : null;
                    File output = isLibrary ? new File(destination, name) : null;
                    try (FileOutputStream libraryOutput =
                             isLibrary ? new FileOutputStream(output) : null) {
                        long fileBytes = 0;
                        int count;
                        while ((count = zip.read(buffer)) > 0) {
                            fileBytes += count;
                            totalBytes += count;
                            if (fileBytes > 256L * 1024L * 1024L ||
                                totalBytes > MAX_GPU_PACKAGE_BYTES) {
                                throw new Exception("Driver package is too large");
                            }
                            if (isMetadata) {
                                if (fileBytes > 1024L * 1024L) {
                                    throw new Exception("Driver metadata is too large");
                                }
                                metadataBytes.write(buffer, 0, count);
                            } else {
                                libraryOutput.write(buffer, 0, count);
                            }
                        }
                    }

                    if (isMetadata) {
                        metadata = new JSONObject(
                            new String(metadataBytes.toByteArray(), StandardCharsets.UTF_8));
                    } else {
                        if (!isArm64Elf(output)) {
                            throw new Exception(name + " is not an ARM64 Android library");
                        }
                        output.setReadable(true, true);
                        output.setExecutable(true, true);
                        libraries.add(name);
                    }
                }
            }

            if (metadata == null) throw new Exception("Package has no meta.json");
            if (metadata.optInt("schemaVersion", 0) != 1) {
                throw new Exception("Unsupported driver package schema");
            }
            if (metadata.optInt("minApi", 0) > Build.VERSION.SDK_INT) {
                throw new Exception("Driver requires a newer Android version");
            }

            String libraryName = metadata.optString("libraryName", "");
            if (libraryName.isEmpty() || !libraries.contains(libraryName)) {
                libraryName = "";
                for (String candidate : libraries) {
                    if (candidate.equals("libvulkan_freedreno.so") ||
                        candidate.startsWith("vulkan.") ||
                        candidate.toLowerCase().contains("vulkan")) {
                        libraryName = candidate;
                        break;
                    }
                }
            }
            if (libraryName.isEmpty()) throw new Exception("Package has no Vulkan driver library");

            String name = metadata.optString("name", "Custom Adreno driver");
            String version = metadata.optString("driverVersion", "");
            if (!version.isEmpty()) name += " " + version;
            gpuDriverImportResult = "OK|" + safeResultField(name) + "|" +
                                    destination.getAbsolutePath() + File.separator + "|" +
                                    libraryName;
            Log.i(TAG, "Imported GPU driver " + name + " / " + libraryName);
        } catch (Exception error) {
            deleteTree(destination);
            gpuDriverImportResult = "ERROR|" + safeResultField(error.getMessage());
            Log.e(TAG, "GPU driver import failed", error);
        }
    }

    // --- RetroMedia -------------------------------------------------------

    private interface RetroMediaWork { void run() throws Exception; }

    private static final class HttpReply {
        int status;
        byte[] body;
        String cookie;
    }

    private void startRetroMediaTask(final String operation,
                                     final RetroMediaWork work) {
        synchronized (this) {
            if (retroMediaBusy) {
                retroMediaResult = retroMediaResult(false, operation, "", 0, 0,
                    0, 0, false, "Another RetroMedia operation is still running");
                return;
            }
            retroMediaBusy = true;
            retroMediaResult = null;
        }
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    work.run();
                } catch (Exception error) {
                    retroMediaResult = retroMediaResult(false, operation, "", 0,
                        0, 0, 0, false, usefulError(error));
                    Log.e(TAG, "RetroMedia " + operation + " failed", error);
                } finally {
                    retroMediaBusy = false;
                }
            }
        }, "Retro3DO-retromedia-" + operation.toLowerCase(Locale.ROOT)).start();
    }

    private static String usefulError(Exception error) {
        String message = error.getMessage();
        return message == null || message.trim().isEmpty()
            ? error.getClass().getSimpleName() : message;
    }

    private static String retroMediaResult(boolean ok, String operation,
                                           String email, int credits,
                                           int freeRemaining, int matched,
                                           int downloaded, boolean isAdmin,
                                           String message) {
        return (ok ? "OK" : "ERROR") + "|" + safeResultField(operation) + "|" +
               safeResultField(email) + "|" + credits + "|" + freeRemaining +
               "|" + matched + "|" + downloaded + "|" +
               (isAdmin ? "1" : "0") + "|" +
               safeResultField(message);
    }

    private SharedPreferences retroMediaPreferences() {
        return getSharedPreferences(RETROMEDIA_PREFS, MODE_PRIVATE);
    }

    private SecretKey retroMediaSecretKey() throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (store.containsAlias(RETROMEDIA_KEY_ALIAS)) {
            return ((KeyStore.SecretKeyEntry) store.getEntry(
                RETROMEDIA_KEY_ALIAS, null)).getSecretKey();
        }
        KeyGenerator generator = KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(
            RETROMEDIA_KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .build());
        return generator.generateKey();
    }

    private String encryptSession(String session) throws Exception {
        Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
        cipher.init(Cipher.ENCRYPT_MODE, retroMediaSecretKey());
        byte[] encrypted = cipher.doFinal(session.getBytes(StandardCharsets.UTF_8));
        return Base64.encodeToString(cipher.getIV(), Base64.NO_WRAP) + "." +
               Base64.encodeToString(encrypted, Base64.NO_WRAP);
    }

    private String decryptSession(String stored) {
        try {
            int separator = stored.indexOf('.');
            if (separator <= 0) return "";
            byte[] iv = Base64.decode(stored.substring(0, separator), Base64.NO_WRAP);
            byte[] encrypted = Base64.decode(stored.substring(separator + 1),
                                             Base64.NO_WRAP);
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, retroMediaSecretKey(),
                        new GCMParameterSpec(128, iv));
            return new String(cipher.doFinal(encrypted), StandardCharsets.UTF_8);
        } catch (Exception error) {
            Log.w(TAG, "Stored RetroMedia session is no longer readable", error);
            clearRetroMediaSession();
            return "";
        }
    }

    private void saveRetroMediaSession(String cookie, String email) throws Exception {
        retroMediaPreferences().edit()
            .putString("session", encryptSession(cookie))
            .putString("email", email == null ? "" : email)
            .apply();
    }

    private String loadRetroMediaSession() {
        String stored = retroMediaPreferences().getString("session", "");
        return stored.isEmpty() ? "" : decryptSession(stored);
    }

    private void clearRetroMediaSession() {
        // Keep the non-secret email as a convenience for the next sign-in.
        // The password is never persisted and the revocable session is gone.
        retroMediaPreferences().edit().remove("session").apply();
    }

    private static byte[] readLimited(InputStream input, int maximum)
        throws Exception {
        if (input == null) return new byte[0];
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[64 * 1024];
        int total = 0;
        int count;
        while ((count = input.read(buffer)) > 0) {
            total += count;
            if (total > maximum) throw new Exception("RetroMedia response is too large");
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    private HttpReply retroMediaRequest(String method, String address,
                                        JSONObject body, String cookie,
                                        int maximumBytes) throws Exception {
        HttpURLConnection connection =
            (HttpURLConnection) new URL(address).openConnection();
        connection.setConnectTimeout(15000);
        connection.setReadTimeout(60000);
        connection.setRequestMethod(method);
        connection.setRequestProperty("Accept", "application/json, image/*, application/zip");
        connection.setRequestProperty("User-Agent", "Retro-3DO/2.1 RetroMedia client");
        if (cookie != null && !cookie.isEmpty()) {
            connection.setRequestProperty("Cookie", cookie);
        }
        if (body != null) {
            byte[] encoded = body.toString().getBytes(StandardCharsets.UTF_8);
            connection.setDoOutput(true);
            connection.setRequestProperty("Content-Type", "application/json; charset=utf-8");
            connection.setFixedLengthStreamingMode(encoded.length);
            try (OutputStream output = connection.getOutputStream()) {
                output.write(encoded);
            }
        }

        HttpReply reply = new HttpReply();
        reply.status = connection.getResponseCode();
        reply.cookie = connection.getHeaderField("Set-Cookie");
        InputStream input = reply.status >= 400
            ? connection.getErrorStream() : connection.getInputStream();
        try {
            reply.body = readLimited(input, maximumBytes);
        } finally {
            if (input != null) input.close();
            connection.disconnect();
        }
        return reply;
    }

    private static String replyText(HttpReply reply) {
        return new String(reply.body == null ? new byte[0] : reply.body,
                          StandardCharsets.UTF_8);
    }

    private static String apiError(HttpReply reply) {
        try {
            String message = new JSONObject(replyText(reply)).optString("error", "");
            if (!message.isEmpty()) return message;
        } catch (Exception ignored) {}
        return "RetroMedia returned HTTP " + reply.status;
    }

    private static String sessionCookie(String setCookie) {
        if (setCookie == null) return "";
        for (String part : setCookie.split(";")) {
            String value = part.trim();
            if (value.startsWith("rm_session=")) return value;
        }
        return "";
    }

    private static JSONObject accountFromReply(HttpReply reply) throws Exception {
        return new JSONObject(replyText(reply)).getJSONObject("account");
    }

    private static int accountCredits(JSONObject account) {
        return account.optInt("credits", 0);
    }

    private static int accountFree(JSONObject account) {
        return account.optInt("freeRemainingToday", 0);
    }

    private void publishAccount(String operation, JSONObject account,
                                int matched, int downloaded, String message) {
        retroMediaResult = retroMediaResult(true, operation,
            account.optString("email", ""), accountCredits(account),
            accountFree(account), matched, downloaded,
            account.optBoolean("isAdmin", false), message);
    }

    private JSONObject fetchRetroMediaAccount(String cookie) throws Exception {
        HttpReply me = retroMediaRequest("GET", RETROMEDIA_URL + "/api/me",
                                        null, cookie,
                                        MAX_RETROMEDIA_REPLY_BYTES);
        if (me.status == 401 || me.status == 403) {
            clearRetroMediaSession();
            throw new Exception("RetroMedia session expired - sign in again");
        }
        if (me.status != 200) throw new Exception(apiError(me));
        return accountFromReply(me);
    }

    private void retroMediaStatus() throws Exception {
        String cookie = loadRetroMediaSession();
        if (cookie.isEmpty()) {
            retroMediaResult = retroMediaResult(true, "STATUS", "", 0, 0,
                                                0, 0, false, "Not signed in");
            return;
        }
        JSONObject account = fetchRetroMediaAccount(cookie);
        publishAccount("STATUS", account, 0, 0, "Connected");
    }

    private void retroMediaLogin(String email, String password) throws Exception {
        email = email == null ? "" : email.trim();
        password = password == null ? "" : password;
        if (email.isEmpty() || password.isEmpty()) {
            throw new Exception("Enter the email and password for your registered account");
        }

        HttpReply configReply = retroMediaRequest(
            "GET", RETROMEDIA_URL + "/api/auth/config", null, "",
            MAX_RETROMEDIA_REPLY_BYTES);
        if (configReply.status != 200) throw new Exception(apiError(configReply));
        JSONObject config = new JSONObject(replyText(configReply));
        JSONObject firebase = config.optJSONObject("firebase");

        HttpReply login;
        if (firebase != null && firebase.optBoolean("enabled", false)) {
            String apiKey = firebase.optString("apiKey", "");
            if (apiKey.isEmpty()) throw new Exception("RetroMedia sign-in is not configured");
            JSONObject credentials = new JSONObject();
            credentials.put("email", email);
            credentials.put("password", password);
            credentials.put("returnSecureToken", true);
            String identityUrl =
                "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" +
                URLEncoder.encode(apiKey, "UTF-8");
            HttpReply identity = retroMediaRequest(
                "POST", identityUrl, credentials, "", MAX_RETROMEDIA_REPLY_BYTES);
            if (identity.status != 200) {
                String code = "";
                try {
                    code = new JSONObject(replyText(identity)).getJSONObject("error")
                        .optString("message", "");
                } catch (Exception ignored) {}
                if (code.contains("INVALID_LOGIN_CREDENTIALS") ||
                    code.contains("INVALID_PASSWORD") || code.contains("EMAIL_NOT_FOUND")) {
                    throw new Exception("Wrong RetroMedia email or password");
                }
                throw new Exception(code.isEmpty() ? apiError(identity) : code);
            }
            String idToken = new JSONObject(replyText(identity)).getString("idToken");
            JSONObject exchange = new JSONObject();
            exchange.put("id_token", idToken);
            login = retroMediaRequest("POST",
                RETROMEDIA_URL + "/api/auth/firebase", exchange, "",
                MAX_RETROMEDIA_REPLY_BYTES);
        } else {
            JSONObject credentials = new JSONObject();
            credentials.put("email", email);
            credentials.put("password", password);
            login = retroMediaRequest("POST", RETROMEDIA_URL + "/api/auth/login",
                                      credentials, "",
                                      MAX_RETROMEDIA_REPLY_BYTES);
        }
        if (login.status != 200) throw new Exception(apiError(login));
        String cookie = sessionCookie(login.cookie);
        if (cookie.isEmpty()) throw new Exception("RetroMedia did not return a session");
        JSONObject account = accountFromReply(login);
        saveRetroMediaSession(cookie, account.optString("email", email));
        publishAccount("LOGIN", account, 0, 0, "Signed in");
    }

    private void retroMediaLogout() throws Exception {
        String cookie = loadRetroMediaSession();
        if (!cookie.isEmpty()) {
            retroMediaRequest("POST", RETROMEDIA_URL + "/api/auth/logout",
                              new JSONObject(), cookie,
                              MAX_RETROMEDIA_REPLY_BYTES);
        }
        clearRetroMediaSession();
        retroMediaCatalogueResult = "";
        retroMediaResult = retroMediaResult(true, "LOGOUT", "", 0, 0, 0, 0,
                                            false, "Signed out");
    }

    private static String canonicalGameName(String name) {
        if (name == null) return "";
        String value = name.toLowerCase(Locale.ROOT)
            .replaceAll("\\([^)]*\\)", " ")
            .replaceAll("\\[[^]]*\\]", " ")
            .replace('&', ' ')
            .replaceAll("[^a-z0-9]+", " ")
            .trim().replaceAll("\\s+", " ");
        // The 3DO disc title is commonly dumped as "Road & Track Presents:
        // The Need for Speed", while RetroMedia follows the shorter database
        // title "Need for Speed, The". They are the same release.
        value = value.replaceFirst("^road (?:and )?track presents ", "");
        if (value.startsWith("the ")) value = value.substring(4);
        if (value.endsWith(" the")) value = value.substring(0, value.length() - 4);
        return value;
    }

    private static int editDistance(String left, String right) {
        int[] previous = new int[right.length() + 1];
        int[] current = new int[right.length() + 1];
        for (int j = 0; j <= right.length(); ++j) previous[j] = j;
        for (int i = 1; i <= left.length(); ++i) {
            current[0] = i;
            for (int j = 1; j <= right.length(); ++j) {
                current[j] = Math.min(Math.min(current[j - 1] + 1,
                                               previous[j] + 1),
                                      previous[j - 1] +
                                      (left.charAt(i - 1) == right.charAt(j - 1) ? 0 : 1));
            }
            int[] swap = previous; previous = current; current = swap;
        }
        return previous[right.length()];
    }

    private static JSONObject bestGameMatch(String localName,
                                            ArrayList<JSONObject> catalogue) {
        String wanted = canonicalGameName(localName);
        if (wanted.isEmpty()) return null;
        JSONObject best = null;
        double bestScore = 0.0;
        for (JSONObject game : catalogue) {
            String title = canonicalGameName(game.optString("title", ""));
            String name = canonicalGameName(game.optString("name", ""));
            if (wanted.equals(title) || wanted.equals(name)) return game;
            String candidate = title.isEmpty() ? name : title;
            if (candidate.isEmpty()) continue;
            int longest = Math.max(wanted.length(), candidate.length());
            double score = longest == 0 ? 0.0
                : 1.0 - ((double) editDistance(wanted, candidate) / longest);
            if (score > bestScore) { bestScore = score; best = game; }
        }
        return bestScore >= 0.86 ? best : null;
    }

    private ArrayList<JSONObject> retroMediaCatalogue(String mediaType)
        throws Exception {
        ArrayList<JSONObject> games = new ArrayList<>();
        for (int page = 1; page <= 4; ++page) {
            String address = RETROMEDIA_URL +
                "/api/systems/3do/games?limit=200&page=" + page + "&type=" +
                URLEncoder.encode(mediaType, "UTF-8");
            HttpReply reply = retroMediaRequest("GET", address, null, "",
                                                MAX_RETROMEDIA_REPLY_BYTES);
            if (reply.status != 200) throw new Exception(apiError(reply));
            JSONObject root = new JSONObject(replyText(reply));
            JSONArray pageGames = root.getJSONArray("games");
            for (int i = 0; i < pageGames.length(); ++i) {
                games.add(pageGames.getJSONObject(i));
            }
            if (games.size() >= root.optInt("total", games.size()) ||
                pageGames.length() == 0) break;
        }
        return games;
    }

    private void retroMediaBrowseCatalogue(String search) throws Exception {
        String cookie = loadRetroMediaSession();
        if (cookie.isEmpty()) throw new Exception("Sign in to RetroMedia first");
        JSONObject account = fetchRetroMediaAccount(cookie);
        if (!account.optBoolean("isAdmin", false)) {
            retroMediaCatalogueResult = "";
            throw new Exception("The game catalogue requires an administrator account");
        }

        StringBuilder out = new StringBuilder();
        int found = 0;
        for (int page = 1; page <= 20; ++page) {
            String address = RETROMEDIA_URL +
                "/api/systems/3do/games?limit=200&category=rom&page=" + page;
            if (search != null && !search.trim().isEmpty()) {
                address += "&search=" + URLEncoder.encode(search.trim(), "UTF-8");
            }
            HttpReply reply = retroMediaRequest("GET", address, null, cookie,
                                                MAX_RETROMEDIA_REPLY_BYTES);
            if (reply.status != 200) throw new Exception(apiError(reply));
            JSONObject root = new JSONObject(replyText(reply));
            JSONArray pageGames = root.getJSONArray("games");
            for (int i = 0; i < pageGames.length(); ++i) {
                JSONObject game = pageGames.getJSONObject(i);
                JSONObject availability = game.optJSONObject("availability");
                int romFiles = availability == null ? 0
                    : availability.optInt("romFiles", 0);
                if (romFiles <= 0) continue;
                out.append(safeResultField(game.optString("slug", ""))).append('|')
                   .append(safeResultField(game.optString("title",
                                                          game.optString("name", ""))))
                   .append('|').append(romFiles).append('|')
                   .append(game.optLong("totalBytes", 0)).append('\n');
                ++found;
            }
            if (pageGames.length() == 0 ||
                page * 200 >= root.optInt("total", found)) break;
        }
        retroMediaCatalogueResult = out.toString();
        publishAccount("CATALOGUE", account, found, 0,
                       found + " downloadable game" + (found == 1 ? "" : "s"));
    }

    private static String safeFileName(String name) {
        if (name == null) return "game.bin";
        int slash = Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
        if (slash >= 0) name = name.substring(slash + 1);
        name = name.replaceAll("[\\p{Cntrl}\\/:*?\"<>|]", "_").trim();
        return name.isEmpty() || name.equals(".") || name.equals("..")
            ? "game.bin" : name;
    }

    private static boolean supported3doFile(String name) {
        String lower = name.toLowerCase(Locale.ROOT);
        return lower.endsWith(".chd") || lower.endsWith(".iso") ||
               lower.endsWith(".cue") || lower.endsWith(".bin") ||
               lower.endsWith(".img");
    }

    private void retroMediaDownloadRom(String slug, String gamesFolder)
        throws Exception {
        String cookie = loadRetroMediaSession();
        if (cookie.isEmpty()) throw new Exception("Sign in to RetroMedia first");
        JSONObject account = fetchRetroMediaAccount(cookie);
        if (!account.optBoolean("isAdmin", false)) {
            throw new Exception("ROM download is available to administrators only");
        }
        if (gamesFolder == null || gamesFolder.isEmpty()) {
            throw new Exception("Choose a Games folder in System first");
        }
        if (!documents.canWrite(gamesFolder)) {
            throw new Exception(
                "Games folder is read-only. Re-select it in System to grant write access");
        }

        String encodedSlug = URLEncoder.encode(slug, "UTF-8").replace("+", "%20");
        HttpReply detailReply = retroMediaRequest("GET",
            RETROMEDIA_URL + "/api/systems/3do/games/" + encodedSlug,
            null, cookie, MAX_RETROMEDIA_REPLY_BYTES);
        if (detailReply.status != 200) throw new Exception(apiError(detailReply));
        JSONObject detail = new JSONObject(replyText(detailReply));
        JSONArray roms = detail.optJSONArray("roms");
        if (roms == null || roms.length() == 0) {
            throw new Exception("No downloadable game files are available");
        }

        String title = detail.optString("title", detail.optString("name", slug));
        String destination = documents.ensureDirectory(
            gamesFolder, safeFileName(title));
        HttpURLConnection connection = (HttpURLConnection) new URL(
            RETROMEDIA_URL + "/api/systems/3do/games/" + encodedSlug +
            "/zip?types=rom").openConnection();
        connection.setConnectTimeout(15000);
        connection.setReadTimeout(10 * 60 * 1000);
        connection.setRequestProperty("Cookie", cookie);
        connection.setRequestProperty("User-Agent", "Retro-3DO/2.1 RetroMedia client");
        connection.setRequestProperty("Accept", "application/octet-stream, application/zip");
        int status = connection.getResponseCode();
        if (status != 200) {
            InputStream error = connection.getErrorStream();
            byte[] body = readLimited(error, MAX_RETROMEDIA_REPLY_BYTES);
            if (error != null) error.close();
            HttpReply failed = new HttpReply();
            failed.status = status;
            failed.body = body;
            connection.disconnect();
            throw new Exception(apiError(failed));
        }

        int written = 0;
        try (InputStream raw = connection.getInputStream()) {
            if (roms.length() == 1) {
                String name = safeFileName(roms.getJSONObject(0).optString("file", title));
                if (!supported3doFile(name)) {
                    throw new Exception("RetroMedia returned an unsupported 3DO image: " + name);
                }
                documents.writeDocument(destination, name,
                                        "application/octet-stream", raw);
                written = 1;
            } else {
                try (ZipInputStream zip = new ZipInputStream(raw)) {
                    ZipEntry entry;
                    while ((entry = zip.getNextEntry()) != null) {
                        if (entry.isDirectory()) continue;
                        String name = safeFileName(entry.getName());
                        if (!supported3doFile(name)) continue;
                        documents.writeDocument(destination, name,
                                                "application/octet-stream", zip);
                        ++written;
                        zip.closeEntry();
                    }
                }
            }
        } finally {
            connection.disconnect();
        }
        if (written == 0) throw new Exception("Download contained no supported 3DO images");
        account = fetchRetroMediaAccount(cookie);
        publishAccount("DOWNLOAD", account, 1, written,
                       title + " downloaded (" + written + " file" +
                       (written == 1 ? "" : "s") + ")");
    }

    private static String sha256(String text) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256")
            .digest(text.getBytes(StandardCharsets.UTF_8));
        StringBuilder out = new StringBuilder();
        for (byte value : digest) out.append(String.format(Locale.ROOT, "%02x", value));
        return out.toString();
    }

    private File artworkDirectory(String mediaType) {
        return new File(new File(getFilesDir(), "retromedia-art"), mediaType);
    }

    private File cachedArtworkFile(String mediaType, String localName)
        throws Exception {
        return new File(artworkDirectory(mediaType), sha256(localName) + ".r3a");
    }

    private static Bitmap firstImageInZip(byte[] zipBytes) throws Exception {
        try (ZipInputStream zip = new ZipInputStream(
                 new java.io.ByteArrayInputStream(zipBytes))) {
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (entry.isDirectory()) continue;
                String name = entry.getName().toLowerCase(Locale.ROOT);
                if (!(name.endsWith(".png") || name.endsWith(".jpg") ||
                      name.endsWith(".jpeg") || name.endsWith(".webp") ||
                      name.endsWith(".bmp"))) continue;
                byte[] image = readLimited(zip, MAX_RETROMEDIA_ART_BYTES);
                Bitmap bitmap = BitmapFactory.decodeByteArray(image, 0, image.length);
                if (bitmap != null) return bitmap;
            }
        }
        throw new Exception("Artwork archive contains no supported image");
    }

    private static Bitmap scaledCardBitmap(Bitmap source) {
        final int maxWidth = 360;
        final int maxHeight = 500;
        double scale = Math.min(1.0, Math.min((double) maxWidth / source.getWidth(),
                                             (double) maxHeight / source.getHeight()));
        if (scale >= 1.0) return source;
        Bitmap scaled = Bitmap.createScaledBitmap(source,
            Math.max(1, (int) Math.round(source.getWidth() * scale)),
            Math.max(1, (int) Math.round(source.getHeight() * scale)), true);
        if (scaled != source) source.recycle();
        return scaled;
    }

    private static void writeCardArtwork(Bitmap source, File target)
        throws Exception {
        Bitmap bitmap = scaledCardBitmap(source);
        File temp = new File(target.getParentFile(), target.getName() + ".tmp");
        int width = bitmap.getWidth();
        int height = bitmap.getHeight();
        int[] pixels = new int[width];
        byte[] rgba = new byte[width * 4];
        try (DataOutputStream output = new DataOutputStream(
                 new FileOutputStream(temp))) {
            output.write(new byte[] {'R', '3', 'A', 'R'});
            output.writeInt(width);
            output.writeInt(height);
            for (int y = 0; y < height; ++y) {
                bitmap.getPixels(pixels, 0, width, 0, y, width, 1);
                for (int x = 0; x < width; ++x) {
                    int argb = pixels[x];
                    int at = x * 4;
                    rgba[at] = (byte) ((argb >>> 16) & 0xff);
                    rgba[at + 1] = (byte) ((argb >>> 8) & 0xff);
                    rgba[at + 2] = (byte) (argb & 0xff);
                    rgba[at + 3] = (byte) ((argb >>> 24) & 0xff);
                }
                output.write(rgba);
            }
        } finally {
            bitmap.recycle();
        }
        if (target.exists() && !target.delete()) {
            throw new Exception("Cannot replace cached artwork");
        }
        if (!temp.renameTo(target)) throw new Exception("Cannot finish cached artwork");
    }

    private String artworkPreferenceKey(String mediaType, String localName)
        throws Exception {
        return "art." + mediaType + "." + sha256(localName);
    }

    private void rememberArtwork(String mediaType, String localName, File file,
                                 int width, int height, String slug) throws Exception {
        retroMediaPreferences().edit()
            .putString(artworkPreferenceKey(mediaType, localName),
                       localName + "|" + file.getAbsolutePath() + "|" +
                       width + "|" + height + "|" + safeResultField(slug))
            .apply();
    }

    private void retroMediaSync(String gameNames, String mediaType) throws Exception {
        if (!RETROMEDIA_CARD_TYPES.contains(mediaType)) {
            throw new Exception("Unsupported RetroMedia card artwork type");
        }
        String cookie = loadRetroMediaSession();
        if (cookie.isEmpty()) throw new Exception("Sign in to RetroMedia first");
        ArrayList<JSONObject> catalogue = retroMediaCatalogue(mediaType);
        File folder = artworkDirectory(mediaType);
        if (!folder.exists() && !folder.mkdirs()) {
            throw new Exception("Cannot create the artwork cache");
        }

        int matched = 0;
        int downloaded = 0;
        int cached = 0;
        int missing = 0;
        String[] names = gameNames == null || gameNames.isEmpty()
            ? new String[0] : gameNames.split("\\n");
        for (String localName : names) {
            if (localName.trim().isEmpty()) continue;
            File target = cachedArtworkFile(mediaType, localName);
            String preference = retroMediaPreferences().getString(
                artworkPreferenceKey(mediaType, localName), "");
            String[] cachedFields = preference.split("\\|", 5);
            boolean legacyRoadTrack = canonicalGameName(localName)
                .equals("need for speed") && cachedFields.length < 5;
            if (target.isFile() && !preference.isEmpty() && !legacyRoadTrack) {
                ++matched;
                ++cached;
                continue;
            }

            JSONObject match = bestGameMatch(localName, catalogue);
            if (match == null) { ++missing; continue; }
            String preview = match.optString("preview", "");
            if (preview.isEmpty()) { ++missing; continue; }
            ++matched;
            String slug = match.getString("slug");
            String address = RETROMEDIA_URL + "/api/systems/3do/games/" +
                URLEncoder.encode(slug, "UTF-8").replace("+", "%20") +
                // Select by media type. This is stable across RetroMedia packs
                // and avoids filenames containing commas being split by the
                // API's multi-file query syntax (Need for Speed, The).
                "/zip?types=" + URLEncoder.encode(mediaType, "UTF-8");
            HttpReply zip = retroMediaRequest("GET", address, null, cookie,
                                              MAX_RETROMEDIA_ART_BYTES);
            if (zip.status == 402) {
                throw new Exception(apiError(zip) + " (" + downloaded +
                                    " artwork download(s) completed)");
            }
            if (zip.status != 200) {
                Log.w(TAG, "RetroMedia artwork failed for " + localName +
                           ": " + apiError(zip));
                ++missing;
                continue;
            }
            Bitmap bitmap = firstImageInZip(zip.body);
            writeCardArtwork(bitmap, target);
            // Read the just-written dimensions from the decoded target record.
            try (java.io.DataInputStream input = new java.io.DataInputStream(
                     new FileInputStream(target))) {
                input.skipBytes(4);
                int width = input.readInt();
                int height = input.readInt();
                rememberArtwork(mediaType, localName, target, width, height, slug);
            }
            ++downloaded;
        }

        JSONObject account = fetchRetroMediaAccount(cookie);
        String message = downloaded + " downloaded";
        if (cached > 0) message += ", " + cached + " already cached";
        if (missing > 0) message += ", " + missing + " not found";
        publishAccount("SYNC", account, matched, downloaded, message);
    }

    private String retroMediaArtwork(String mediaType) {
        if (!RETROMEDIA_CARD_TYPES.contains(mediaType)) return "";
        String prefix = "art." + mediaType + ".";
        Map<String, ?> entries = retroMediaPreferences().getAll();
        StringBuilder out = new StringBuilder();
        for (Map.Entry<String, ?> entry : entries.entrySet()) {
            if (!entry.getKey().startsWith(prefix) ||
                !(entry.getValue() instanceof String)) continue;
            String value = (String) entry.getValue();
            String[] fields = value.split("\\|", 5);
            if (fields.length < 4 || !new File(fields[1]).isFile()) continue;
            out.append(safeResultField(fields[0])).append('|')
               .append(fields[1]).append('|').append(fields[2]).append('|')
               .append(fields[3]).append('\n');
        }
        return out.toString();
    }
}
