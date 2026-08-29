package com.fourdo.android;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import androidx.documentfile.provider.DocumentFile;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import androidx.appcompat.widget.SwitchCompat;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.function.Consumer;
import java.util.function.IntConsumer;

/**
 * 3DO Opera — Ymir-style single-Activity host.
 *
 * Owns: the native surface, the play bar, the settings panel, the game library
 * screen, the in-game virtual pad, the controller-mapping overlay, the loading
 * overlay, the FPS counter, and the auto-hide control logic. Replaces the
 * previous four-Activity split ({@code HomeActivity}, {@code EmulatorActivity},
 * {@code SettingsActivity}, {@code NewControllerMapperActivity}).
 *
 * {@code SetupWizardActivity} remains a separate Activity for the one-shot
 * first-run storage/BIOS flow.
 */
public final class MainActivity extends AppCompatActivity {

    // ------------------------------------------------------------------ constants

    public static final String PREFS_NAME = "fourdo_prefs";

    public static final String KEY_BIOS_PATH = "bios_path";
    public static final String KEY_LAST_GAME_PATH = "last_game_path";
    public static final String KEY_APP_STORAGE_ROOT = "app_storage_root";
    public static final String KEY_LIBRARY_FOLDER = "library_folder";
    /**
     * A SAF tree holding the game library, read in place.
     *
     * The library used to be adopted by COPYING every game into app storage. On
     * a real library that means duplicating gigabytes onto internal storage -
     * slow, wasteful, and simply impossible on a device without the headroom -
     * to gain nothing, because SAF can read the originals where they are.
     */
    public static final String KEY_LIBRARY_TREE_URI = "library_tree_uri";
    public static final String KEY_LIBRARY_REFRESH_REQUIRED = "library_refresh_required";
    public static final String KEY_DEBUG_OVERLAY_ENABLED = "debug_overlay_enabled";
    public static final String KEY_VIEW_STYLE = "view_style";
    public static final String KEY_RENDERER_TYPE = "renderer_type";
    public static final String KEY_TEXTURE_FILTERING = "texture_filtering";
    public static final String KEY_REGION = "region";
    public static final String KEY_BEZEL_ENABLED = "bezel_enabled";
    public static final String KEY_BEZEL_GITHUB_URL = "bezel_github_url";
    public static final String KEY_DISPLAY_ROTATION = "display_rotation";
    public static final String KEY_VULKAN_DRIVER_PATH = "vulkan_driver_path";
    public static final String KEY_CRT_ENABLED = "crt_shader_enabled";
    public static final String KEY_VIRTUAL_PAD_VISIBLE = "virtual_pad_visible";
    public static final String KEY_ASPECT_RATIO_16BY9 = "aspect_ratio_16by9";
    public static final String KEY_NEAREST_FILTERING = "nearest_filtering";
    public static final String KEY_ANTI_ALIASING_MODE = "anti_aliasing_mode";
    public static final String KEY_RESOLUTION_SCALE = "resolution_scale";
    public static final String KEY_OUTPUT_RESOLUTION_PRESET = "output_resolution_preset";
    public static final String PREF_SETUP_COMPLETED = "setup_completed";

    // Renderer enum (must match native values).
    public static final int RENDERER_AUTO = 0;
    public static final int RENDERER_OPENGL_ES = 1;
    public static final int RENDERER_VULKAN = 2;
    public static final int RENDERER_SOFTWARE = 3;

    // Texture filtering enum.
    public static final int FILTERING_LINEAR = 0;
    public static final int FILTERING_NEAREST = 1;

    // Region enum (must match native values).
    public static final int REGION_NTSC = 0;
    public static final int REGION_PAL1 = 1;
    public static final int REGION_PAL2 = 2;

    // Display rotation (1..4 = fixed; 0 = follow device).
    public static final int ROTATION_AUTO = 0;
    public static final int ROTATION_FIXED_0 = 1;
    public static final int ROTATION_FIXED_90 = 2;
    public static final int ROTATION_FIXED_180 = 3;
    public static final int ROTATION_FIXED_270 = 4;

    // Library view style.
    public static final int VIEW_STYLE_GRID_SMALL = 0;
    public static final int VIEW_STYLE_GRID_MEDIUM = 1;
    public static final int VIEW_STYLE_GRID_LARGE = 2;
    public static final int VIEW_STYLE_CAROUSEL = 3;

    // Tunables.
    private static final int MAX_RESOLUTION_SCALE = 9;
    private static final int MAX_CONCURRENT_IGDB_REQUESTS = 3;
    private static final int MAX_IGDB_LOOKUPS_PER_SCAN = 120;
    private static final long TOP_CONTROLS_AUTO_HIDE_MS = 3000L;

    // SAF request codes (consolidated from HomeActivity + EmulatorActivity + SettingsActivity).
    private static final int REQ_IMPORT_BIOS = 1;
    private static final int REQ_IMPORT_LIBRARY = 2;
    private static final int REQ_IMPORT_DRIVER = 3;
    private static final int REQ_IMPORT_GAME = 4;

    // Buttons covered by the in-game virtual pad.
    private static final int[] VIRTUAL_PAD_BUTTONS = {
            ControllerMappingManager.BUTTON_A,
            ControllerMappingManager.BUTTON_B,
            ControllerMappingManager.BUTTON_C,
            ControllerMappingManager.BUTTON_PLAY_PAUSE,
            ControllerMappingManager.BUTTON_STOP,
            ControllerMappingManager.BUTTON_DPAD_UP,
            ControllerMappingManager.BUTTON_DPAD_DOWN,
            ControllerMappingManager.BUTTON_DPAD_LEFT,
            ControllerMappingManager.BUTTON_DPAD_RIGHT,
            ControllerMappingManager.BUTTON_L1,
            ControllerMappingManager.BUTTON_R1
    };

    // Load the JNI library once per process.
    static {
        System.loadLibrary("native-lib");
    }

    // ----------------------------------------------------------- UI field storage

    private FrameLayout rootView;
    private FrameLayout screenContainer;
    private SurfaceView emulatorSurfaceView;
    private ImageView emulatorBezel;

    // Play bar (top-right, auto-hide).
    private View topControlsBar;
    private TextView fpsCounter;
    private Button topRendererButton;
    private Button topAspectButton;
    private Button topCrtButton;
    private Button topBezelButton;
    private Button topPadButton;
    private Button topSettingsButton;

    // In-game translucent virtual pad overlay.
    private View virtualPadOverlay;

    // Right-anchored settings panel.
    private View settingsPanel;
    private TextView sideStatusText;
    private SwitchCompat sideDebugSwitch;
    private SwitchCompat sideBezelSwitch;
    private SwitchCompat sideCrtSwitch;
    private TextView sideBiosPathText;
    private TextView sideLibraryPathText;
    private View bezelDownloadCard;
    private TextView sideVulkanDriverPathText;

    // Game library full-screen overlay.
    private View gameLibraryScreen;
    private EditText gameLibrarySearch;
    private GridLayout gameLibraryGrid;
    private HorizontalScrollView gameLibraryCarouselScroll;
    private LinearLayout gameLibraryCarousel;
    private TextView gameLibraryStatus;
    private Button gameLibraryViewButton;
    private final List<GameLibraryEntry> currentGameLibrary = new ArrayList<>();
    private boolean gameLibraryCarouselMode;

    // Unified Controller screen: external-gamepad mapping (3DO pad diagram) +
    // on-screen touch-pad layout controls.
    private View mapperOverlay;
    private final java.util.LinkedHashMap<Integer, Button> mapperHotspots = new java.util.LinkedHashMap<>();
    private DpadView editorDpad;
    private Button mapperBackButton;
    private TextView mapperStatusText;
    private Button padShowHideButton;
    private Button padEditButton;
    private int mapperWaitingForButton = -1;

    // In-game touch pad: live controls + drag-to-reposition edit mode.
    private final java.util.List<View> padButtons = new java.util.ArrayList<>();
    private final java.util.List<PadSpec> padSpecs = new java.util.ArrayList<>();
    private FrameLayout virtualPadContainer;
    private DpadView dpadView;
    private TextView padEditBanner;
    private boolean padEditMode = false;

    // Loading + status overlays.
    private View loadingOverlay;
    private TextView loadingText;
    private TextView statusIndicator;
    private View displaySwitchOverlay;

    // IGDB service (shared between game scan and library scan).
    private IgdbService igdbService;
    private int igdbNextIndex;
    private int igdbInFlight;

    // FPS / timing.
    private int frameCount = 0;
    private long lastFpsTime = 0;
    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final Handler fpsHandler = new Handler(Looper.getMainLooper());
    private Runnable fpsRunnable;
    private final Runnable hideTopControlsRunnable = () -> {
        if (topControlsBar == null) {
            return;
        }
        topControlsBar.setVisibility(View.GONE);
    };

    // State machine.
    private boolean debugModeEnabled = false;
    private boolean manuallyPaused = false;
    private boolean overlayPaused = false;
    private boolean emulatorInitializing = false;
    private boolean emulatorStarted = false;

    // Emulator settings.
    private boolean aspectRatio16by9 = false;
    private int currentRenderer = RENDERER_VULKAN;
    private boolean nearestFiltering = false;
    private int antiAliasingMode = 0;
    private boolean crtShaderEnabled = false;
    private boolean bezelEnabled = true;
    private boolean virtualPadVisible = false;
    private String currentBezelPath = "";
    private int resolutionScale = 0;
    private int outputResolutionPreset = 0;
    private boolean flipVertical = false;
    private ParcelFileDescriptor mGameDescriptor;
    private String mGamePath = null;
    private final EmulatorAudioEngine audioEngine = new EmulatorAudioEngine();
    private String appVersion = "";

    // Back navigation.
    private OnBackInvokedCallback backInvokedCallback;

    // ------------------------------------------------------------- lifecycle

    public static void start(Context context) {
        start(context, null);
    }

    public static void start(Context context, String gamePath) {
        Intent intent = new Intent(context, MainActivity.class);
        if (gamePath != null) {
            intent.putExtra("game_path", gamePath);
        }
        context.startActivity(intent);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);

        // First-run routing: hand off to SetupWizardActivity if paths are missing.
        if (needsSetup()) {
            SetupWizardActivity.start(this);
            finish();
            return;
        }

        configureWindowAndLayout();
        initializeUiState();
        setupActivityButtons();
        setupEmulatorSurface();
        handleStartupIntent(getIntent());
        registerBackHandler();
    }

    /**
     * Targeting API 36 forces edge-to-edge: windowFullscreen, statusBarColor and
     * navigationBarColor in the theme are all ignored, so the bars come back and sit
     * over the video unless they are hidden through the inset controller instead.
     * shortEdges keeps the picture full-bleed on notched devices in landscape.
     */
    private void applyFullscreenWindowMode() {
        Window window = getWindow();
        WindowCompat.setDecorFitsSystemWindows(window, false);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        WindowInsetsControllerCompat controller =
                WindowCompat.getInsetsController(window, window.getDecorView());
        controller.setSystemBarsBehavior(
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        controller.hide(WindowInsetsCompat.Type.systemBars());
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            // A transient bar swipe, or returning from the picker, leaves the bars
            // showing; re-hide them the way the sticky-immersive flags used to.
            applyFullscreenWindowMode();
            resizeScreenSurface();
        }
    }

    @Override
    protected void onDestroy() {
        unregisterBackHandler();
        if (emulatorStarted || emulatorInitializing) {
            autoSaveNVRAM();
            releaseVirtualPadButtons();
            stopFpsCounter();
            stopAudioPlayback();
            emulatorStarted = false;
            shutdownEmulator();
            cleanupRenderer();
        }
        super.onDestroy();
    }

    @SuppressLint("GestureBackNavigation")
    @Override
    public void onBackPressed() {
        if (handleBackNavigation()) {
            return;
        }
        super.onBackPressed();
    }

    private void registerBackHandler() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
            return;
        }
        backInvokedCallback = () -> {
            if (!handleBackNavigation()) {
                finish();
            }
        };
        getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT, backInvokedCallback);
    }

    private void unregisterBackHandler() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU || backInvokedCallback == null) {
            return;
        }
        getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backInvokedCallback);
        backInvokedCallback = null;
    }

    /** Ymir-style: topmost overlay first, then the panel, then finish. */
    private boolean handleBackNavigation() {
        if (mapperOverlay != null && mapperOverlay.getVisibility() == View.VISIBLE) {
            closeMapperOverlay();
            return true;
        }
        if (gameLibraryScreen != null && gameLibraryScreen.getVisibility() == View.VISIBLE) {
            closeGameLibraryScreen();
            return true;
        }
        if (settingsPanel != null && settingsPanel.getVisibility() == View.VISIBLE) {
            settingsPanel.setVisibility(View.GONE);
            resumeAfterOverlayIfIdle();
            return true;
        }
        return false;
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
            showTopControlsTemporarily();
        }
        return super.dispatchTouchEvent(event);
    }

    @Override
    protected void onPause() {
        super.onPause();
        releaseVirtualPadButtons();
        autoSaveNVRAM();
        if (emulatorStarted && !emulatorInitializing && !manuallyPaused) {
            pauseEmulator();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        debugModeEnabled = prefs.getBoolean(KEY_DEBUG_OVERLAY_ENABLED, false);
        bezelEnabled = prefs.getBoolean(KEY_BEZEL_ENABLED, true);
        virtualPadVisible = prefs.getBoolean(KEY_VIRTUAL_PAD_VISIBLE, virtualPadVisible);
        crtShaderEnabled = prefs.getBoolean(KEY_CRT_ENABLED, false);
        nearestFiltering = prefs.getBoolean(KEY_NEAREST_FILTERING, false);

        applyVirtualPadVisibility();
        applyDebugModeUi();
        applyBezelVisibility();
        refreshSidePathTexts();

        // Re-push filter/CRT prefs to the renderer in case they were changed
        // while paused (e.g. by an in-panel Switch).
        setFiltering(nearestFiltering);
        setCrtShaderEnabled(crtShaderEnabled);
        updateButtonLabels();
        updateStatusIndicator();

        if (emulatorStarted && !emulatorInitializing && !manuallyPaused && !hasVisibleRuntimeOverlay()) {
            resumeEmulator();
        } else if (hasVisibleRuntimeOverlay()) {
            pauseForOverlay();
        }
    }

    @Override
    public void onConfigurationChanged(@NonNull Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        DeviceOrientationManager.handleEmulatorOrientationChange(this);
        setRotation(getEffectiveRotation());
        if (DeviceOrientationManager.isLandscape(this) && emulatorSurfaceView != null) {
            SurfaceHolder holder = emulatorSurfaceView.getHolder();
            if (holder.getSurface() != null && !holder.getSurface().isValid()) {
                initRenderer(emulatorSurfaceView.getWidth(), emulatorSurfaceView.getHeight());
            }
        }
    }

    // -------------------------------------------------------- prefs + setup

    private SharedPreferences prefs;

    /** True if first-run wizard hasn't completed or any path is missing. */
    private boolean needsSetup() {
        // App storage is always app-specific now (no permission, no user choice),
        // so KEY_APP_STORAGE_ROOT is kept in sync automatically rather than gated.
        prefs.edit().putString(KEY_APP_STORAGE_ROOT,
                SafFileImporter.getManagedAppRootPath(this)).apply();
        boolean completed = prefs.getBoolean(PREF_SETUP_COMPLETED, false);
        String biosPath = prefs.getString(KEY_BIOS_PATH, "");
        String libraryPath = prefs.getString(KEY_LIBRARY_FOLDER, "");
        return !completed
                || !EmulatorPathStore.isValidFilePath(biosPath)
                || !EmulatorPathStore.isValidDirectoryPath(libraryPath);
    }

    // ------------------------------------------------------------ configure

    private void configureWindowAndLayout() {
        DeviceOrientationManager.setOptimalEmulatorOrientation(this);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        applyFullscreenWindowMode();
        try {
            appVersion = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            appVersion = "?";
        }
        setContentView(createContentView());
    }

    private void initializeUiState() {
        loadSettings();
        applyVirtualPadVisibility();
        applyBezelVisibility();
        applyDebugModeUi();
        refreshSidePathTexts();
        updateButtonLabels();
        startFpsCounter();
    }

    private void setupActivityButtons() {
        if (topRendererButton != null) topRendererButton.setOnClickListener(v -> {
            cycleRenderer();
            scheduleTopControlsAutoHide();
        });
        if (topAspectButton != null) topAspectButton.setOnClickListener(v -> {
            toggleAspectMode();
            scheduleTopControlsAutoHide();
        });
        if (topCrtButton != null) topCrtButton.setOnClickListener(v -> {
            toggleCrtShader();
            scheduleTopControlsAutoHide();
        });
        if (topBezelButton != null) topBezelButton.setOnClickListener(v -> {
            toggleBezelOverlay();
            scheduleTopControlsAutoHide();
        });
        if (topSettingsButton != null) topSettingsButton.setOnClickListener(v -> {
            toggleSettingsPanel();
            scheduleTopControlsAutoHide();
        });
    }

    private void setupEmulatorSurface() {
        emulatorSurfaceView = new SurfaceView(this);
        emulatorSurfaceView.setClickable(true);
        emulatorSurfaceView.setOnClickListener(v -> showTopControlsTemporarily());
        screenContainer.addView(emulatorSurfaceView, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        emulatorSurfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(@NonNull SurfaceHolder holder) {
                // Wait for surfaceChanged with a real size.
            }

            @Override
            public void surfaceChanged(@NonNull SurfaceHolder holder, int format, int width, int height) {
                if (width > 0 && height > 0) {
                    applyRendererDefaults(width, height, holder.getSurface());
                }
            }

            @Override
            public void surfaceDestroyed(@NonNull SurfaceHolder holder) {
                setSurface(null);
            }
        });
    }

    // ----------------------------------------------------------- view tree

    private View createContentView() {
        FrameLayout root = new FrameLayout(this);
        rootView = root;
        root.setBackgroundColor(0xFF050607);

        // 1. Surface container (centered, fills root).
        screenContainer = new FrameLayout(this);
        screenContainer.setBackgroundColor(0xFF000000);
        root.addView(screenContainer, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
                Gravity.CENTER));

        // 2. Bezel overlay (fills root, below play bar).
        emulatorBezel = new ImageView(this);
        emulatorBezel.setScaleType(ImageView.ScaleType.FIT_XY);
        emulatorBezel.setAdjustViewBounds(false);
        emulatorBezel.setClickable(false);
        emulatorBezel.setFocusable(false);
        emulatorBezel.setImageResource(R.drawable.bezel_fz10);
        emulatorBezel.setVisibility(bezelEnabled ? View.VISIBLE : View.GONE);
        root.addView(emulatorBezel, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // 3. In-game translucent virtual pad overlay.
        virtualPadOverlay = createVirtualPadOverlay();
        virtualPadOverlay.setVisibility(virtualPadVisible ? View.VISIBLE : View.GONE);
        root.addView(virtualPadOverlay, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // 4. Top-right play bar (auto-hide).
        topControlsBar = createPlayBar();
        FrameLayout.LayoutParams playBarParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.RIGHT);
        playBarParams.setMargins(0, dp(8), dp(8), 0);
        root.addView(topControlsBar, playBarParams);
        scheduleTopControlsAutoHide();

        // 5. Right-anchored settings panel (360dp wide, hidden by default).
        settingsPanel = createSettingsPanel();
        settingsPanel.setVisibility(View.GONE);
        FrameLayout.LayoutParams settingsParams = new FrameLayout.LayoutParams(
                dp(360),
                FrameLayout.LayoutParams.MATCH_PARENT,
                Gravity.RIGHT);
        root.addView(settingsPanel, settingsParams);

        // 6. Controller-mapping overlay (hidden by default; layered above settings).
        mapperOverlay = createMapperOverlay();
        mapperOverlay.setVisibility(View.GONE);
        root.addView(mapperOverlay, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // 7. Game library screen (hidden by default; layered above mapper).
        gameLibraryScreen = createGameLibraryScreen();
        gameLibraryScreen.setVisibility(View.GONE);
        root.addView(gameLibraryScreen, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // 8. Display-switch transient overlay (hidden by default).
        displaySwitchOverlay = createDisplaySwitchOverlay();
        displaySwitchOverlay.setVisibility(View.GONE);
        root.addView(displaySwitchOverlay, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // 9. Loading overlay (hidden by default).
        loadingOverlay = createLoadingOverlay();
        loadingOverlay.setVisibility(View.GONE);
        root.addView(loadingOverlay, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // 10. Status indicator (bottom-right, hidden by default).
        statusIndicator = new TextView(this);
        statusIndicator.setTextColor(0xFFFFFFFF);
        statusIndicator.setTextSize(12.0f);
        statusIndicator.setBackgroundColor(0xAA181B1F);
        statusIndicator.setPadding(dp(8), dp(6), dp(8), dp(6));
        statusIndicator.setVisibility(View.GONE);
        FrameLayout.LayoutParams statusParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM | Gravity.RIGHT);
        statusParams.setMargins(0, 0, dp(8), dp(8));
        root.addView(statusIndicator, statusParams);

        return root;
    }

    private View createPlayBar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setPadding(dp(4), dp(4), dp(4), dp(4));
        bar.setBackgroundColor(0xAA181B1F);

        fpsCounter = new TextView(this);
        fpsCounter.setTextColor(0xFFE8EAED);
        fpsCounter.setTextSize(12.0f);
        fpsCounter.setPadding(dp(8), 0, dp(8), 0);
        fpsCounter.setText("FPS 0");
        bar.addView(fpsCounter);

        topRendererButton = makeCompactButton(getRendererShortName(currentRenderer), v -> cycleRenderer());
        bar.addView(topRendererButton);
        topAspectButton = makeCompactButton(aspectRatio16by9 ? "16:9" : "4:3", v -> toggleAspectMode());
        bar.addView(topAspectButton);
        topCrtButton = makeCompactButton(crtShaderEnabled ? "CRT" : "No CRT", v -> toggleCrtShader());
        bar.addView(topCrtButton);
        topBezelButton = makeCompactButton(bezelEnabled ? "Bezel" : "No Bezel", v -> toggleBezelOverlay());
        bar.addView(topBezelButton);
        topPadButton = makeCompactButton(virtualPadVisible ? "Pad" : "No Pad", v -> toggleVirtualPad());
        bar.addView(topPadButton);
        topSettingsButton = makeCompactButton("⚙", v -> toggleSettingsPanel());
        bar.addView(topSettingsButton);

        return bar;
    }

    private View createSettingsPanel() {
        ScrollView settingsScroll = new ScrollView(this);
        settingsScroll.setFillViewport(false);
        settingsScroll.setBackgroundColor(0xEE181B1F);

        LinearLayout controls = new LinearLayout(this);
        controls.setOrientation(LinearLayout.VERTICAL);
        controls.setPadding(dp(14), dp(12), dp(14), dp(14));
        settingsScroll.addView(controls);

        // Header.
        LinearLayout settingsHeader = new LinearLayout(this);
        settingsHeader.setGravity(Gravity.CENTER_VERTICAL);
        settingsHeader.setOrientation(LinearLayout.HORIZONTAL);

        TextView title = new TextView(this);
        title.setText("3DO Opera Settings");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(16.0f);
        settingsHeader.addView(title, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        settingsHeader.addView(makeCompactButton("X", v -> {
            settingsPanel.setVisibility(View.GONE);
            resumeAfterOverlayIfIdle();
        }));
        controls.addView(settingsHeader);

        // Action cards (Ymir pattern).
        controls.addView(makeSettingsActionCard("▦", "Game Library",
                "Browse cards, bezels, and IGDB", v -> openGameLibrary()));
        bezelDownloadCard = makeSettingsActionCard("⇩", "Download Bezels",
                "Pull 3DO bezel pack from GitHub", v -> showDownloadBezelsDialog());
        controls.addView(bezelDownloadCard);
        controls.addView(makeSettingsActionCard("🎮", "Controller",
                "Map a gamepad, or set up the touch controls", v -> openControllerScreen()));
        controls.addView(makeSettingsActionCard("↻", "Reset / Boot BIOS",
                "Restart to BIOS without a disc", v -> bootBios()));
        controls.addView(makeSettingsActionCard("ⓘ", "Debug Info",
                "Show renderer and core status", v -> toggleDebugInfo()));
        controls.addView(makeSettingsActionCard("🖼", "Render Options",
                "Renderer, AA, CRT, resolution", v -> showRenderOptionsDialog()));
        controls.addView(makeSettingsActionCard("📜", "View Logs",
                "Tail logcat", v -> showLogsDialog()));

        // Storage path rows.
        TextViewHolder biosHolder = new TextViewHolder();
        controls.addView(makePathRow("BIOS path:", R.string.bios_not_set, biosHolder));
        sideBiosPathText = biosHolder.view;
        TextViewHolder libraryHolder = new TextViewHolder();
        controls.addView(makePathRow("Game library:", R.string.library_not_set, libraryHolder));
        sideLibraryPathText = libraryHolder.view;
        TextViewHolder driverHolder = new TextViewHolder();
        controls.addView(makePathRow("Vulkan driver:", R.string.vulkan_driver_system, driverHolder));
        sideVulkanDriverPathText = driverHolder.view;

        // Path-edit buttons.
        LinearLayout importRow = new LinearLayout(this);
        importRow.setOrientation(LinearLayout.HORIZONTAL);
        importRow.setGravity(Gravity.CENTER_VERTICAL);
        importRow.addView(makeCompactButton("Import BIOS", v -> openBiosBrowser()));
        LinearLayout.LayoutParams importPad = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        importPad.setMargins(0, dp(8), dp(6), 0);
        importRow.getChildAt(0).setLayoutParams(importPad);
        importRow.addView(makeCompactButton("Import Library", v -> openLibraryPicker()));
        LinearLayout.LayoutParams importPad2 = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        importPad2.setMargins(dp(6), dp(8), 0, 0);
        importRow.getChildAt(1).setLayoutParams(importPad2);
        controls.addView(importRow);
        LinearLayout importRow2 = new LinearLayout(this);
        importRow2.setOrientation(LinearLayout.HORIZONTAL);
        importRow2.setGravity(Gravity.CENTER_VERTICAL);
        importRow2.addView(makeCompactButton("Import Vulkan Driver", v -> openDriverImport()));
        LinearLayout.LayoutParams importPad3 = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        importPad3.setMargins(0, dp(8), dp(6), 0);
        importRow2.getChildAt(0).setLayoutParams(importPad3);
        importRow2.addView(makeCompactButton("Reset Driver", v -> {
            String existingPath = prefs.getString(KEY_VULKAN_DRIVER_PATH, "");
            if (existingPath != null && !existingPath.isEmpty()) {
                File existing = new File(existingPath);
                if (existing.exists()) {
                    //noinspection ResultOfMethodCallIgnored
                    existing.delete();
                }
            }
            prefs.edit().remove(KEY_VULKAN_DRIVER_PATH).apply();
            refreshSidePathTexts();
            toast("Vulkan driver reset to system default");
        }));
        LinearLayout.LayoutParams importPad4 = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        importPad4.setMargins(dp(6), dp(8), 0, 0);
        importRow2.getChildAt(1).setLayoutParams(importPad4);
        controls.addView(importRow2);

        // Render options (one row per setting).
        controls.addView(makeSpinnerRow("Renderer",
                new String[]{"Vulkan", "OpenGL ES", "Software"},
                currentRenderer == RENDERER_VULKAN ? 0
                        : currentRenderer == RENDERER_OPENGL_ES ? 1 : 2,
                position -> {
                    int selectedRenderer = RENDERER_VULKAN;
                    if (position == 1) selectedRenderer = RENDERER_OPENGL_ES;
                    else if (position == 2) selectedRenderer = RENDERER_SOFTWARE;
                    if (currentRenderer == selectedRenderer) return;
                    currentRenderer = selectedRenderer;
                    if (!isHardwareRendererSelected()) {
                        resolutionScale = 1;
                        outputResolutionPreset = 0;
                        setResolutionScale(resolutionScale);
                        setOutputResolutionPreset(0);
                    }
                    setRendererType(currentRenderer);
                    rebindRendererSurface();
                    saveSettings();
                    updateButtonLabels();
                    updateStatusIndicator();
                }));

        controls.addView(makeSpinnerRow("Aspect",
                new String[]{"4:3", "16:9"}, aspectRatio16by9 ? 1 : 0,
                position -> {
                    boolean newWide = (position == 1);
                    if (aspectRatio16by9 == newWide) return;
                    aspectRatio16by9 = newWide;
                    setAspectRatio(aspectRatio16by9);
                    saveSettings();
                    updateButtonLabels();
                    updateStatusIndicator();
                }));

        controls.addView(makeSpinnerRow("Filter",
                new String[]{"Linear", "Nearest"}, nearestFiltering ? 1 : 0,
                position -> {
                    boolean newNearest = (position == 1);
                    if (nearestFiltering == newNearest) return;
                    nearestFiltering = newNearest;
                    setFiltering(nearestFiltering);
                    saveSettings();
                    updateButtonLabels();
                    updateStatusIndicator();
                }));

        controls.addView(makeSpinnerRow("Anti-aliasing",
                new String[]{"Off", "Low (FXAA)", "High (FXAA+)"}, antiAliasingMode,
                position -> {
                    if (antiAliasingMode == position) return;
                    antiAliasingMode = position;
                    setAntiAliasingMode(antiAliasingMode);
                    saveSettings();
                    updateButtonLabels();
                    updateStatusIndicator();
                }));

        controls.addView(makeSpinnerRow("Resolution",
                getResolutionScaleOptions(), getResolutionScaleIndex(resolutionScale),
                position -> {
                    int newScale = getResolutionScaleByIndex(position);
                    if (resolutionScale == newScale) return;
                    if (!isHardwareRendererSelected()) {
                        showStatusToast("Upscaling is only available on hardware renderers");
                        return;
                    }
                    resolutionScale = newScale;
                    outputResolutionPreset = 0;
                    setResolutionScale(resolutionScale);
                    setOutputResolutionPreset(0);
                    rebindRendererSurface();
                    saveSettings();
                    updateButtonLabels();
                    updateStatusIndicator();
                }));

        controls.addView(makeSpinnerRow("Region",
                new String[]{"NTSC", "PAL 1", "PAL 2"},
                prefs.getInt(KEY_REGION, REGION_NTSC),
                position -> prefs.edit().putInt(KEY_REGION, position).apply()));

        controls.addView(makeSpinnerRow("Library view",
                new String[]{"Grid (Small)", "Grid (Medium)", "Grid (Large)", "Carousel"},
                prefs.getInt(KEY_VIEW_STYLE, VIEW_STYLE_GRID_MEDIUM),
                position -> prefs.edit().putInt(KEY_VIEW_STYLE, position).apply()));

        // Switch rows.
        sideDebugSwitch = makeSwitchRow("Debug overlay",
                prefs.getBoolean(KEY_DEBUG_OVERLAY_ENABLED, false),
                isChecked -> prefs.edit().putBoolean(KEY_DEBUG_OVERLAY_ENABLED, isChecked).apply());
        controls.addView(sideDebugSwitch);
        sideBezelSwitch = makeSwitchRow("Bezel overlay",
                prefs.getBoolean(KEY_BEZEL_ENABLED, true),
                isChecked -> prefs.edit().putBoolean(KEY_BEZEL_ENABLED, isChecked).apply());
        controls.addView(sideBezelSwitch);
        sideCrtSwitch = makeSwitchRow("CRT scanlines",
                prefs.getBoolean(KEY_CRT_ENABLED, false),
                isChecked -> prefs.edit().putBoolean(KEY_CRT_ENABLED, isChecked).apply());
        controls.addView(sideCrtSwitch);

        sideStatusText = new TextView(this);
        sideStatusText.setTextColor(0xFFE8EAED);
        sideStatusText.setTextSize(12.0f);
        sideStatusText.setPadding(0, dp(10), 0, 0);
        controls.addView(sideStatusText, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        return settingsScroll;
    }

    /** Stores the `selected` view in the supplied 1-element holder. */
    private interface ViewHolder { void set(View v); }

    /** Returns a row with a label and a value TextView; stores the TextView in `holder`. */
    private View makePathRow(String label, int unsetStringRes, ViewHolder holder) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(10), 0, 0);
        TextView labelView = new TextView(this);
        labelView.setText(label);
        labelView.setTextColor(0xFFBAC2CC);
        labelView.setTextSize(11.0f);
        row.addView(labelView);
        TextView value = new TextView(this);
        value.setText(getString(unsetStringRes));
        value.setTextColor(0xFFE8EAED);
        value.setTextSize(12.0f);
        row.addView(value, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        holder.set(value);
        return row;
    }

    /** Holder that captures a single TextView reference for later mutation. */
    private static final class TextViewHolder implements ViewHolder {
        TextView view;
        @Override public void set(View v) { this.view = (TextView) v; }
    }

    private View makeSpinnerRow(String label, String[] options, int initialSelection,
                                IntConsumer onSelected) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.VERTICAL);
        row.setPadding(0, dp(10), 0, 0);
        TextView labelView = new TextView(this);
        labelView.setText(label);
        labelView.setTextColor(0xFFBAC2CC);
        labelView.setTextSize(11.0f);
        row.addView(labelView);
        Spinner spinner = new Spinner(this);
        spinner.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, options));
        spinner.setSelection(initialSelection);
        spinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, View view, int position, long id) {
                onSelected.accept(position);
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) { }
        });
        row.addView(spinner, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        return row;
    }

    private SwitchCompat makeSwitchRow(String label, boolean initial,
                                       Consumer<Boolean> onChecked) {
        SwitchCompat sw = new SwitchCompat(this);
        sw.setText(label);
        sw.setTextColor(0xFFE8EAED);
        sw.setTextSize(13.0f);
        sw.setChecked(initial);
        sw.setOnCheckedChangeListener((buttonView, isChecked) -> onChecked.accept(isChecked));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        params.setMargins(0, dp(10), 0, 0);
        sw.setLayoutParams(params);
        return sw;
    }

    private View createGameLibraryScreen() {
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.VERTICAL);
        screen.setPadding(dp(18), dp(12), dp(18), dp(12));
        screen.setBackgroundColor(0xFA0B0D10);
        screen.setClickable(true);
        screen.setFocusable(true);

        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setOrientation(LinearLayout.HORIZONTAL);
        TextView title = new TextView(this);
        title.setText("Game Library");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(18.0f);
        header.addView(title, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        gameLibraryViewButton = makeCompactButton("Carousel", v -> toggleGameLibraryView());
        header.addView(gameLibraryViewButton);
        header.addView(makeCompactButton("Refresh", v -> refreshGameLibraryScreen()));
        header.addView(makeCompactButton("Bezels", v -> showDownloadBezelsDialog()));
        header.addView(makeCompactButton("X", v -> closeGameLibraryScreen()));
        screen.addView(header);

        gameLibrarySearch = new EditText(this);
        gameLibrarySearch.setSingleLine(true);
        gameLibrarySearch.setHint("Search games");
        gameLibrarySearch.setTextColor(0xFFFFFFFF);
        gameLibrarySearch.setHintTextColor(0xFF8C939D);
        gameLibrarySearch.setPadding(dp(10), 0, dp(10), 0);
        LinearLayout.LayoutParams searchParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(42));
        searchParams.setMargins(0, dp(10), 0, dp(8));
        screen.addView(gameLibrarySearch, searchParams);

        gameLibraryStatus = new TextView(this);
        gameLibraryStatus.setTextColor(0xFFBAC2CC);
        gameLibraryStatus.setTextSize(12.0f);
        gameLibraryStatus.setPadding(0, 0, 0, dp(8));
        screen.addView(gameLibraryStatus);

        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(false);
        gameLibraryGrid = new GridLayout(this);
        gameLibraryGrid.setColumnCount(4);
        gameLibraryGrid.setUseDefaultMargins(false);
        scroll.addView(gameLibraryGrid, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT,
                ScrollView.LayoutParams.WRAP_CONTENT));
        screen.addView(scroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));

        gameLibraryCarouselScroll = new HorizontalScrollView(this);
        gameLibraryCarouselScroll.setFillViewport(false);
        gameLibraryCarouselScroll.setVisibility(View.GONE);
        gameLibraryCarousel = new LinearLayout(this);
        gameLibraryCarousel.setOrientation(LinearLayout.HORIZONTAL);
        gameLibraryCarousel.setGravity(Gravity.CENTER_VERTICAL);
        gameLibraryCarouselScroll.addView(gameLibraryCarousel, new HorizontalScrollView.LayoutParams(
                HorizontalScrollView.LayoutParams.WRAP_CONTENT,
                HorizontalScrollView.LayoutParams.MATCH_PARENT));
        screen.addView(gameLibraryCarouselScroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));

        gameLibrarySearch.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {
                renderGameLibraryCards();
            }
            @Override public void afterTextChanged(Editable s) {}
        });
        return screen;
    }

    private View createMapperOverlay() {
        LinearLayout screen = new LinearLayout(this);
        screen.setOrientation(LinearLayout.VERTICAL);
        screen.setBackgroundColor(0xFA0B0D10);
        screen.setClickable(true);
        screen.setFocusable(true);
        screen.setPadding(dp(16), dp(12), dp(16), dp(12));

        // Header.
        LinearLayout header = new LinearLayout(this);
        header.setGravity(Gravity.CENTER_VERTICAL);
        header.setOrientation(LinearLayout.HORIZONTAL);
        TextView title = new TextView(this);
        title.setText("Controller");
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(18.0f);
        header.addView(title, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        mapperBackButton = makeCompactButton("X", v -> closeMapperOverlay());
        header.addView(mapperBackButton);
        screen.addView(header);

        mapperStatusText = new TextView(this);
        mapperStatusText.setTextColor(0xFFC8D0DB);
        mapperStatusText.setTextSize(13.0f);
        mapperStatusText.setPadding(0, dp(6), 0, dp(6));
        screen.addView(mapperStatusText);

        // --- External-controller mapping: tappable hotspots over the 3DO pad.
        ThreeDoControllerView diagram = new ThreeDoControllerView(this);
        mapperHotspots.clear();
        for (ThreeDoControllerView.Hotspot hs : ThreeDoControllerView.hotspots()) {
            Button b = makeHotspotButton(hs.shape);
            b.setTag(hs);
            mapperHotspots.put(hs.buttonIndex, b);
            addMapperTouchFeedback(b, hs.buttonIndex);
            diagram.addView(b);
        }
        // D-pad uses the same cross view as the on-screen pad; tap an arm to bind.
        editorDpad = new DpadView(this);
        editorDpad.setEditorMode(true);
        editorDpad.setTag(ThreeDoControllerView.dpadRegion());
        editorDpad.setArmTapListener(idx -> {
            mapperWaitingForButton = idx;
            updateMapperWaitingState();
            toast("Press a controller button for 3DO " + ControllerMappingManager.buttonName(idx));
        });
        diagram.addView(editorDpad);
        FrameLayout diagramHost = new FrameLayout(this);
        diagramHost.addView(diagram, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        screen.addView(diagramHost, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));

        // --- On-screen touch-pad controls.
        LinearLayout touchRow = new LinearLayout(this);
        touchRow.setOrientation(LinearLayout.HORIZONTAL);
        touchRow.setGravity(Gravity.CENTER_VERTICAL);
        touchRow.setPadding(0, dp(8), 0, 0);
        TextView touchLabel = new TextView(this);
        touchLabel.setText("On-screen touch pad");
        touchLabel.setTextColor(0xFFC8D0DB);
        touchLabel.setTextSize(13.0f);
        touchRow.addView(touchLabel, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        padShowHideButton = makeCompactButton(virtualPadVisible ? "Hide" : "Show", v -> toggleVirtualPad());
        touchRow.addView(padShowHideButton);
        padEditButton = makeCompactButton("Edit Layout", v -> togglePadEditMode());
        touchRow.addView(padEditButton);
        touchRow.addView(makeCompactButton("Reset", v -> resetPadLayout()));
        screen.addView(touchRow);

        refreshMapperLabels();
        return screen;
    }

    // Hotspot styling states.
    private static final int HS_UNASSIGNED = 0;
    private static final int HS_ASSIGNED = 1;
    private static final int HS_SELECTED = 2;

    // Settings status colours: green = configured/present, neutral = not set.
    private static final int STATUS_OK_GREEN = 0xFF32D17A;
    private static final int STATUS_NEUTRAL = 0xFFC8D0DB;

    /** Tap target laid over a button on the 3DO pad diagram. */
    private Button makeHotspotButton(ThreeDoControllerView.Shape shape) {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setTextColor(0xFFFFFFFF);
        b.setTextSize(9.0f);
        b.setPadding(0, 0, 0, 0);
        b.setBackground(hotspotBackground(shape, HS_UNASSIGNED));
        return b;
    }

    private GradientDrawable hotspotBackground(ThreeDoControllerView.Shape shape, int state) {
        GradientDrawable d = new GradientDrawable();
        switch (shape) {
            case PILL:
                d.setShape(GradientDrawable.RECTANGLE);
                d.setCornerRadius(dp(999));
                break;
            case ARM:
                d.setShape(GradientDrawable.RECTANGLE);
                d.setCornerRadius(dp(7));
                break;
            default:
                d.setShape(GradientDrawable.OVAL);
                break;
        }
        int fill, line;
        switch (state) {
            case HS_SELECTED:                       // currently waiting for a press
                fill = 0x553BA7FF; line = 0xFF3BA7FF; break;
            case HS_ASSIGNED:                       // mapped — colour it in
                fill = 0x6632D17A; line = 0xFF32D17A; break;
            default:                                // unassigned
                fill = 0x22FFFFFF; line = 0x55FFFFFF; break;
        }
        d.setColor(fill);
        d.setStroke(dp(state == HS_SELECTED ? 2 : 1), line);
        return d;
    }

    private View createDisplaySwitchOverlay() {
        FrameLayout overlay = new FrameLayout(this);
        overlay.setClickable(true);
        overlay.setFocusable(true);
        overlay.setBackgroundColor(0x66000000);
        TextView label = new TextView(this);
        label.setText("Applying display...");
        label.setTextColor(0xFFFFFFFF);
        label.setTextSize(16.0f);
        label.setGravity(Gravity.CENTER);
        label.setPadding(dp(18), dp(12), dp(18), dp(12));
        label.setBackgroundColor(0xEE181B1F);
        overlay.addView(label, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER));
        return overlay;
    }

    private View createLoadingOverlay() {
        LinearLayout root = new LinearLayout(this);
        root.setGravity(Gravity.CENTER);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0x99000000);
        android.widget.ProgressBar progress = new android.widget.ProgressBar(this);
        root.addView(progress);
        loadingText = new TextView(this);
        loadingText.setText("Starting emulator...");
        loadingText.setTextColor(0xFFFFFFFF);
        loadingText.setTextSize(16.0f);
        loadingText.setPadding(0, dp(12), 0, 0);
        root.addView(loadingText);
        return root;
    }

    // ------------------------------------------------------------ virtual pad

    /** Default placement (centre as a fraction of the overlay) + size of a touch button. */
    private static final class PadSpec {
        final int idx;
        final String label;
        final int wDp, hDp;
        final float defCx, defCy;
        PadSpec(int idx, String label, int wDp, int hDp, float defCx, float defCy) {
            this.idx = idx; this.label = label; this.wDp = wDp; this.hDp = hDp;
            this.defCx = defCx; this.defCy = defCy;
        }
    }

    // The D-pad is a single cross control (see DpadView); these are the
    // individually-tappable buttons. The cross-pad is added separately below.
    private static final int DPAD_ID = -100;       // sentinel for layout persistence
    private static final int DPAD_SIZE_DP = 150;
    private static final float DPAD_DEF_CX = 0.13f, DPAD_DEF_CY = 0.72f;

    private static final PadSpec[] PAD_SPECS = {
            new PadSpec(ControllerMappingManager.BUTTON_L1, "L", 96, 44, 0.12f, 0.14f),
            new PadSpec(ControllerMappingManager.BUTTON_R1, "R", 96, 44, 0.88f, 0.14f),
            new PadSpec(ControllerMappingManager.BUTTON_C, "C", 62, 62, 0.95f, 0.56f),
            new PadSpec(ControllerMappingManager.BUTTON_B, "B", 62, 62, 0.89f, 0.69f),
            new PadSpec(ControllerMappingManager.BUTTON_A, "A", 62, 62, 0.82f, 0.82f),
            new PadSpec(ControllerMappingManager.BUTTON_PLAY_PAUSE, "P", 60, 40, 0.44f, 0.92f),
            new PadSpec(ControllerMappingManager.BUTTON_STOP, "X", 60, 40, 0.56f, 0.92f),
    };

    // Active drag bookkeeping (single control at a time, edit mode only).
    private float padDragDownX, padDragDownY;
    private int padDragStartLeft, padDragStartTop;

    private View createVirtualPadOverlay() {
        FrameLayout overlay = new FrameLayout(this);
        overlay.setClickable(true);
        overlay.setFocusable(false);
        overlay.setOnTouchListener((view, event) -> {
            if (!padEditMode && event.getActionMasked() == MotionEvent.ACTION_UP) {
                showTopControlsTemporarily();
            }
            return true;
        });
        virtualPadContainer = overlay;

        padButtons.clear();
        padSpecs.clear();
        for (PadSpec spec : PAD_SPECS) {
            Button button = makeVirtualPadButton(spec);
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                    dp(spec.wDp), dp(spec.hDp), Gravity.TOP | Gravity.LEFT);
            overlay.addView(button, lp);
            padButtons.add(button);
            padSpecs.add(spec);
        }

        // Single cross-style D-pad (8-way; diagonals press two directions).
        dpadView = new DpadView(this);
        dpadView.setListener((up, down, left, right) -> {
            if (!emulatorStarted) return;
            setInputState(ControllerMappingManager.BUTTON_DPAD_UP, up);
            setInputState(ControllerMappingManager.BUTTON_DPAD_DOWN, down);
            setInputState(ControllerMappingManager.BUTTON_DPAD_LEFT, left);
            setInputState(ControllerMappingManager.BUTTON_DPAD_RIGHT, right);
        });
        // In edit mode the host consumes the gesture to drag; otherwise it
        // returns false so DpadView.onTouchEvent reads directions.
        PadSpec dpadSpec = new PadSpec(DPAD_ID, "", DPAD_SIZE_DP, DPAD_SIZE_DP, DPAD_DEF_CX, DPAD_DEF_CY);
        dpadView.setOnTouchListener((view, event) ->
                padEditMode && handlePadDrag(view, event, dpadSpec));
        overlay.addView(dpadView, new FrameLayout.LayoutParams(
                dp(DPAD_SIZE_DP), dp(DPAD_SIZE_DP), Gravity.TOP | Gravity.LEFT));
        padButtons.add(dpadView);
        padSpecs.add(dpadSpec);

        // Reposition once the overlay has real bounds (and on rotation/resize).
        overlay.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if ((r - l) != (or - ol) || (b - t) != (ob - ot)) {
                positionPadButtons();
            }
        });

        // Drag banner shown only in edit mode.
        padEditBanner = new TextView(this);
        padEditBanner.setText("Drag buttons to reposition · tap here when done");
        padEditBanner.setTextColor(0xFFEAF0F7);
        padEditBanner.setTextSize(13.0f);
        padEditBanner.setGravity(Gravity.CENTER);
        padEditBanner.setBackgroundColor(0xCC1E66B0);
        padEditBanner.setPadding(dp(12), dp(8), dp(12), dp(8));
        padEditBanner.setClickable(true);
        padEditBanner.setOnClickListener(v -> setPadEditMode(false));
        padEditBanner.setVisibility(View.GONE);
        overlay.addView(padEditBanner, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP));
        return overlay;
    }

    private Button makeVirtualPadButton(PadSpec spec) {
        Button button = makeCompactButton(spec.label, null);
        button.setTextColor(0xEEFFFFFF);
        button.setTextSize(spec.hDp >= 54 ? 18.0f : 14.0f);
        button.setBackground(virtualPadButtonBackground(false));
        button.setOnTouchListener((view, event) -> {
            if (padEditMode) {
                return handlePadDrag(view, event, spec);
            }
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_POINTER_DOWN:
                    setInputState(spec.idx, true);
                    view.setBackground(virtualPadButtonBackground(true));
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_POINTER_UP:
                case MotionEvent.ACTION_CANCEL:
                    setInputState(spec.idx, false);
                    view.setBackground(virtualPadButtonBackground(false));
                    return true;
                default:
                    return true;
            }
        });
        return button;
    }

    private boolean handlePadDrag(View view, MotionEvent event, PadSpec spec) {
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) view.getLayoutParams();
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                padDragDownX = event.getRawX();
                padDragDownY = event.getRawY();
                padDragStartLeft = lp.leftMargin;
                padDragStartTop = lp.topMargin;
                view.setBackground(virtualPadButtonBackground(true));
                return true;
            case MotionEvent.ACTION_MOVE: {
                int w = virtualPadContainer.getWidth();
                int h = virtualPadContainer.getHeight();
                int nl = clamp(padDragStartLeft + (int) (event.getRawX() - padDragDownX), 0, w - view.getWidth());
                int nt = clamp(padDragStartTop + (int) (event.getRawY() - padDragDownY), 0, h - view.getHeight());
                lp.leftMargin = nl;
                lp.topMargin = nt;
                view.setLayoutParams(lp);
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL: {
                int w = virtualPadContainer.getWidth();
                int h = virtualPadContainer.getHeight();
                if (w > 0 && h > 0) {
                    float cx = (lp.leftMargin + view.getWidth() / 2f) / w;
                    float cy = (lp.topMargin + view.getHeight() / 2f) / h;
                    prefs.edit()
                            .putFloat(padPosKey(spec.idx, "cx"), cx)
                            .putFloat(padPosKey(spec.idx, "cy"), cy)
                            .apply();
                }
                view.setBackground(virtualPadButtonBackground(false));
                return true;
            }
            default:
                return true;
        }
    }

    private void positionPadButtons() {
        if (virtualPadContainer == null) return;
        int w = virtualPadContainer.getWidth();
        int h = virtualPadContainer.getHeight();
        if (w <= 0 || h <= 0) return;
        for (int i = 0; i < padButtons.size(); i++) {
            View button = padButtons.get(i);
            PadSpec spec = padSpecs.get(i);
            float cx = prefs.getFloat(padPosKey(spec.idx, "cx"), spec.defCx);
            float cy = prefs.getFloat(padPosKey(spec.idx, "cy"), spec.defCy);
            int bw = dp(spec.wDp);
            int bh = dp(spec.hDp);
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) button.getLayoutParams();
            lp.leftMargin = clamp(Math.round(cx * w - bw / 2f), 0, w - bw);
            lp.topMargin = clamp(Math.round(cy * h - bh / 2f), 0, h - bh);
            button.setLayoutParams(lp);
        }
    }

    private static String padPosKey(int idx, String axis) {
        return "pad_" + axis + "_" + idx;
    }

    private static int clamp(int v, int lo, int hi) {
        if (hi < lo) return lo;
        return v < lo ? lo : (v > hi ? hi : v);
    }

    private GradientDrawable virtualPadButtonBackground(boolean pressed) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setShape(GradientDrawable.RECTANGLE);
        drawable.setCornerRadius(dp(999));
        drawable.setColor(pressed ? 0x99C5CBD3 : 0x665F6670);
        drawable.setStroke(dp(1), pressed ? 0xEEFFFFFF : 0x99D6DADF);
        return drawable;
    }

    // ----------------------------------------------------- mapper overlay ops

    private void addMapperTouchFeedback(final Button button, final int buttonIndex) {
        button.setOnClickListener(v -> {
            mapperWaitingForButton = buttonIndex;
            updateMapperWaitingState();
            toast("Press a controller button for 3DO " + ControllerMappingManager.buttonName(buttonIndex));
        });
    }

    private void refreshMapperLabels() {
        updateMapperWaitingState();
    }

    private void updateMapperWaitingState() {
        if (mapperStatusText != null) {
            if (mapperWaitingForButton >= 0) {
                mapperStatusText.setText("Press a controller button for "
                        + ControllerMappingManager.buttonName(mapperWaitingForButton));
            } else {
                mapperStatusText.setText("Tap a button on the pad, then press a controller button to map it.");
            }
        }
        for (java.util.Map.Entry<Integer, Button> e : mapperHotspots.entrySet()) {
            applyHotspotStyle(e.getValue(), e.getKey());
        }
        if (editorDpad != null) {
            editorDpad.setArmState(ControllerMappingManager.BUTTON_DPAD_UP,
                    hotspotState(ControllerMappingManager.BUTTON_DPAD_UP));
            editorDpad.setArmState(ControllerMappingManager.BUTTON_DPAD_DOWN,
                    hotspotState(ControllerMappingManager.BUTTON_DPAD_DOWN));
            editorDpad.setArmState(ControllerMappingManager.BUTTON_DPAD_LEFT,
                    hotspotState(ControllerMappingManager.BUTTON_DPAD_LEFT));
            editorDpad.setArmState(ControllerMappingManager.BUTTON_DPAD_RIGHT,
                    hotspotState(ControllerMappingManager.BUTTON_DPAD_RIGHT));
            editorDpad.invalidate();
        }
    }

    /** Style state for a 3DO button: selected (waiting) > assigned > unassigned. */
    private int hotspotState(int buttonIndex) {
        if (mapperWaitingForButton == buttonIndex) return HS_SELECTED;
        return ControllerMappingManager.getMappedKeyCode(this, buttonIndex) > 0
                ? HS_ASSIGNED : HS_UNASSIGNED;
    }

    /** Colour a hotspot by state and label it with its current binding. */
    private void applyHotspotStyle(Button button, int buttonIndex) {
        ThreeDoControllerView.Shape shape = ((ThreeDoControllerView.Hotspot) button.getTag()).shape;
        int keyCode = ControllerMappingManager.getMappedKeyCode(this, buttonIndex);
        button.setBackground(hotspotBackground(shape, hotspotState(buttonIndex)));
        button.setText(keyCode > 0 ? ControllerMappingManager.keyName(keyCode) : "");
    }

    private boolean isGameControllerEvent(KeyEvent event) {
        int source = event.getSource();
        return ((source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD)
                || ((source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                || ((source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD);
    }

    /**
     * Open the controller screen from the settings panel. The settings panel is
     * dismissed first so that closing the controller screen returns straight to
     * the game and resumes it (otherwise the still-visible settings panel keeps
     * the emulator paused and the screen feels stuck).
     */
    private void openControllerScreen() {
        if (settingsPanel != null) settingsPanel.setVisibility(View.GONE);
        if (mapperOverlay != null && mapperOverlay.getVisibility() != View.VISIBLE) {
            toggleMapperOverlay();
        }
    }

    private void toggleMapperOverlay() {
        if (mapperOverlay == null) return;
        boolean show = mapperOverlay.getVisibility() != View.VISIBLE;
        if (show) {
            pauseForOverlay();
            mapperOverlay.setVisibility(View.VISIBLE);
            mapperOverlay.bringToFront();
            refreshMapperLabels();
        } else {
            closeMapperOverlay();
        }
    }

    private void closeMapperOverlay() {
        if (mapperOverlay == null) return;
        mapperWaitingForButton = -1;
        mapperOverlay.setVisibility(View.GONE);
        resumeAfterOverlayIfIdle();
    }

    // ----------------------------------------------------- settings panel ops

    private void toggleSettingsPanel() {
        if (settingsPanel == null) return;
        boolean show = settingsPanel.getVisibility() != View.VISIBLE;
        if (show) {
            pauseForOverlay();
            settingsPanel.setVisibility(View.VISIBLE);
            settingsPanel.bringToFront();
            refreshSidePathTexts();
        } else {
            settingsPanel.setVisibility(View.GONE);
            resumeAfterOverlayIfIdle();
        }
        updateButtonLabels();
    }

    private void refreshSidePathTexts() {
        if (sideBiosPathText == null) return;
        String biosPath = prefs.getString(KEY_BIOS_PATH, "");
        boolean biosSet = biosPath != null && !biosPath.isEmpty();
        sideBiosPathText.setText(biosSet
                ? getString(R.string.bios_path_value, getDisplayPath(biosPath))
                : getString(R.string.bios_not_set));
        sideBiosPathText.setTextColor(biosSet ? STATUS_OK_GREEN : STATUS_NEUTRAL);

        String libraryPath = prefs.getString(KEY_LIBRARY_FOLDER, "");
        boolean librarySet = libraryPath != null && !libraryPath.isEmpty();
        sideLibraryPathText.setText(librarySet
                ? getString(R.string.library_path_value, getDisplayPath(libraryPath))
                : getString(R.string.library_not_set));
        sideLibraryPathText.setTextColor(librarySet ? STATUS_OK_GREEN : STATUS_NEUTRAL);

        String driverPath = prefs.getString(KEY_VULKAN_DRIVER_PATH, "");
        sideVulkanDriverPathText.setText(driverPath == null || driverPath.isEmpty()
                ? getString(R.string.vulkan_driver_system)
                : getDisplayPath(driverPath));

        // Green-tint the Download Bezels card once a bezel pack is present.
        if (bezelDownloadCard != null) {
            boolean haveBezels = BezelResolver.countDownloadedBezels(this) > 0;
            bezelDownloadCard.setBackground(haveBezels
                    ? cardBackground(0xFF14301F, STATUS_OK_GREEN)
                    : cardBackground(0xFF22272E, 0xFF3D4652));
        }
    }

    private void toggleDebugInfo() {
        statusIndicator.setVisibility(debugModeEnabled ? View.VISIBLE : View.GONE);
        if (debugModeEnabled) {
            statusIndicator.setText("Running: " + (mGamePath == null ? "BIOS" : new File(mGamePath).getName()));
        } else {
            statusIndicator.setText("");
        }
    }

    // ---------------------------------------------------------- play bar ops

    private void showTopControlsTemporarily() {
        if (topControlsBar == null) return;
        // An open overlay (controller, settings, library) owns the top-right
        // corner — keep the pause bar hidden so it can't cover the close button.
        if (hasVisibleRuntimeOverlay()) {
            topControlsBar.setVisibility(View.GONE);
            return;
        }
        topControlsBar.setVisibility(View.VISIBLE);
        topControlsBar.bringToFront();
        scheduleTopControlsAutoHide();
    }

    private void scheduleTopControlsAutoHide() {
        fpsHandler.removeCallbacks(hideTopControlsRunnable);
        fpsHandler.postDelayed(hideTopControlsRunnable, TOP_CONTROLS_AUTO_HIDE_MS);
    }

    private void resizeScreenSurface() {
        if (emulatorSurfaceView == null) return;
        SurfaceHolder holder = emulatorSurfaceView.getHolder();
        Surface s = holder.getSurface();
        if (s == null || !s.isValid()) return;
        int w = emulatorSurfaceView.getWidth();
        int h = emulatorSurfaceView.getHeight();
        if (w > 0 && h > 0) {
            applyRendererDefaults(w, h, s);
        }
    }

    // ------------------------------------------------------ button builders

    private Button makeButton(String label, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        button.setOnClickListener(listener);
        button.setMinHeight(48);
        return button;
    }

    private Button makeCompactButton(String label, View.OnClickListener listener) {
        Button button = makeButton(label, listener);
        button.setTextSize(12.0f);
        button.setMinWidth(dp(64));
        button.setMinimumWidth(dp(64));
        button.setMinHeight(dp(32));
        button.setMinimumHeight(dp(32));
        button.setPadding(dp(6), 0, dp(6), 0);
        return button;
    }

    private View makeSettingsActionCard(String icon, String title, String subtitle,
                                        View.OnClickListener listener) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(12), dp(10), dp(12), dp(10));
        card.setBackground(cardBackground(0xFF22272E, 0xFF3D4652));
        card.setClickable(true);
        card.setFocusable(true);
        card.setOnClickListener(listener);

        TextView iconView = new TextView(this);
        iconView.setText(icon);
        iconView.setTextColor(0xFFFFFFFF);
        iconView.setTextSize(18.0f);
        iconView.setGravity(Gravity.CENTER);
        iconView.setBackground(cardBackground(0xFF303844, 0xFF596474));
        card.addView(iconView, new LinearLayout.LayoutParams(dp(48), dp(48)));

        LinearLayout textColumn = new LinearLayout(this);
        textColumn.setOrientation(LinearLayout.VERTICAL);
        textColumn.setGravity(Gravity.CENTER_VERTICAL);
        textColumn.setPadding(dp(12), 0, 0, 0);

        TextView titleView = new TextView(this);
        titleView.setText(title);
        titleView.setTextColor(0xFFFFFFFF);
        titleView.setTextSize(14.0f);
        titleView.setMaxLines(1);
        textColumn.addView(titleView);

        TextView subtitleView = new TextView(this);
        subtitleView.setText(subtitle);
        subtitleView.setTextColor(0xFFBAC2CC);
        subtitleView.setTextSize(11.0f);
        subtitleView.setMaxLines(2);
        textColumn.addView(subtitleView);

        card.addView(textColumn, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));

        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(76));
        params.setMargins(0, dp(12), 0, 0);
        card.setLayoutParams(params);
        return card;
    }

    private GradientDrawable cardBackground(int fillColor, int strokeColor) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fillColor);
        drawable.setCornerRadius(dp(8));
        drawable.setStroke(dp(1), strokeColor);
        return drawable;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    // --------------------------------------------------- renderer + lifecycle

    private void applyRendererDefaults(int width, int height, Surface surface) {
        // Self-heal the driver path: if the prefs entry points to a file that
        // no longer exists (SD card unmounted, SAF tree revoked) but a same-
        // named .so is already in getFilesDir()/drivers/ (i.e. a previous
        // version of the app put it there), point the prefs at the internal
        // copy. The renderer's dlopen() can only reach .so files under
        // permitted_paths, and the app's filesDir is the canonical home for
        // anything we want to dlopen.
        String driverPath = prefs.getString(KEY_VULKAN_DRIVER_PATH, "");
        if (!driverPath.isEmpty() && !new File(driverPath).isFile()) {
            File internalDriver = new File(getFilesDir(), "drivers/" + new File(driverPath).getName());
            if (internalDriver.isFile()) {
                String migrated = internalDriver.getAbsolutePath();
                prefs.edit().putString(KEY_VULKAN_DRIVER_PATH, migrated).apply();
                driverPath = migrated;
                android.util.Log.i("3DOOpera", "Vulkan driver path repaired: now " + migrated);
            } else {
                android.util.Log.i("3DOOpera", "Vulkan driver path " + driverPath + " is missing — clearing prefs");
                prefs.edit().remove(KEY_VULKAN_DRIVER_PATH).apply();
                driverPath = "";
            }
        }
        if (!driverPath.isEmpty()) {
            colocateTurnipDependencyStubs(new File(driverPath));
        }
        setVulkanDriverPath(driverPath);
        setRendererType(currentRenderer);
        setFiltering(nearestFiltering);
        setAspectRatio(aspectRatio16by9);
        setAntiAliasingMode(antiAliasingMode);
        setCrtShaderEnabled(crtShaderEnabled);
        setResolutionScale(resolutionScale);
        setOutputResolutionPreset(outputResolutionPreset);
        setFlipVertical(false);
        setFlipX(false);
        setFlipY(false);
        setRotation(getEffectiveRotation());
        initRenderer(width, height);
        setSurface(surface);
    }

    /**
     * Co-locate the stub libraries (libhardware.so, libnativewindow.so,
     * libsync.so) next to the user-imported Turnip driver. Turnip's
     * libvulkan_freedreno.so lists these in DT_NEEDED, and the
     * classloader-namespace does not allow apps to dlopen the system
     * copies from /system/lib64. The dynamic loader's DT_NEEDED
     * resolution searches the same directory as the .so being loaded,
     * so dropping stub .so files alongside the driver satisfies the
     * link-time references.
     *
     * The stubs ship in the APK at lib/&lt;abi&gt;/lib&lt;name&gt;.so. With
     * useLegacyPackaging = false (the AGP 8+ default), the .so files
     * are NOT extracted to nativeLibraryDir — they live inside the APK
     * and the loader reads them in place. So we extract the stubs to
     * the driver directory ourselves at first launch.
     */
    private void colocateTurnipDependencyStubs(File driverFile) {
        File driverDir = driverFile.getParentFile();
        if (driverDir == null || !driverDir.isDirectory()) {
            return;
        }
        String abi = Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : null;
        if (abi == null || abi.isEmpty()) {
            return;
        }
        String apkPath = getApplicationInfo().sourceDir;
        if (apkPath == null || apkPath.isEmpty()) {
            return;
        }
        String[] stubSonames = {"libhardware.so", "libnativewindow.so", "libsync.so"};
        for (String soname : stubSonames) {
            File dst = new File(driverDir, soname);
            // Skip if already copied (size is a reasonable proxy — the stub
            // contents don't change between app versions; if they did, the
            // version-based check below would catch it).
            if (dst.isFile() && dst.length() > 1024) {
                continue;
            }
            String apkEntry = "lib/" + abi + "/" + soname;
            try (java.io.InputStream in = openApkEntry(apkPath, apkEntry);
                 java.io.OutputStream out = new java.io.FileOutputStream(dst)) {
                if (in == null) {
                    android.util.Log.w("3DOOpera", "Stub " + apkEntry + " not found in APK — Turnip load will likely fail");
                    continue;
                }
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                android.util.Log.i("3DOOpera", "Co-located stub " + soname + " (" + dst.length() + " bytes) next to driver");
            } catch (java.io.IOException e) {
                android.util.Log.e("3DOOpera", "Failed to co-locate " + soname + ": " + e.getMessage());
            }
        }
    }

    /**
     * Read a single entry from a zip/APK file as a stream. Returns null
     * if the entry doesn't exist. We use java.util.zip.ZipFile (always
     * available, handles all compression methods).
     */
    private static java.io.InputStream openApkEntry(String apkPath, String entryName) {
        try {
            java.util.zip.ZipFile zip = new java.util.zip.ZipFile(apkPath);
            java.util.zip.ZipEntry entry = zip.getEntry(entryName);
            if (entry == null) {
                zip.close();
                return null;
            }
            // Return a stream that closes the zip on stream close.
            return new ZipFileInputStreamBridge(zip, zip.getInputStream(entry));
        } catch (java.io.IOException e) {
            return null;
        }
    }

    /** Tiny adapter that closes the parent ZipFile when the stream is closed. */
    private static final class ZipFileInputStreamBridge extends java.io.FilterInputStream {
        private final java.util.zip.ZipFile owner;
        ZipFileInputStreamBridge(java.util.zip.ZipFile owner, java.io.InputStream in) {
            super(in);
            this.owner = owner;
        }
        @Override
        public void close() throws java.io.IOException {
            try { super.close(); } finally { owner.close(); }
        }
    }

    private void handleStartupIntent(Intent intent) {
        if (intent != null && intent.hasExtra("game_path")) {
            mGamePath = intent.getStringExtra("game_path");
        }
        if (!EmulatorPathStore.isSupportedCdPath(mGamePath)) {
            String lastGamePath = EmulatorPathStore.getSavedLastGamePath(this);
            if (EmulatorPathStore.isSupportedCdPath(lastGamePath)) {
                mGamePath = lastGamePath;
            } else {
                mGamePath = null;
            }
        }
        String biosPath = EmulatorPathStore.getSavedBiosPath(this);
        if (!EmulatorPathStore.isValidFilePath(biosPath)) {
            toast(getString(R.string.bios_required));
            openBiosBrowser();
        } else if (mGamePath == null || mGamePath.isEmpty()) {
            initializeEmulatorAsync(null, false);
        } else {
            initializeEmulatorAsync(mGamePath, true);
        }
    }

    private void openBiosBrowser() {
        Intent intent = SafFileImporter.createOpenDocumentIntent();
        startActivityForResult(intent, REQ_IMPORT_BIOS);
    }

    private void openFileBrowser() {
        Intent intent = SafFileImporter.createOpenDocumentIntent();
        startActivityForResult(intent, REQ_IMPORT_GAME);
    }

    private void openLibraryPicker() {
        Intent intent = SafFileImporter.createOpenDocumentTreeIntent();
        startActivityForResult(intent, REQ_IMPORT_LIBRARY);
    }

    private void openDriverImport() {
        Intent intent = SafFileImporter.createOpenDocumentIntent();
        startActivityForResult(intent, REQ_IMPORT_DRIVER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != Activity.RESULT_OK || data == null || data.getData() == null) {
            // If a wizard-style flow started this, just refresh state.
            refreshSidePathTexts();
            return;
        }
        Uri uri = data.getData();
        if (requestCode == REQ_IMPORT_BIOS) {
            try {
                String biosPath = SafFileImporter.importBios(this, uri);
                EmulatorPathStore.saveBiosPath(this, biosPath);
                initializeEmulatorAsync(null, false);
                toast("BIOS imported");
            } catch (Exception e) {
                toast("BIOS import failed: " + e.getMessage());
            }
        } else if (requestCode == REQ_IMPORT_DRIVER) {
            try {
                String driverPath = SafFileImporter.importVulkanDriver(this, uri);
                prefs.edit().putString(KEY_VULKAN_DRIVER_PATH, driverPath).apply();
                refreshSidePathTexts();
                toast("GPU driver imported");
            } catch (Exception e) {
                toast("Driver import failed: " + e.getMessage());
            }
        } else if (requestCode == REQ_IMPORT_LIBRARY) {
            toast("Importing library...");
            new Thread(() -> {
                try {
                    SafFileImporter.ImportResult result = SafFileImporter.importLibraryTree(this, uri);
                    prefs.edit()
                            .putString(KEY_LIBRARY_FOLDER, result.path)
                            .putBoolean(KEY_LIBRARY_REFRESH_REQUIRED, true)
                            .apply();
                    uiHandler.post(() -> {
                        refreshSidePathTexts();
                        toast("Imported " + result.importedFileCount + " library files");
                    });
                } catch (Exception e) {
                    uiHandler.post(() -> toast("Library import failed: " + e.getMessage()));
                }
            }, "main-library-import").start();
        } else if (requestCode == REQ_IMPORT_GAME) {
            try {
                mGamePath = SafFileImporter.importGameFile(this, uri);
                EmulatorPathStore.saveLastGamePath(this, mGamePath);
                initializeEmulatorAsync(mGamePath, true);
            } catch (Exception e) {
                toast("Game import failed: " + e.getMessage());
            }
        }
    }

    // ------------------------------------------------------------ emulator

    private void performHardReset() {
        performHardReset(null);
    }

    private void performHardReset(String preferredGamePath) {
        stopAudioPlayback();
        shutdownEmulator();

        String biosPath = EmulatorPathStore.getSavedBiosPath(this);
        if (!EmulatorPathStore.isValidFilePath(biosPath)) {
            toast(getString(R.string.bios_required));
            openBiosBrowser();
            return;
        }

        String gameToLoad = preferredGamePath;
        if (!EmulatorPathStore.isSupportedCdPath(gameToLoad)) {
            gameToLoad = mGamePath;
        }
        if (!EmulatorPathStore.isSupportedCdPath(gameToLoad)) {
            String lastGamePath = EmulatorPathStore.getSavedLastGamePath(this);
            if (EmulatorPathStore.isSupportedCdPath(lastGamePath)) {
                gameToLoad = lastGamePath;
                mGamePath = lastGamePath;
            }
        }
        if (EmulatorPathStore.isSupportedCdPath(gameToLoad)) {
            mGamePath = gameToLoad;
        }
        initializeEmulatorAsync(gameToLoad, gameToLoad != null && !gameToLoad.isEmpty());
    }

    private void initializeEmulatorAsync(String gamePath, boolean loadingGame) {
        if (emulatorInitializing) {
            return;
        }
        emulatorInitializing = true;
        emulatorStarted = false;
        showLoadingOverlay(loadingGame ? "Loading game..." : "Starting emulator...");
        new Thread(() -> {
            boolean initialized;
            String biosPath = EmulatorPathStore.getSavedBiosPath(this);
            int region = prefs.getInt(KEY_REGION, REGION_NTSC);
            setRegion(region);
            initialized = initEmulator(gamePath, biosPath);
            runOnUiThread(() -> {
                emulatorInitializing = false;
                hideLoadingOverlay();
                if (!initialized) {
                    emulatorStarted = false;
                    String status = getStatus();
                    String message = "Failed to initialize emulator";
                    if (status != null && !status.isEmpty() && !"Not Running".equals(status)) {
                        message = status;
                    }
                    if (loadingGame && gamePath != null && !gamePath.isEmpty()) {
                        EmulatorPathStore.clearLastGamePath(this);
                        mGamePath = null;
                        currentBezelPath = "";
                        applyBezelVisibility();
                        toast(message + "; booting BIOS");
                        initializeEmulatorAsync(null, false);
                    } else {
                        toast(message);
                        toggleSettingsPanel();
                    }
                    return;
                }
                emulatorStarted = true;
                currentBezelPath = "";
                applyBezelVisibility();
                startAudioPlayback();
                String nvramPath = getNVRAMPath();
                if (nvramPath != null && !nvramPath.isEmpty()) {
                    File nvramFile = new File(nvramPath);
                    if (nvramFile.exists()) {
                        loadNVRAM(nvramPath);
                    }
                }
            });
        }, "emulator-init").start();
    }

    private void showLoadingOverlay(String message) {
        if (loadingText != null) loadingText.setText(message);
        if (loadingOverlay != null) loadingOverlay.setVisibility(View.VISIBLE);
    }

    private void hideLoadingOverlay() {
        if (loadingOverlay != null) loadingOverlay.setVisibility(View.GONE);
    }

    private void showPauseMenu() {
        manuallyPaused = true;
        pauseEmulator();
        new AlertDialog.Builder(this)
            .setTitle("Paused")
            .setItems(new CharSequence[]{"Resume", "Controller Mapping", "Close App"}, (dialog, which) -> {
                if (which == 0) {
                    manuallyPaused = false;
                    resumeEmulator();
                } else if (which == 1) {
                    manuallyPaused = false;
                    toggleMapperOverlay();
                } else if (which == 2) {
                    manuallyPaused = false;
                    finish();
                }
            })
            .setOnCancelListener(dialog -> {
                manuallyPaused = false;
                resumeEmulator();
            })
            .show();
    }

    private void bootBios() {
        mGamePath = null;
        stopAudioPlayback();
        shutdownEmulator();
        initializeEmulatorAsync(null, false);
        if (settingsPanel != null) settingsPanel.setVisibility(View.GONE);
        if (gameLibraryScreen != null) gameLibraryScreen.setVisibility(View.GONE);
        if (mapperOverlay != null) mapperOverlay.setVisibility(View.GONE);
        overlayPaused = false;
    }

    // ---------------------------------------------------------- NVRAM / audio

    private String getNVRAMPath() {
        File nvramDir = new File(SafFileImporter.getManagedAppDataDirectory(this), "nvram");
        if (!nvramDir.exists()) nvramDir.mkdirs();
        if (mGamePath == null || mGamePath.isEmpty()) {
            return new File(nvramDir, "default.nvram").getAbsolutePath();
        }
        String gameName = new File(mGamePath).getName();
        int dot = gameName.lastIndexOf('.');
        if (dot != -1) gameName = gameName.substring(0, dot);
        return new File(nvramDir, gameName + ".nvram").getAbsolutePath();
    }

    private void autoSaveNVRAM() {
        if (!emulatorStarted) return;
        String nvramPath = getNVRAMPath();
        if (nvramPath != null && !nvramPath.isEmpty()) {
            boolean saved = saveNVRAM(nvramPath);
            if (saved) Log.d("3DO-Main", "NVRAM auto-saved to: " + nvramPath);
        }
    }

    private void startAudioPlayback() {
        audioEngine.start(this::drainAudioFrames);
    }

    private void stopAudioPlayback() {
        audioEngine.stop();
    }

    // ------------------------------------------------------------ input

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (mapperOverlay != null && mapperOverlay.getVisibility() == View.VISIBLE
                && mapperWaitingForButton >= 0 && isGameControllerEvent(event)) {
            ControllerMappingManager.assignKeyCode(this, mapperWaitingForButton, keyCode);
            toast("Assigned " + ControllerMappingManager.buttonName(mapperWaitingForButton)
                    + " to " + ControllerMappingManager.keyName(keyCode));
            mapperWaitingForButton = -1;
            refreshMapperLabels();
            return true;
        }
        Integer button = EmulatorInputRouter.resolveMappedButton(this, keyCode, event);
        if (button != null) {
            setInputState(button, true);
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        Integer button = EmulatorInputRouter.resolveMappedButton(this, keyCode, event);
        if (button != null) {
            setInputState(button, false);
            return true;
        }
        return super.onKeyUp(keyCode, event);
    }

    // ----------------------------------------------------- pause / overlay

    private void pauseForOverlay() {
        if (!overlayPaused && emulatorStarted && !emulatorInitializing && !manuallyPaused) {
            pauseEmulator();
            overlayPaused = true;
        }
    }

    private void resumeAfterOverlayIfIdle() {
        if (!overlayPaused || hasVisibleRuntimeOverlay()) return;
        overlayPaused = false;
        if (emulatorStarted && !emulatorInitializing && !manuallyPaused) {
            resumeEmulator();
        }
    }

    private boolean hasVisibleRuntimeOverlay() {
        return (settingsPanel != null && settingsPanel.getVisibility() == View.VISIBLE)
                || (gameLibraryScreen != null && gameLibraryScreen.getVisibility() == View.VISIBLE)
                || (mapperOverlay != null && mapperOverlay.getVisibility() == View.VISIBLE);
    }

    private void releaseVirtualPadButtons() {
        if (dpadView != null) dpadView.release();
        if (!emulatorStarted) return;
        for (int button : VIRTUAL_PAD_BUTTONS) {
            setInputState(button, false);
        }
    }

    // ---------------------------------------------------------- toggles

    private void toggleVirtualPad() {
        virtualPadVisible = !virtualPadVisible;
        prefs.edit().putBoolean(KEY_VIRTUAL_PAD_VISIBLE, virtualPadVisible).apply();
        if (!virtualPadVisible && padEditMode) setPadEditMode(false);
        applyVirtualPadVisibility();
        updateButtonLabels();
        if (padShowHideButton != null) padShowHideButton.setText(virtualPadVisible ? "Hide" : "Show");
    }

    private void applyVirtualPadVisibility() {
        if (virtualPadOverlay == null) return;
        if (!virtualPadVisible) releaseVirtualPadButtons();
        virtualPadOverlay.setVisibility(virtualPadVisible ? View.VISIBLE : View.GONE);
        if (virtualPadVisible) virtualPadOverlay.post(this::positionPadButtons);
    }

    private void togglePadEditMode() {
        // Editing only makes sense while the pad is on-screen; turn it on first.
        if (!virtualPadVisible) toggleVirtualPad();
        boolean entering = !padEditMode;
        setPadEditMode(entering);
        // The controller panel is opaque, so close it to reveal the draggable
        // pad over the game; the banner's "tap here when done" exits edit mode.
        if (entering) closeMapperOverlay();
    }

    private void setPadEditMode(boolean enabled) {
        padEditMode = enabled;
        if (padEditBanner != null) padEditBanner.setVisibility(enabled ? View.VISIBLE : View.GONE);
        if (padEditButton != null) padEditButton.setText(enabled ? "Done" : "Edit Layout");
        if (enabled) releaseVirtualPadButtons(); // clear any held inputs before dragging
    }

    private void resetPadLayout() {
        SharedPreferences.Editor editor = prefs.edit();
        for (PadSpec spec : PAD_SPECS) {
            editor.remove(padPosKey(spec.idx, "cx")).remove(padPosKey(spec.idx, "cy"));
        }
        editor.remove(padPosKey(DPAD_ID, "cx")).remove(padPosKey(DPAD_ID, "cy"));
        editor.apply();
        positionPadButtons();
        toast("Touch pad layout reset");
    }

    private void cycleRenderer() {
        if (currentRenderer == RENDERER_VULKAN) currentRenderer = RENDERER_OPENGL_ES;
        else if (currentRenderer == RENDERER_OPENGL_ES) currentRenderer = RENDERER_SOFTWARE;
        else currentRenderer = RENDERER_VULKAN;
        if (!isHardwareRendererSelected()) {
            resolutionScale = 1;
            outputResolutionPreset = 0;
            setResolutionScale(resolutionScale);
            setOutputResolutionPreset(0);
        }
        setRendererType(currentRenderer);
        rebindRendererSurface();
        saveSettings();
        updateButtonLabels();
        updateStatusIndicator();
    }

    private void toggleAspectMode() {
        aspectRatio16by9 = !aspectRatio16by9;
        setAspectRatio(aspectRatio16by9);
        saveSettings();
        updateButtonLabels();
        updateStatusIndicator();
    }

    private void toggleTextureFilter() {
        nearestFiltering = !nearestFiltering;
        setFiltering(nearestFiltering);
        saveSettings();
        updateButtonLabels();
        updateStatusIndicator();
    }

    private void toggleBezelOverlay() {
        bezelEnabled = !bezelEnabled;
        prefs.edit().putBoolean(KEY_BEZEL_ENABLED, bezelEnabled).apply();
        applyBezelVisibility();
        updateButtonLabels();
    }

    private void toggleCrtShader() {
        crtShaderEnabled = !crtShaderEnabled;
        setCrtShaderEnabled(crtShaderEnabled);
        saveSettings();
        updateButtonLabels();
        updateStatusIndicator();
    }

    private void applyBezelVisibility() {
        if (emulatorBezel != null) {
            applyGameBezelImage();
            emulatorBezel.setVisibility(bezelEnabled ? View.VISIBLE : View.GONE);
        }
    }

    private void applyGameBezelImage() {
        if (emulatorBezel == null) return;
        File bezelFile = BezelResolver.findBezelForGame(this, mGamePath);
        String newPath = bezelFile == null ? "" : bezelFile.getAbsolutePath();
        if (newPath.equals(currentBezelPath)) return;
        currentBezelPath = newPath;
        if (bezelFile != null && bezelFile.isFile()) {
            emulatorBezel.setImageURI(Uri.fromFile(bezelFile));
        } else {
            emulatorBezel.setImageResource(R.drawable.bezel_fz10);
        }
    }

    // ----------------------------------------------------- save / load

    private void loadSettings() {
        debugModeEnabled = prefs.getBoolean(KEY_DEBUG_OVERLAY_ENABLED, false);
        aspectRatio16by9 = prefs.getBoolean(KEY_ASPECT_RATIO_16BY9, false);
        currentRenderer = normalizeRendererType(prefs.getInt(KEY_RENDERER_TYPE, RENDERER_VULKAN));
        nearestFiltering = prefs.getBoolean(KEY_NEAREST_FILTERING, false);
        antiAliasingMode = prefs.getInt(KEY_ANTI_ALIASING_MODE, 0);
        if (antiAliasingMode < 0 || antiAliasingMode > 2) antiAliasingMode = 0;
        crtShaderEnabled = prefs.getBoolean(KEY_CRT_ENABLED, false);
        bezelEnabled = prefs.getBoolean(KEY_BEZEL_ENABLED, true);
        virtualPadVisible = prefs.getBoolean(KEY_VIRTUAL_PAD_VISIBLE, false);
        resolutionScale = prefs.getInt(KEY_RESOLUTION_SCALE, 0);
        if (resolutionScale < 0 || resolutionScale > MAX_RESOLUTION_SCALE) resolutionScale = 0;
        outputResolutionPreset = prefs.getInt(KEY_OUTPUT_RESOLUTION_PRESET, 0);
        if (!(outputResolutionPreset == 0 || outputResolutionPreset == 720
                || outputResolutionPreset == 1080 || outputResolutionPreset == 1440
                || outputResolutionPreset == 2160)) {
            outputResolutionPreset = 0;
        }
        if (!isHardwareRendererSelected()) {
            resolutionScale = 1;
            outputResolutionPreset = 0;
        }
        flipVertical = false;
        setFlipVertical(false);
        setFlipX(false);
        setFlipY(false);
    }

    private void saveSettings() {
        prefs.edit()
            .putBoolean(KEY_ASPECT_RATIO_16BY9, aspectRatio16by9)
            .putInt(KEY_RENDERER_TYPE, currentRenderer)
            .putBoolean(KEY_NEAREST_FILTERING, nearestFiltering)
            .putInt(KEY_ANTI_ALIASING_MODE, antiAliasingMode)
            .putBoolean(KEY_CRT_ENABLED, crtShaderEnabled)
            .putInt(KEY_RESOLUTION_SCALE, resolutionScale)
            .putInt(KEY_OUTPUT_RESOLUTION_PRESET, outputResolutionPreset)
            .apply();
    }

    // ------------------------------------------------- render options dialog

    private void showRenderOptionsDialog() {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle("Render Options");
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(12), dp(18), dp(4));
        scroll.addView(root);

        root.addView(makeSpinnerRow("Renderer",
                new String[]{"Vulkan", "OpenGL ES", "Software"},
                currentRenderer == RENDERER_VULKAN ? 0
                        : currentRenderer == RENDERER_OPENGL_ES ? 1 : 2,
                position -> {
                    int selectedRenderer = RENDERER_VULKAN;
                    if (position == 1) selectedRenderer = RENDERER_OPENGL_ES;
                    else if (position == 2) selectedRenderer = RENDERER_SOFTWARE;
                    if (currentRenderer == selectedRenderer) return;
                    currentRenderer = selectedRenderer;
                    if (!isHardwareRendererSelected()) {
                        resolutionScale = 1;
                        outputResolutionPreset = 0;
                        setResolutionScale(resolutionScale);
                        setOutputResolutionPreset(0);
                    }
                    setRendererType(currentRenderer);
                    rebindRendererSurface();
                    saveSettings();
                    updateButtonLabels();
                    updateStatusIndicator();
                }));

        root.addView(makeSpinnerRow("Aspect",
                new String[]{"4:3", "16:9"}, aspectRatio16by9 ? 1 : 0,
                position -> {
                    boolean newWide = (position == 1);
                    if (aspectRatio16by9 == newWide) return;
                    aspectRatio16by9 = newWide;
                    setAspectRatio(aspectRatio16by9);
                    saveSettings();
                    updateButtonLabels();
                    updateStatusIndicator();
                }));

        root.addView(makeSpinnerRow("Texture Filter",
                new String[]{"Linear (Smooth)", "Nearest (Sharp)"}, nearestFiltering ? 1 : 0,
                position -> toggleTextureFilter()));

        root.addView(makeSpinnerRow("Anti-Aliasing",
                new String[]{"Off", "Low (FXAA)", "High (FXAA+)"}, antiAliasingMode,
                position -> {
                    if (antiAliasingMode == position) return;
                    antiAliasingMode = position;
                    setAntiAliasingMode(antiAliasingMode);
                    saveSettings();
                    updateButtonLabels();
                    updateStatusIndicator();
                }));

        SwitchCompat crtSwitch = new SwitchCompat(this);
        crtSwitch.setText("CRT scanlines & curvature");
        crtSwitch.setChecked(crtShaderEnabled);
        crtSwitch.setOnCheckedChangeListener((buttonView, isChecked) -> toggleCrtShader());
        root.addView(crtSwitch, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        root.addView(makeSpinnerRow("Resolution",
                getResolutionScaleOptions(), getResolutionScaleIndex(resolutionScale),
                position -> {
                    int newScale = getResolutionScaleByIndex(position);
                    if (resolutionScale == newScale) return;
                    if (!isHardwareRendererSelected()) {
                        showStatusToast("Upscaling is only available on hardware renderers");
                        return;
                    }
                    resolutionScale = newScale;
                    outputResolutionPreset = 0;
                    setResolutionScale(resolutionScale);
                    setOutputResolutionPreset(0);
                    rebindRendererSurface();
                    saveSettings();
                    updateButtonLabels();
                    updateStatusIndicator();
                }));

        builder.setView(scroll);
        builder.setPositiveButton("Close", null);
        builder.show();
    }

    private String[] getResolutionScaleOptions() {
        return new String[] {
                "Auto (Based on Window Size)",
                "1x Native",
                "2x",
                "3x Native (for 720p)",
                "4x",
                "5x Native (for 1080p)",
                "6x Native (for 1440p)",
                "7x",
                "8x",
                "9x Native (for 4K)"
        };
    }

    private int getResolutionScaleIndex(int scale) {
        if (scale < 0) return 0;
        if (scale > MAX_RESOLUTION_SCALE) return MAX_RESOLUTION_SCALE;
        return scale;
    }

    private int getResolutionScaleByIndex(int index) {
        if (index < 0) return 0;
        if (index > MAX_RESOLUTION_SCALE) return MAX_RESOLUTION_SCALE;
        return index;
    }

    private boolean isHardwareRendererSelected() {
        return currentRenderer != RENDERER_SOFTWARE;
    }

    private void rebindRendererSurface() {
        if (emulatorSurfaceView == null) return;
        SurfaceHolder holder = emulatorSurfaceView.getHolder();
        if (holder == null) return;
        Surface surface = holder.getSurface();
        if (surface == null || !surface.isValid()) return;
        int width = emulatorSurfaceView.getWidth();
        int height = emulatorSurfaceView.getHeight();
        if (width > 0 && height > 0) {
            initRenderer(width, height);
        }
        setSurface(null);
        emulatorSurfaceView.post(() -> {
            Surface reboundSurface = holder.getSurface();
            if (reboundSurface != null && reboundSurface.isValid()) {
                setSurface(reboundSurface);
            }
        });
    }

    // ----------------------------------------------------- labels + status

    private String getRendererShortName(int rendererType) {
        if (rendererType == RENDERER_VULKAN) return "Vulkan";
        if (rendererType == RENDERER_OPENGL_ES) return "GL";
        if (rendererType == RENDERER_SOFTWARE) return "Soft";
        return "Vulkan";
    }

    private int normalizeRendererType(int rendererType) {
        if (rendererType == RENDERER_VULKAN || rendererType == RENDERER_OPENGL_ES
                || rendererType == RENDERER_SOFTWARE) {
            return rendererType;
        }
        return RENDERER_VULKAN;
    }

    private String getAaLabel(int mode) {
        if (mode == 1) return "AA-L";
        if (mode == 2) return "AA-H";
        return "AA-Off";
    }

    private String getResolutionPresetLabel(int presetHeight) {
        if (presetHeight == 720) return "720p";
        if (presetHeight == 1080) return "1080p";
        if (presetHeight == 1440) return "1440p";
        if (presetHeight == 2160) return "2160p";
        return "Auto";
    }

    private String getInternalScaleLabel(int scale) {
        if (scale <= 0) return "Auto";
        return scale + "x";
    }

    private String getEffectiveResolutionLabel() {
        if (!isHardwareRendererSelected()) return "1x (Software)";
        if (resolutionScale == 3) return "3x (720p)";
        if (resolutionScale == 5) return "5x (1080p)";
        if (resolutionScale == 6) return "6x (1440p)";
        if (resolutionScale == 9) return "9x (4K)";
        if (resolutionScale <= 0) return "Auto";
        return resolutionScale + "x";
    }

    private void updateStatusIndicator() {
        if (statusIndicator == null) return;
        statusIndicator.setVisibility(View.GONE);
        statusIndicator.setText("");
    }

    private void updateButtonLabels() {
        if (topRendererButton != null) topRendererButton.setText(getRendererShortName(currentRenderer));
        if (topAspectButton != null) topAspectButton.setText(aspectRatio16by9 ? "16:9" : "4:3");
        if (topCrtButton != null) topCrtButton.setText(crtShaderEnabled ? "CRT" : "No CRT");
        if (topBezelButton != null) topBezelButton.setText(bezelEnabled ? "Bezel" : "No Bezel");
        if (topPadButton != null) topPadButton.setText(virtualPadVisible ? "Pad" : "No Pad");
        if (sideStatusText != null) {
            String game = mGamePath == null || mGamePath.isEmpty() ? "BIOS" : new File(mGamePath).getName();
            String currentBezel = mGamePath == null || mGamePath.isEmpty()
                    ? BezelResolver.describeDownloadedBezels(this)
                    : BezelResolver.describeBezel(this, mGamePath);
            sideStatusText.setText("Running: " + game
                    + "\n" + getRendererShortName(currentRenderer) + " | "
                    + (aspectRatio16by9 ? "16:9" : "4:3") + " | "
                    + (nearestFiltering ? "Nearest" : "Linear") + " | "
                    + (crtShaderEnabled ? "CRT" : "No CRT")
                    + "\n" + currentBezel
                    + "\nVirtual pad: " + (virtualPadVisible ? "shown" : "hidden"));
        }
    }

    private void applyDebugModeUi() {
        if (fpsCounter != null) fpsCounter.setVisibility(View.VISIBLE);
        if (statusIndicator != null) {
            statusIndicator.setVisibility(View.GONE);
            statusIndicator.setText("");
        }
    }

    private void showStatusToast(String message) {
        Toast toast = Toast.makeText(this, message, Toast.LENGTH_SHORT);
        View toastView = toast.getView();
        if (toastView != null) {
            TextView messageView = toastView.findViewById(android.R.id.message);
            if (messageView != null) {
                messageView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
            }
        }
        toast.show();
    }

    // --------------------------------------------------- FPS counter

    private void startFpsCounter() {
        fpsRunnable = new Runnable() {
            @Override
            public void run() {
                long now = System.currentTimeMillis();
                if (lastFpsTime > 0) {
                    long elapsed = now - lastFpsTime;
                    if (elapsed > 0) {
                        int renderedFrames = consumeRenderedFrames();
                        int fps = (int) (renderedFrames * 1000L / elapsed);
                        fpsCounter.setText("FPS " + fps);
                    }
                }
                lastFpsTime = now;
                fpsHandler.postDelayed(this, 1000);
            }
        };
        fpsHandler.post(fpsRunnable);
    }

    private void stopFpsCounter() {
        if (fpsRunnable != null) fpsHandler.removeCallbacks(fpsRunnable);
    }

    // Called from native code to count frames.
    private void countFrame() {
        frameCount++;
    }

    /** Rotation is always 0 — see getEffectiveRotation() below. */
    private int getEffectiveRotation() {
        return 0;
    }

    // ------------------------------------------------------ library ops

    private void openGameLibrary() {
        String libraryPath = EmulatorPathStore.getSavedLibraryFolder(this);
        if (!EmulatorPathStore.isValidDirectoryPath(libraryPath)) {
            toast("Set a game library folder first");
            return;
        }
        if (settingsPanel != null) settingsPanel.setVisibility(View.GONE);
        if (mapperOverlay != null) mapperOverlay.setVisibility(View.GONE);
        pauseForOverlay();
        gameLibraryScreen.setVisibility(View.VISIBLE);
        gameLibraryScreen.bringToFront();
        if (gameLibrarySearch != null) gameLibrarySearch.setText("");
        refreshGameLibraryScreen();
    }

    private void closeGameLibraryScreen() {
        if (gameLibraryScreen == null) return;
        gameLibraryScreen.setVisibility(View.GONE);
        resumeAfterOverlayIfIdle();
    }

    private void refreshGameLibraryScreen() {
        if (gameLibraryStatus != null) gameLibraryStatus.setText("Scanning 3DO library...");
        currentGameLibrary.clear();
        renderGameLibraryCards();
        new Thread(() -> {
            List<GameLibraryEntry> games = scanGameLibrary();
            runOnUiThread(() -> {
                currentGameLibrary.clear();
                currentGameLibrary.addAll(games);
                renderGameLibraryCards();
                beginIgdbMatching();
            });
        }, "3do-game-library-scan").start();
    }

    private List<GameLibraryEntry> scanGameLibrary() {
        List<GameLibraryEntry> games = new ArrayList<>();
        String libraryPath = EmulatorPathStore.getSavedLibraryFolder(this);
        File root = new File(libraryPath);
        if (!root.isDirectory()) return games;
        scanGameDirectory(root, games, 0);
        games.sort(Comparator.comparing(entry -> entry.displayName.toLowerCase(Locale.US)));
        return games;
    }

    /** Enumerate a SAF tree in place. Nothing is copied. */
    private List<GameLibraryEntry> scanGameTree(Uri treeUri) {
        List<GameLibraryEntry> games = new ArrayList<>();
        DocumentFile root = DocumentFile.fromTreeUri(this, treeUri);
        if (root == null || !root.isDirectory()) return games;
        scanGameTreeDirectory(root, games, 0);
        games.sort(Comparator.comparing(entry -> entry.displayName.toLowerCase(Locale.US)));
        return games;
    }

    private void scanGameTreeDirectory(DocumentFile dir, List<GameLibraryEntry> games, int depth) {
        if (dir == null || depth > 6 || games.size() >= 1000) return;
        for (DocumentFile child : dir.listFiles()) {
            if (games.size() >= 1000) return;
            String name = child.getName();
            if (name == null) continue;
            if (child.isDirectory()) {
                if (!name.startsWith(".")) scanGameTreeDirectory(child, games, depth + 1);
            } else if (isSupportedLibraryName(name)) {
                games.add(new GameLibraryEntry(name, null, child.getUri()));
            }
        }
    }

    private boolean isSupportedLibraryName(String name) {
        String lower = name.toLowerCase(Locale.US);
        // .bin is deliberately excluded here: in a tree we cannot cheaply tell a
        // raw disc image from the data track a .cue already refers to, and
        // listing both would show every game twice.
        return lower.endsWith(".cue") || lower.endsWith(".iso") || lower.endsWith(".chd");
    }

    private void scanGameDirectory(File directory, List<GameLibraryEntry> games, int depth) {
        if (directory == null || depth > 6 || games.size() >= 1000) return;
        File[] files = directory.listFiles();
        if (files == null) return;
        for (File file : files) {
            if (games.size() >= 1000) return;
            if (file.isDirectory() && !file.getName().startsWith(".")) {
                scanGameDirectory(file, games, depth + 1);
            } else if (isSupportedLibraryGame(file)) {
                games.add(new GameLibraryEntry(file.getName(), file));
            }
        }
    }

    private boolean isSupportedLibraryGame(File file) {
        if (file == null || !file.isFile()) return false;
        String name = file.getName().toLowerCase(Locale.US);
        return name.endsWith(".cue") || name.endsWith(".iso") || name.endsWith(".chd")
                || (name.endsWith(".bin") && !hasCueForBin(file));
    }

    private boolean hasCueForBin(File binFile) {
        File parent = binFile.getParentFile();
        if (parent == null || !parent.isDirectory()) return false;
        String binName = binFile.getName();
        int dot = binName.lastIndexOf('.');
        String baseName = dot > 0 ? binName.substring(0, dot) : binName;
        File sameBaseCue = new File(parent, baseName + ".cue");
        if (sameBaseCue.isFile()) return true;
        File[] cueFiles = parent.listFiles(file -> file.isFile()
                && file.getName().toLowerCase(Locale.ROOT).endsWith(".cue"));
        if (cueFiles == null) return false;
        for (File cueFile : cueFiles) {
            if (cueReferencesFile(cueFile, binName)) return true;
        }
        return false;
    }

    private boolean cueReferencesFile(File cueFile, String fileName) {
        try (BufferedReader reader = new BufferedReader(new FileReader(cueFile))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String trimmed = line.trim();
                if (!trimmed.regionMatches(true, 0, "FILE", 0, 4)) continue;
                String referencedName = parseCueFileReference(trimmed);
                if (referencedName != null
                        && new File(referencedName).getName().equalsIgnoreCase(fileName)) {
                    return true;
                }
            }
        } catch (IOException e) {
            Log.w("3DO-Main", "Failed reading cue file for BIN pairing", e);
        }
        return false;
    }

    private String parseCueFileReference(String trimmedLine) {
        if (trimmedLine == null || !trimmedLine.regionMatches(true, 0, "FILE", 0, 4)) return null;
        String rest = trimmedLine.substring(4).trim();
        if (rest.isEmpty()) return null;
        if (rest.charAt(0) == '"') {
            int secondQuote = rest.indexOf('"', 1);
            if (secondQuote <= 1) return null;
            return rest.substring(1, secondQuote);
        }
        int lastSpace = rest.lastIndexOf(' ');
        return lastSpace > 0 ? rest.substring(0, lastSpace).trim() : rest;
    }

    private void renderGameLibraryCards() {
        if (gameLibraryGrid == null || gameLibraryCarousel == null || gameLibraryStatus == null) return;
        gameLibraryGrid.removeAllViews();
        gameLibraryCarousel.removeAllViews();
        if (gameLibraryCarouselScroll != null) {
            gameLibraryCarouselScroll.setVisibility(gameLibraryCarouselMode ? View.VISIBLE : View.GONE);
        }
        gameLibraryGrid.setVisibility(gameLibraryCarouselMode ? View.GONE : View.VISIBLE);
        if (gameLibraryViewButton != null) {
            gameLibraryViewButton.setText(gameLibraryCarouselMode ? "Grid" : "Carousel");
        }
        String query = gameLibrarySearch == null ? "" : gameLibrarySearch.getText().toString();
        int shown = 0;
        for (GameLibraryEntry game : currentGameLibrary) {
            if (!matchesSearch(displayBaseName(game.displayName), query)) continue;
            if (gameLibraryCarouselMode) {
                gameLibraryCarousel.addView(createGameCard(game, true));
            } else {
                gameLibraryGrid.addView(createGameCard(game, false));
            }
            shown++;
        }
        if (currentGameLibrary.isEmpty()) {
            gameLibraryStatus.setText("No 3DO disc images found. | " + BezelResolver.describeDownloadedBezels(this));
        } else if (shown == currentGameLibrary.size()) {
            gameLibraryStatus.setText(shown + " 3DO games | " + BezelResolver.describeDownloadedBezels(this));
        } else {
            gameLibraryStatus.setText(shown + " of " + currentGameLibrary.size() + " 3DO games | "
                    + BezelResolver.describeDownloadedBezels(this));
        }
    }

    private void toggleGameLibraryView() {
        gameLibraryCarouselMode = !gameLibraryCarouselMode;
        renderGameLibraryCards();
    }

    private View createGameCard(GameLibraryEntry entry, boolean carousel) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setGravity(Gravity.CENTER_HORIZONTAL);
        card.setPadding(dp(8), dp(8), dp(8), dp(8));
        card.setBackground(cardBackground(0xFF191D22, 0xFF353B44));
        card.setOnClickListener(v -> showGameDetails(entry));
        card.setOnLongClickListener(v -> { showGameDetails(entry); return true; });
        card.setLongClickable(true);

        FrameLayout coverSlot = new FrameLayout(this);
        coverSlot.setBackground(cardBackground(0xFF262C34, 0xFF404853));
        if (entry.coverBitmap != null) {
            ImageView cover = new ImageView(this);
            cover.setImageBitmap(entry.coverBitmap);
            cover.setScaleType(ImageView.ScaleType.FIT_CENTER);
            cover.setAdjustViewBounds(false);
            coverSlot.addView(cover, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT));
        } else {
            TextView coverLabel = new TextView(this);
            coverLabel.setText(entry.igdbLookupStarted ? "IGDB" : "3DO");
            coverLabel.setTextColor(0xFFB9C2CE);
            coverLabel.setTextSize(12.0f);
            coverLabel.setGravity(Gravity.CENTER);
            coverSlot.addView(coverLabel, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.MATCH_PARENT));
        }
        card.addView(coverSlot, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                carousel ? dp(228) : dp(184)));

        TextView title = new TextView(this);
        title.setText(entry.igdbGame != null && entry.igdbGame.name != null && !entry.igdbGame.name.isEmpty()
                ? entry.igdbGame.name
                : displayBaseName(entry.displayName));
        title.setTextColor(0xFFFFFFFF);
        title.setTextSize(12.0f);
        title.setGravity(Gravity.CENTER);
        title.setMaxLines(2);
        title.setPadding(0, dp(4), 0, 0);
        card.addView(title, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(32)));

        TextView detail = new TextView(this);
        detail.setText(gameCardDetail(entry));
        detail.setTextColor(0xFF9AA3AF);
        detail.setTextSize(10.0f);
        detail.setGravity(Gravity.CENTER);
        detail.setMaxLines(1);
        card.addView(detail, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(16)));

        if (carousel) {
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(dp(192), dp(300));
            params.setMargins(dp(8), dp(8), dp(8), dp(8));
            card.setLayoutParams(params);
        } else {
            GridLayout.LayoutParams params = new GridLayout.LayoutParams();
            params.width = dp(168);
            params.height = dp(256);
            params.setMargins(dp(6), dp(6), dp(6), dp(6));
            card.setLayoutParams(params);
        }
        return card;
    }

    private String gameCardDetail(GameLibraryEntry entry) {
        String bezelLabel = BezelResolver.findBezelForGame(this, entry.file.getAbsolutePath()) == null ? "" : "Bezel";
        if (entry.igdbGame == null) {
            String format = fileExtensionLabel(entry.displayName);
            return bezelLabel.isEmpty() ? format : format + " | " + bezelLabel;
        }
        StringBuilder detail = new StringBuilder();
        if (entry.igdbGame.releaseDate != null && !entry.igdbGame.releaseDate.isEmpty()) {
            detail.append(entry.igdbGame.releaseDate);
        }
        if (entry.igdbGame.publisher != null && !entry.igdbGame.publisher.isEmpty()) {
            if (detail.length() > 0) detail.append(" | ");
            detail.append(entry.igdbGame.publisher);
        }
        if (!bezelLabel.isEmpty()) {
            if (detail.length() > 0) detail.append(" | ");
            detail.append(bezelLabel);
        }
        return detail.length() == 0 ? fileExtensionLabel(entry.displayName) : detail.toString();
    }

    private void loadGameFromLibrary(GameLibraryEntry entry) {
        if (entry == null) return;
        String path = resolveGamePath(entry);
        if (path == null) {
            toast("Game file not found");
            return;
        }
        mGamePath = path;
        EmulatorPathStore.saveLastGamePath(this, mGamePath);
        currentBezelPath = "";
        applyBezelVisibility();
        if (gameLibraryScreen != null) gameLibraryScreen.setVisibility(View.GONE);
        if (settingsPanel != null) settingsPanel.setVisibility(View.GONE);
        if (mapperOverlay != null) mapperOverlay.setVisibility(View.GONE);
        overlayPaused = false;
        performHardReset(mGamePath);
    }

    /**
     * The path to hand the emulator core.
     *
     * A game in a SAF tree has no filesystem path, so it is opened here and
     * passed as /proc/self/fd/N - the descriptor names the file, and the core
     * opens it with an ordinary ifstream exactly as it would any other path.
     * The descriptor has to outlive the load, so it is held until the next one
     * replaces it.
     */
    private String resolveGamePath(GameLibraryEntry entry) {
        if (entry.file != null && entry.file.isFile()) {
            closeGameDescriptor();
            return entry.file.getAbsolutePath();
        }
        if (entry.uri == null) return null;
        try {
            ParcelFileDescriptor pfd =
                    getContentResolver().openFileDescriptor(entry.uri, "r");
            if (pfd == null) return null;
            closeGameDescriptor();
            mGameDescriptor = pfd;
            return "/proc/self/fd/" + pfd.getFd();
        } catch (Exception e) {
            Log.w("3DO-Main", "Could not open game: " + entry.displayName, e);
            return null;
        }
    }

    private void closeGameDescriptor() {
        if (mGameDescriptor == null) return;
        try { mGameDescriptor.close(); } catch (IOException ignored) { }
        mGameDescriptor = null;
    }

    private void beginIgdbMatching() {
        if (currentGameLibrary.isEmpty()) return;
        if (igdbService == null) igdbService = IgdbService.getInstance(this);
        igdbNextIndex = 0;
        igdbInFlight = 0;
        pumpIgdbQueue();
    }

    private void pumpIgdbQueue() {
        while (igdbInFlight < MAX_CONCURRENT_IGDB_REQUESTS
                && igdbNextIndex < currentGameLibrary.size()
                && igdbNextIndex < MAX_IGDB_LOOKUPS_PER_SCAN) {
            GameLibraryEntry entry = currentGameLibrary.get(igdbNextIndex++);
            if (entry.igdbLookupStarted) continue;
            entry.igdbLookupStarted = true;
            igdbInFlight++;
            searchGameOnIgdb(entry);
        }
    }

    private void searchGameOnIgdb(GameLibraryEntry entry) {
        String queryName = buildIgdbQueryName(entry.displayName);
        igdbService.lookupGame(queryName, game -> {
            entry.igdbGame = game;
            if (game != null && game.coverUrl != null && !game.coverUrl.isEmpty()) {
                igdbService.loadCover(game.coverUrl, game.id, (cover, localPath) -> {
                    entry.coverBitmap = cover;
                    igdbInFlight--;
                    renderGameLibraryCards();
                    pumpIgdbQueue();
                });
            } else {
                igdbInFlight--;
                renderGameLibraryCards();
                pumpIgdbQueue();
            }
        });
    }

    private void showGameDetails(GameLibraryEntry entry) {
        if (igdbService == null) igdbService = IgdbService.getInstance(this);

        LinearLayout container = new LinearLayout(this);
        container.setOrientation(LinearLayout.HORIZONTAL);
        int padding = dp(12);
        container.setPadding(padding, padding, padding, padding);
        container.setBackgroundColor(0xF8101418);

        LinearLayout leftColumn = new LinearLayout(this);
        leftColumn.setOrientation(LinearLayout.VERTICAL);
        leftColumn.setPadding(0, 0, dp(10), 0);
        ScrollView leftScroll = new ScrollView(this);
        leftScroll.setFillViewport(false);
        leftScroll.setScrollbarFadingEnabled(false);
        leftScroll.addView(leftColumn, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT,
                ScrollView.LayoutParams.WRAP_CONTENT));

        LinearLayout rightColumn = new LinearLayout(this);
        rightColumn.setOrientation(LinearLayout.VERTICAL);
        rightColumn.setPadding(dp(10), 0, 0, 0);

        LinearLayout actionRow = new LinearLayout(this);
        actionRow.setOrientation(LinearLayout.HORIZONTAL);
        actionRow.setGravity(Gravity.CENTER_VERTICAL);
        actionRow.setPadding(0, 0, 0, dp(8));
        Button loadButton = makeCompactButton("Load", null);
        actionRow.addView(loadButton, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));
        Button searchMissingButton = null;
        if (entry.igdbGame == null) {
            searchMissingButton = makeCompactButton("Search IGDB", null);
            LinearLayout.LayoutParams searchMissingParams = new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
            searchMissingParams.setMargins(dp(8), 0, 0, 0);
            actionRow.addView(searchMissingButton, searchMissingParams);
        }
        Button matchBezelButton = null;
        if (BezelResolver.findBezelForGame(this, entry.file.getAbsolutePath()) == null) {
            matchBezelButton = makeCompactButton("Match Bezel", null);
            LinearLayout.LayoutParams matchParams = new LinearLayout.LayoutParams(
                    0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
            matchParams.setMargins(dp(8), 0, 0, 0);
            actionRow.addView(matchBezelButton, matchParams);
        }
        leftColumn.addView(actionRow, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        TextView bezelStatus = new TextView(this);
        bezelStatus.setText(BezelResolver.describeBezelDiagnostic(this, entry.file.getAbsolutePath()));
        bezelStatus.setTextColor(0xFFBAC2CC);
        bezelStatus.setTextSize(11.0f);
        bezelStatus.setPadding(0, 0, 0, dp(8));
        leftColumn.addView(bezelStatus, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        TextView details = new TextView(this);
        details.setText(formatIgdbDetails(entry));
        details.setTextColor(0xFFE8EAED);
        details.setTextSize(13.0f);
        details.setPadding(0, 0, 0, dp(10));
        leftColumn.addView(details, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        EditText search = new EditText(this);
        search.setSingleLine(true);
        search.setHint("Manual IGDB search");
        search.setText(buildIgdbQueryName(entry.displayName));
        search.setTextColor(0xFFFFFFFF);
        search.setHintTextColor(0xFF8C939D);
        search.setSelectAllOnFocus(true);
        leftColumn.addView(search, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        Button searchButton = makeCompactButton("Search 3DO IGDB", null);
        leftColumn.addView(searchButton, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        ListView resultsList = new ListView(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(
                this, android.R.layout.simple_list_item_1, new ArrayList<>()) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                View row = super.getView(position, convertView, parent);
                TextView text = row.findViewById(android.R.id.text1);
                if (text != null) {
                    text.setTextColor(0xFFE8EAED);
                    text.setTextSize(13.0f);
                }
                row.setBackgroundColor(0xFF101418);
                return row;
            }
        };
        List<IgdbService.IgdbGame> results = new ArrayList<>();
        resultsList.setAdapter(adapter);
        resultsList.setBackgroundColor(0xFF101418);
        resultsList.setCacheColorHint(0xFF101418);
        leftColumn.addView(resultsList, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(180)));

        TextView mediaTitle = new TextView(this);
        mediaTitle.setText("Gameplay Images");
        mediaTitle.setTextColor(0xFFFFFFFF);
        mediaTitle.setTextSize(13.0f);
        mediaTitle.setPadding(0, 0, 0, dp(6));
        rightColumn.addView(mediaTitle);

        ScrollView mediaScroll = new ScrollView(this);
        mediaScroll.setFillViewport(false);
        LinearLayout mediaStrip = new LinearLayout(this);
        mediaStrip.setOrientation(LinearLayout.VERTICAL);
        mediaScroll.addView(mediaStrip, new ScrollView.LayoutParams(
                ScrollView.LayoutParams.MATCH_PARENT,
                ScrollView.LayoutParams.WRAP_CONTENT));
        rightColumn.addView(mediaScroll, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));
        populateIgdbMediaStrip(entry, mediaStrip);

        TextView mediaHint = new TextView(this);
        mediaHint.setText("Use IGDB search to refresh missing images");
        mediaHint.setTextColor(0xFFBAC2CC);
        mediaHint.setTextSize(11.0f);
        mediaHint.setPadding(0, dp(8), 0, 0);
        rightColumn.addView(mediaHint);

        container.addView(leftScroll, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.MATCH_PARENT, 1.05f));
        container.addView(rightColumn, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.MATCH_PARENT, 0.95f));

        AlertDialog dialog = new AlertDialog.Builder(this)
                .setView(wrapGameDetailsWithBezel(entry, container))
                .setNegativeButton("Close", null)
                .create();

        loadButton.setOnClickListener(v -> {
            dialog.dismiss();
            loadGameFromLibrary(entry);
        });
        searchButton.setOnClickListener(v -> {
            if (!igdbService.hasCredentials()) {
                adapter.clear();
                adapter.add("IGDB credentials missing");
                adapter.notifyDataSetChanged();
                return;
            }
            String query = search.getText().toString().trim();
            adapter.clear();
            adapter.add("Searching 3DO IGDB...");
            adapter.notifyDataSetChanged();
            igdbService.searchGames(query, games -> {
                results.clear();
                adapter.clear();
                if (games.isEmpty()) {
                    adapter.add("No 3DO IGDB matches");
                } else {
                    results.addAll(games);
                    for (IgdbService.IgdbGame game : games) {
                        adapter.add(igdbResultLabel(game));
                    }
                }
                adapter.notifyDataSetChanged();
            });
        });
        Button finalSearchMissingButton = searchMissingButton;
        if (finalSearchMissingButton != null) {
            finalSearchMissingButton.setOnClickListener(v -> searchButton.performClick());
        }
        Button finalMatchBezelButton = matchBezelButton;
        if (finalMatchBezelButton != null) {
            finalMatchBezelButton.setOnClickListener(v -> {
                dialog.dismiss();
                showBezelMatchDialog(entry);
            });
        }
        resultsList.setOnItemClickListener((parent, view, position, id) -> {
            if (position < 0 || position >= results.size()) return;
            IgdbService.IgdbGame game = results.get(position);
            entry.igdbGame = game;
            entry.igdbLookupStarted = true;
            igdbService.cacheGame(buildIgdbQueryName(entry.displayName), game);
            if (game.coverUrl != null && !game.coverUrl.isEmpty()) {
                igdbService.loadCover(game.coverUrl, game.id, (cover, localPath) -> {
                    entry.coverBitmap = cover;
                    renderGameLibraryCards();
                    populateIgdbMediaStrip(entry, mediaStrip);
                });
            }
            dialog.dismiss();
        });
        dialog.setOnShowListener(d -> {
            search.clearFocus();
            if (dialog.getWindow() != null) {
                dialog.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
                dialog.getWindow().setSoftInputMode(
                        WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN
                                | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
                dialog.getWindow().setLayout(
                        Math.min(getResources().getDisplayMetrics().widthPixels - dp(24), dp(980)),
                        Math.min(getResources().getDisplayMetrics().heightPixels - dp(24), dp(640)));
            }
            Button closeButton = dialog.getButton(AlertDialog.BUTTON_NEGATIVE);
            if (closeButton != null) closeButton.setTextColor(0xFFFFFFFF);
        });
        dialog.show();
    }

    private void showBezelMatchDialog(GameLibraryEntry entry) {
        List<File> allBezels = BezelResolver.listDownloadedBezels(this);
        if (allBezels.isEmpty()) {
            toast("No downloaded bezels found");
            showGameDetails(entry);
            return;
        }
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(12), dp(8), dp(12), dp(8));
        panel.setBackgroundColor(0xFF101418);
        TextView target = new TextView(this);
        target.setText("Game key: " + BezelResolver.expectedKeyForGame(entry.file.getAbsolutePath()));
        target.setTextColor(0xFFBAC2CC);
        target.setTextSize(12.0f);
        target.setPadding(0, 0, 0, dp(8));
        panel.addView(target);
        EditText filter = new EditText(this);
        filter.setSingleLine(true);
        filter.setHint("Filter downloaded bezels");
        filter.setText(buildIgdbQueryName(entry.displayName));
        filter.setSelectAllOnFocus(true);
        filter.setTextColor(0xFFFFFFFF);
        filter.setHintTextColor(0xFF8C939D);
        panel.addView(filter, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        ListView list = new ListView(this);
        list.setBackgroundColor(0xFF101418);
        list.setCacheColorHint(0xFF101418);
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(
                this, android.R.layout.simple_list_item_1, new ArrayList<>()) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                View row = super.getView(position, convertView, parent);
                TextView text = row.findViewById(android.R.id.text1);
                if (text != null) {
                    text.setTextColor(0xFFE8EAED);
                    text.setTextSize(13.0f);
                }
                row.setBackgroundColor(0xFF101418);
                return row;
            }
        };
        List<File> filtered = new ArrayList<>();
        list.setAdapter(adapter);
        panel.addView(list, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));
        renderBezelMatchCandidates(allBezels, filtered, adapter, filter.getText().toString());
        filter.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {
                renderBezelMatchCandidates(allBezels, filtered, adapter, s == null ? "" : s.toString());
            }
            @Override public void afterTextChanged(Editable s) {}
        });
        AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Match Bezel")
                .setView(panel)
                .setNegativeButton("Cancel", null)
                .create();
        list.setOnItemClickListener((parent, view, position, id) -> {
            if (position < 0 || position >= filtered.size()) return;
            File selected = filtered.get(position);
            BezelResolver.saveManualBezelMatch(this, entry.file.getAbsolutePath(), selected);
            toast("Matched bezel: " + selected.getName());
            currentBezelPath = "";
            applyBezelVisibility();
            renderGameLibraryCards();
            dialog.dismiss();
            showGameDetails(entry);
        });
        dialog.setOnShowListener(d -> {
            filter.clearFocus();
            if (dialog.getWindow() != null) {
                dialog.getWindow().setBackgroundDrawable(new ColorDrawable(0xFF101418));
                dialog.getWindow().setSoftInputMode(
                        WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN
                                | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING);
                dialog.getWindow().setLayout(
                        Math.min(getResources().getDisplayMetrics().widthPixels - dp(36), dp(820)),
                        Math.min(getResources().getDisplayMetrics().heightPixels - dp(36), dp(620)));
            }
            Button cancel = dialog.getButton(AlertDialog.BUTTON_NEGATIVE);
            if (cancel != null) cancel.setTextColor(0xFFFFFFFF);
        });
        dialog.setOnDismissListener(d -> { if (filter.hasFocus()) filter.clearFocus(); });
        dialog.show();
    }

    private void renderBezelMatchCandidates(List<File> allBezels, List<File> filtered,
                                            ArrayAdapter<String> adapter, String query) {
        String needle = query == null ? "" : query.toLowerCase(Locale.ROOT).trim();
        filtered.clear();
        adapter.clear();
        for (File bezel : allBezels) {
            String name = bezel.getName();
            if (needle.isEmpty() || name.toLowerCase(Locale.ROOT).contains(needle)) {
                filtered.add(bezel);
                adapter.add(displayBaseName(name));
            }
            if (filtered.size() >= 200) break;
        }
        if (filtered.isEmpty()) adapter.add("No matching downloaded bezels");
        adapter.notifyDataSetChanged();
    }

    private View wrapGameDetailsWithBezel(GameLibraryEntry entry, View content) {
        File bezelFile = BezelResolver.findBezelForGame(this, entry.file.getAbsolutePath());
        FrameLayout frame = new FrameLayout(this);
        frame.setBackgroundColor(0xFF050607);
        FrameLayout.LayoutParams contentParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT);
        if (bezelFile != null && bezelFile.isFile()) {
            Log.d("3DO-Main", "Details bezel matched "
                    + entry.file.getName() + " -> " + bezelFile.getAbsolutePath());
            ImageView bezel = new ImageView(this);
            bezel.setImageURI(Uri.fromFile(bezelFile));
            bezel.setScaleType(ImageView.ScaleType.FIT_XY);
            frame.addView(bezel, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
            contentParams.setMargins(dp(92), dp(18), dp(92), dp(18));
            content.setBackgroundColor(0xEA101418);
        } else {
            Log.d("3DO-Main", "Details bezel missing for "
                    + entry.file.getName() + " | " + BezelResolver.describeBezelDiagnostic(this, entry.file.getAbsolutePath()));
            TextView noMatch = new TextView(this);
            noMatch.setText(BezelResolver.describeBezelDiagnostic(this, entry.file.getAbsolutePath()));
            noMatch.setTextColor(0xFFFFD166);
            noMatch.setTextSize(12.0f);
            noMatch.setGravity(Gravity.CENTER);
            noMatch.setPadding(dp(10), dp(6), dp(10), dp(6));
            noMatch.setBackgroundColor(0xCC1B2028);
            FrameLayout.LayoutParams noMatchParams = new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP);
            frame.addView(noMatch, noMatchParams);
            contentParams.setMargins(dp(10), dp(38), dp(10), dp(10));
            content.setBackgroundColor(0xF0101418);
        }
        frame.addView(content, contentParams);
        frame.setMinimumHeight(dp(560));
        return frame;
    }

    private void populateIgdbMediaStrip(GameLibraryEntry entry, LinearLayout mediaStrip) {
        mediaStrip.removeAllViews();
        if (entry.igdbGame == null) {
            addMediaLabel(mediaStrip, "No IGDB media");
            return;
        }
        List<String> urls = new ArrayList<>();
        urls.addAll(entry.igdbGame.screenshots);
        urls.addAll(entry.igdbGame.artworks);
        if (entry.igdbGame.coverUrl != null && !entry.igdbGame.coverUrl.isEmpty()) {
            urls.add(entry.igdbGame.coverUrl);
        }
        if (urls.isEmpty()) {
            addMediaLabel(mediaStrip, "No IGDB media");
            return;
        }
        int max = Math.min(urls.size(), 7);
        for (int i = 0; i < max; i++) {
            String url = urls.get(i);
            ImageView image = new ImageView(this);
            image.setScaleType(ImageView.ScaleType.FIT_CENTER);
            image.setBackground(cardBackground(0xFF20262D, 0xFF3A424C));
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, dp(132));
            params.setMargins(0, 0, 0, dp(8));
            mediaStrip.addView(image, params);
            final int index = i;
            if (url.equals(entry.igdbGame.coverUrl)) {
                igdbService.loadCover(url, entry.igdbGame.id, (bitmap, localPath) -> {
                    if (bitmap != null) {
                        image.setImageBitmap(bitmap);
                        entry.coverBitmap = bitmap;
                        renderGameLibraryCards();
                    }
                });
            } else {
                igdbService.loadMediaImage(url, entry.igdbGame.id + "_" + index, bitmap -> {
                    if (bitmap != null) image.setImageBitmap(bitmap);
                });
            }
        }
    }

    private void addMediaLabel(LinearLayout mediaStrip, String label) {
        TextView empty = new TextView(this);
        empty.setText(label);
        empty.setTextColor(0xFF9AA3AF);
        empty.setGravity(Gravity.CENTER);
        empty.setBackground(cardBackground(0xFF20262D, 0xFF3A424C));
        mediaStrip.addView(empty, new LinearLayout.LayoutParams(dp(180), dp(108)));
    }

    private String formatIgdbDetails(GameLibraryEntry entry) {
        StringBuilder details = new StringBuilder();
        details.append(displayBaseName(entry.displayName));
        details.append("\n").append(BezelResolver.describeBezelDiagnostic(this, entry.file.getAbsolutePath()));
        if (entry.igdbGame == null) {
            details.append("\nIGDB: no match yet");
            return details.toString();
        }
        IgdbService.IgdbGame game = entry.igdbGame;
        details.append("\nIGDB: ").append(emptyFallback(game.name, "Unknown"));
        if (game.releaseDate != null && !game.releaseDate.isEmpty()) {
            details.append("\nYear: ").append(game.releaseDate);
        }
        if (game.publisher != null && !game.publisher.isEmpty()) {
            details.append("\nPublisher: ").append(game.publisher);
        }
        if (game.summary != null && !game.summary.isEmpty()) {
            details.append("\n\n").append(game.summary);
        }
        return details.toString();
    }

    private String igdbResultLabel(IgdbService.IgdbGame game) {
        StringBuilder label = new StringBuilder(emptyFallback(game.name, "Unknown"));
        if (game.releaseDate != null && !game.releaseDate.isEmpty()) {
            label.append(" (").append(game.releaseDate).append(")");
        }
        if (game.publisher != null && !game.publisher.isEmpty()) {
            label.append(" - ").append(game.publisher);
        }
        return label.toString();
    }

    private String emptyFallback(String value, String fallback) {
        return value == null || value.isEmpty() ? fallback : value;
    }

    private boolean matchesSearch(String value, String query) {
        if (query == null || query.trim().isEmpty()) return true;
        return value != null && value.toLowerCase(Locale.US).contains(query.trim().toLowerCase(Locale.US));
    }

    private String displayBaseName(String name) {
        if (name == null) return "";
        String value = new File(name).getName();
        int dot = value.lastIndexOf('.');
        return dot > 0 ? value.substring(0, dot) : value;
    }

    private String buildIgdbQueryName(String rawName) {
        String value = displayBaseName(rawName).trim();
        int cut = value.length();
        int paren = value.indexOf('(');
        int bracket = value.indexOf('[');
        int brace = value.indexOf('{');
        if (paren >= 0) cut = Math.min(cut, paren);
        if (bracket >= 0) cut = Math.min(cut, bracket);
        if (brace >= 0) cut = Math.min(cut, brace);
        value = value.substring(0, cut).replace('_', ' ').trim();
        value = value.replaceAll("(?i)\\bv\\d+(?:\\.\\d+)*\\b", "").trim();
        return value.isEmpty() ? displayBaseName(rawName) : value;
    }

    private String fileExtensionLabel(String name) {
        int dot = name == null ? -1 : name.lastIndexOf('.');
        if (dot < 0 || dot == name.length() - 1) return "3DO";
        return name.substring(dot + 1).toUpperCase(Locale.US);
    }

    // -------------------------------------------------- bezel download

    private void showDownloadBezelsDialog() {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(24), dp(12), dp(24), 0);
        layout.setBackgroundColor(0xFF101418);
        TextView hint = new TextView(this);
        hint.setText("Download per-game 3DO bezel PNGs into app storage.");
        hint.setTextColor(0xFFE8EAED);
        hint.setTextSize(13.0f);
        layout.addView(hint);
        EditText input = new EditText(this);
        input.setSingleLine(false);
        input.setMinLines(2);
        input.setTextColor(0xFFFFFFFF);
        input.setHintTextColor(0xFF8C939D);
        input.setText(prefs.getString(KEY_BEZEL_GITHUB_URL, BezelDownloader.DEFAULT_GITHUB_ZIP_URL));
        layout.addView(input, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        new AlertDialog.Builder(this)
                .setTitle("Download Bezels")
                .setView(layout)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Download", (dialog, which) -> {
                    String url = input.getText().toString().trim();
                    if (url.isEmpty()) {
                        toast("GitHub URL is required");
                        return;
                    }
                    prefs.edit().putString(KEY_BEZEL_GITHUB_URL, url).apply();
                    downloadBezelsFromGithub(url);
                })
                .show();
    }

    private void downloadBezelsFromGithub(String url) {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(24), dp(18), dp(24), dp(8));
        layout.setBackgroundColor(0xFF101418);
        TextView status = new TextView(this);
        status.setText("Starting download...");
        status.setTextColor(0xFFFFFFFF);
        status.setTextSize(14.0f);
        layout.addView(status);
        android.widget.ProgressBar progress = new android.widget.ProgressBar(this, null,
                android.R.attr.progressBarStyleHorizontal);
        progress.setIndeterminate(true);
        LinearLayout.LayoutParams progressParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(18));
        progressParams.setMargins(0, dp(14), 0, dp(8));
        layout.addView(progress, progressParams);
        TextView detail = new TextView(this);
        detail.setTextColor(0xFFBAC2CC);
        detail.setTextSize(12.0f);
        layout.addView(detail);
        AlertDialog progressDialog = new AlertDialog.Builder(this)
                .setTitle("Downloading Bezels")
                .setView(layout)
                .setCancelable(false)
                .create();
        progressDialog.setOnShowListener(dialog -> {
            if (progressDialog.getWindow() != null) {
                progressDialog.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            }
        });
        progressDialog.show();
        new Thread(() -> {
            try {
                BezelDownloader.Result result = BezelDownloader.downloadGithubZip(this, url,
                        (phase, current, total, indeterminate) ->
                                runOnUiThread(() -> updateBezelDownloadProgress(
                                        status, detail, progress, phase, current, total, indeterminate)));
                runOnUiThread(() -> {
                    progressDialog.dismiss();
                    currentBezelPath = "";
                    applyBezelVisibility();
                    renderGameLibraryCards();
                    updateButtonLabels();
                    toast("Downloaded " + result.pngCount + " bezels");
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    progressDialog.dismiss();
                    toast("Bezel download failed: " + e.getMessage());
                });
            }
        }, "3do-bezel-download").start();
    }

    private void updateBezelDownloadProgress(TextView status, TextView detail,
                                             android.widget.ProgressBar progress,
                                             String phase, long current, long total,
                                             boolean indeterminate) {
        status.setText(phase);
        progress.setIndeterminate(indeterminate);
        if (!indeterminate && total > 0) {
            progress.setMax((int) Math.min(total, Integer.MAX_VALUE));
            progress.setProgress((int) Math.min(current, Integer.MAX_VALUE));
            detail.setText(formatProgressValue(current, total));
        } else if (current > 0) {
            detail.setText(formatBytes(current));
        } else {
            detail.setText("Working...");
        }
    }

    private String formatProgressValue(long current, long total) {
        if (total <= 0) return formatBytes(current);
        if (total < 2048) return current + " / " + total;
        return formatBytes(current) + " / " + formatBytes(total);
    }

    private String formatBytes(long bytes) {
        if (bytes >= 1024L * 1024L) {
            return String.format(Locale.US, "%.1f MB", bytes / (1024f * 1024f));
        }
        if (bytes >= 1024L) {
            return String.format(Locale.US, "%.1f KB", bytes / 1024f);
        }
        return bytes + " B";
    }

    // ------------------------------------------------------ logcat

    private void showLogsDialog() {
        StringBuilder logs = new StringBuilder();
        try {
            Process process = Runtime.getRuntime().exec("logcat -d -t 200 *:I");
            BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream()));
            String line;
            while ((line = reader.readLine()) != null) {
                logs.append(line).append("\n");
            }
            reader.close();
        } catch (Exception e) {
            logs.append("Error reading logs: ").append(e.getMessage());
        }
        if (logs.length() == 0) logs.append("No logs available.");
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        builder.setTitle("Application Logs");
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(dp(8), dp(8), dp(8), dp(8));
        TextView logTextView = new TextView(this);
        logTextView.setText(logs.toString());
        logTextView.setTextColor(0xFF00FF00);
        logTextView.setTextSize(10);
        logTextView.setTypeface(android.graphics.Typeface.MONOSPACE);
        ScrollView scrollView = new ScrollView(this);
        scrollView.addView(logTextView);
        layout.addView(scrollView, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        builder.setView(layout);
        builder.setPositiveButton("Close", null);
        builder.setNegativeButton("Clear", (dialog, which) -> {
            try {
                Runtime.getRuntime().exec("logcat -c");
                toast("Logs cleared");
            } catch (Exception e) {
                Log.e("3DO-Main", "Failed to clear logs", e);
            }
        });
        builder.show();
    }

    // ----------------------------------------------------- helpers

    private String getDisplayPath(String path) {
        if (path == null || path.isEmpty()) return path;
        File libraryDir = SafFileImporter.getManagedLibraryDirectory(this);
        if (path.startsWith(libraryDir.getAbsolutePath())) {
            String relative = path.substring(libraryDir.getAbsolutePath().length());
            return relative.isEmpty() ? "Managed Library" : "Managed Library" + relative;
        }
        File appRoot = SafFileImporter.getManagedAppRoot(this);
        if (path.startsWith(appRoot.getAbsolutePath())) {
            String relative = path.substring(appRoot.getAbsolutePath().length());
            return relative.isEmpty() ? "App Storage" : "App Storage" + relative;
        }
        File biosDir = new File(getFilesDir(), "bios");
        if (path.startsWith(biosDir.getAbsolutePath())) {
            String relative = path.substring(biosDir.getAbsolutePath().length());
            return relative.isEmpty() ? "Managed BIOS" : "Managed BIOS" + relative;
        }
        File driverDir = new File(getFilesDir(), "drivers");
        if (path.startsWith(driverDir.getAbsolutePath())) {
            String relative = path.substring(driverDir.getAbsolutePath().length());
            return relative.isEmpty() ? "Managed Driver" : "Managed Driver" + relative;
        }
        return path;
    }

    private void toast(String message) {
        Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
    }

    // ----------------------------------------------------- native methods

    private native boolean initEmulator(String gamePath, String biosPath);
    private native void shutdownEmulator();
    private native void pauseEmulator();
    private native void resumeEmulator();
    private native void togglePause();
    private native void resetEmulator();
    private native boolean loadCdImage(String gamePath);
    private native String getStatus();
    private native void setInputState(int button, boolean pressed);
    private native int drainAudioFrames(int[] packedFrames);
    private native boolean initRenderer(int width, int height);
    private native void cleanupRenderer();
    private native void setSurface(Surface surface);
    private native boolean saveNVRAM(String path);
    private native boolean loadNVRAM(String path);
    private native int getStateSize();
    private native boolean saveState(byte[] buf);
    private native boolean loadState(byte[] buf);
    private native void setRegion(int region);
    private native int getRegion();
    private native void setCpuSpeed(float multiplier);
    private native void setRendererType(int type);
    private native void setVulkanDriverPath(String path);
    private native void setFiltering(boolean nearest);
    private native String getRendererName();
    private native void setAspectRatio(boolean wide);
    private native boolean getAspectRatio();
    private native void setCrtShaderEnabled(boolean enabled);
    private native boolean getCrtShaderEnabled();
    private native void setResolutionScale(int scale);
    private native int getResolutionScale();
    private native void setAntiAliasingMode(int mode);
    private native int getAntiAliasingMode();
    private native void setOutputResolutionPreset(int targetHeight);
    private native int getOutputResolutionPreset();
    private native String getRenderTargetInfo();
    private native void setFlipVertical(boolean flip);
    private native void setFlipX(boolean flip);
    private native void setFlipY(boolean flip);
    private native void setRotation(int degrees);
    private native int consumeRenderedFrames();

    // ----------------------------------------------------- inner classes

    private static final class GameLibraryEntry {
        final String displayName;
        /** Set when the game is a real file in app storage. */
        final File file;
        /** Set when the game lives in a SAF tree and is read in place. */
        final Uri uri;
        IgdbService.IgdbGame igdbGame;
        Bitmap coverBitmap;
        boolean igdbLookupStarted;

        GameLibraryEntry(String displayName, File file) {
            this(displayName, file, null);
        }

        GameLibraryEntry(String displayName, File file, Uri uri) {
            this.displayName = displayName;
            this.file = file;
            this.uri = uri;
        }
    }
}
