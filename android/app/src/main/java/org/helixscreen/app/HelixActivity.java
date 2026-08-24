package org.helixscreen.app;

import android.content.res.Configuration;
import android.graphics.Color;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.Scanner;

import org.helixscreen.app.BuildConfig;
import org.libsdl.app.SDLActivity;

/**
 * HelixScreen Android activity.
 * Extends SDLActivity to provide the SDL2 + native code bridge.
 *
 * System UI behavior:
 *   - Default state: status bar AND navigation bar hidden in immersive mode.
 *   - API 30+: Uses WindowInsetsController with transient gesture bars.
 *     System edge-swipe reveals translucent bars that auto-fade. Our explicit
 *     show/hide via the custom swipe gesture uses the 3-second auto-hide timer.
 *   - API 28-29: Uses legacy setSystemUiVisibility with non-sticky IMMERSIVE
 *     flags so the hidden state persists across taps.
 *   - Reveal gesture: a swipe that starts within EDGE_ZONE_DP of the bottom
 *     edge and travels upward more than SWIPE_THRESHOLD_DP shows the nav bar.
 *     LVGL treats the drag as a scroll gesture and cancels any pending click,
 *     so bottom-row app buttons stay tappable without accidental reveals.
 *   - Always-visible mode: setNavBarAlwaysVisible(true) (called from native
 *     when the user toggles the setting) pins the nav bar onscreen and
 *     disables both the swipe reveal and the auto-hide timer. Status bar
 *     stays hidden either way.
 *   - Touch events are never consumed here; SDL / LVGL sees them all.
 */
public class HelixActivity extends SDLActivity {

    /** A swipe must start within this many dp of the bottom edge to arm. */
    private static final int EDGE_ZONE_DP = 32;

    /** Vertical travel required to trigger the reveal, in dp. */
    private static final int SWIPE_THRESHOLD_DP = 16;

    /** Nav bar hides this many ms after the last touch while it is visible. */
    private static final long NAV_HIDE_TIMEOUT_MS = 3000;

    private boolean mNavBarVisible = false;
    private boolean mSwipeArmed = false;
    private float mSwipeStartY = 0f;

    /**
     * User-controlled "keep navbar onscreen" preference (issue #908).
     * Set via the static JNI bridge {@link #setNavBarAlwaysVisible(boolean)}
     * from the C++ DisplaySettingsManager. Static so the value survives
     * activity recreation and can be pushed before onResume.
     */
    private static volatile boolean sNavBarAlwaysVisible = false;

    /**
     * True when the active app theme is light (= system bar icons should be
     * dark for legibility). Updated from {@link #setWindowBackgroundColor}
     * by computing luminance of the screen_bg color.
     */
    private static volatile boolean sLightAppearance = false;

    /**
     * Desired FLAG_KEEP_SCREEN_ON state (issue #1245).
     *
     * SDL2 adds the flag during video init (SDL_video.c disables the screensaver
     * by default), which is what we want while the app is in use — but it never
     * clears it, so the device could never power the panel down. Native
     * DisplayManager releases it when its own Display Sleep timeout fires and
     * re-asserts it on wake, so Android's display timeout genuinely turns the
     * panel off instead of us painting a black rectangle over a lit screen.
     *
     * Static so the value survives activity recreation; {@link #onResume} re-applies
     * it because Android can hand us a brand-new window with default flags.
     * True at startup to match what SDL already did.
     */
    private static volatile boolean sKeepScreenOn = true;

    /**
     * Incremented on every {@link #onResume}. Native code samples this when it
     * releases the screen lock and again while idle: a change means the app was
     * paused and resumed (i.e. the panel powered off and came back), which is the
     * only "wake" signal available when the user never touches our surface.
     */
    private static volatile int sResumeSeq = 0;

    private final Handler mHideHandler = new Handler(Looper.getMainLooper());
    private final Runnable mHideRunnable = new Runnable() {
        @Override
        public void run() {
            setNavBarVisible(false);
        }
    };
    private final Runnable mApplySystemUiRunnable = new Runnable() {
        @Override
        public void run() {
            applySystemUi();
        }
    };

    @Override
    protected String[] getLibraries() {
        return new String[]{
            "SDL2",
            // Preloaded so the native side can dlopen("libturbojpeg.so")
            // (prestonbrown/helixscreen#1245). From API 24 the linker namespace
            // model no longer resolves a bare dlopen() against an APK-bundled
            // .so by basename, so libturbojpeg must be entered through the
            // framework load list (SDLActivity calls System.loadLibrary for each
            // entry here). Without it the camera silently falls back to scalar
            // stb_image decode. Listed before "main" because CameraStream
            // constructs on the native thread after libmain loads.
            "turbojpeg",
            "main"
        };
    }

    @Override
    protected String[] getArguments() {
        java.util.List<String> args = new java.util.ArrayList<>();
        if (BuildConfig.DEBUG) {
            args.add("-vv");
        }
        // Launch with: adb shell am start -n org.helixscreen.app/.HelixActivity --ez test true
        if (getIntent() != null && getIntent().getBooleanExtra("test", false)) {
            args.add("--test");
        }
        return args.toArray(new String[0]);
    }

    // =========================================================================
    // System UI management
    // =========================================================================

    private void applySystemUi() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            applySystemUiModern(window);
        } else {
            applySystemUiLegacy(window);
        }
    }

    /**
     * API 30+: WindowInsetsController replaces the deprecated
     * setSystemUiVisibility flags with explicit show/hide calls and a
     * behavior enum that controls how gestures interact with hidden bars.
     */
    private void applySystemUiModern(Window window) {
        if (Build.VERSION.SDK_INT < 30) return;

        WindowInsetsController controller = window.getInsetsController();
        if (controller == null) return;

        // targetSdk 35+ enforces edge-to-edge — setDecorFitsSystemWindows()
        // is a no-op. Inset handling for the pinned-navbar case happens in
        // installNavbarInsetListener() which pads the content view directly.
        window.setDecorFitsSystemWindows(false);

        // Bars stay visible until explicitly hidden (matches legacy non-sticky
        // IMMERSIVE). Required for 3-button nav where users need time to tap
        // back/home/recents. Our 3-second auto-hide timer re-hides them.
        controller.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_DEFAULT);

        if (sNavBarAlwaysVisible) {
            // Pin nav bar, keep status bar hidden.
            controller.hide(WindowInsets.Type.statusBars());
            controller.show(WindowInsets.Type.navigationBars());
        } else if (mNavBarVisible) {
            controller.show(WindowInsets.Type.navigationBars());
        } else {
            controller.hide(WindowInsets.Type.systemBars());
        }

        // Icon color follows the app's own light/dark theme. Without this,
        // some OEMs (Samsung One UI) default to a light navbar with dark
        // icons and a forced white contrast scrim even when we ask for
        // transparent bars.
        int lightMask = WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS
                      | WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS;
        controller.setSystemBarsAppearance(
                sLightAppearance ? lightMask : 0, lightMask);

        // Transparent bars — disable Android's default contrast scrim
        // (API 29+ adds a white scrim behind the nav bar when enforced).
        // NOTE: in 3-button nav mode Samsung One UI ignores both of these
        // and forces a scrim that follows the SYSTEM light/dark theme.
        // Gesture nav mode honors the request.
        window.setNavigationBarColor(Color.TRANSPARENT);
        window.setStatusBarColor(Color.TRANSPARENT);
        window.setNavigationBarContrastEnforced(false);
        window.setStatusBarContrastEnforced(false);
    }

    /**
     * API 28-29: legacy setSystemUiVisibility path.
     * Uses non-sticky IMMERSIVE so HIDE_NAVIGATION persists across taps.
     */
    @SuppressWarnings("deprecation")
    private void applySystemUiLegacy(Window window) {
        int flags = View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                  | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                  | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                  | View.SYSTEM_UI_FLAG_FULLSCREEN;

        if (sLightAppearance) {
            flags |= View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR
                   | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        }

        if (!mNavBarVisible && !sNavBarAlwaysVisible) {
            flags |= View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                   | View.SYSTEM_UI_FLAG_IMMERSIVE;
        }
        window.getDecorView().setSystemUiVisibility(flags);

        window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        window.setNavigationBarColor(Color.TRANSPARENT);
        window.setStatusBarColor(Color.TRANSPARENT);
    }

    private void scheduleAutoHide() {
        mHideHandler.removeCallbacks(mHideRunnable);
        mHideHandler.postDelayed(mHideRunnable, NAV_HIDE_TIMEOUT_MS);
    }

    private void setNavBarVisible(boolean visible) {
        if (mNavBarVisible == visible) {
            return;
        }
        mNavBarVisible = visible;
        applySystemUi();
        // Auto-hide is meaningless when the user has pinned the bar.
        if (visible && !sNavBarAlwaysVisible) {
            scheduleAutoHide();
        } else {
            mHideHandler.removeCallbacks(mHideRunnable);
        }
    }

    // =========================================================================
    // Touch handling — custom bottom-edge swipe to reveal nav bar
    // =========================================================================

    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        final float density = getResources().getDisplayMetrics().density;
        final int edgeZonePx = (int) (EDGE_ZONE_DP * density + 0.5f);
        final int swipeThresholdPx = (int) (SWIPE_THRESHOLD_DP * density + 0.5f);
        final int decorHeight = getWindow().getDecorView().getHeight();

        switch (ev.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                if (!mNavBarVisible
                        && !sNavBarAlwaysVisible
                        && decorHeight > 0
                        && ev.getY() >= (decorHeight - edgeZonePx)) {
                    mSwipeArmed = true;
                    mSwipeStartY = ev.getY();
                } else {
                    mSwipeArmed = false;
                }
                if (mNavBarVisible && !sNavBarAlwaysVisible) {
                    scheduleAutoHide();
                }
                break;
            case MotionEvent.ACTION_MOVE:
                if (mSwipeArmed && (mSwipeStartY - ev.getY()) >= swipeThresholdPx) {
                    setNavBarVisible(true);
                    mSwipeArmed = false;
                    // Cancel the touch so LVGL drops any pending click on
                    // bottom-row buttons that overlap the swipe edge zone.
                    ev.setAction(MotionEvent.ACTION_CANCEL);
                    super.dispatchTouchEvent(ev);
                    return true;
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                mSwipeArmed = false;
                break;
        }
        return super.dispatchTouchEvent(ev);
    }

    // =========================================================================
    // Lifecycle — reapply system UI after focus/resume
    // =========================================================================

    @Override
    public void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (Build.VERSION.SDK_INT >= 30) {
            installNavbarInsetListener();
        }
    }

    /**
     * Listen for system-driven navbar visibility changes — Samsung One UI
     * 3-button mode shows the bar on app entry, gesture-nav reveals it on
     * bottom swipe, etc. Our own touch handler doesn't see these because
     * the system consumes the gesture. Without this hook, mNavBarVisible
     * stays out of sync and the 3-second auto-hide timer is never scheduled.
     */
    private void installNavbarInsetListener() {
        getWindow().getDecorView().setOnApplyWindowInsetsListener((v, insets) -> {
            boolean navVisibleNow =
                    insets.isVisible(WindowInsets.Type.navigationBars());
            if (navVisibleNow != mNavBarVisible) {
                mNavBarVisible = navVisibleNow;
                if (navVisibleNow && !sNavBarAlwaysVisible) {
                    scheduleAutoHide();
                } else {
                    mHideHandler.removeCallbacks(mHideRunnable);
                }
            }

            // Self-correct: if the user wants the bar pinned but something
            // hid it (SDL's COMMAND_CHANGE_WINDOW_STYLE re-asserts immersive
            // after a fold/unlock resume race), re-assert on the next tick.
            if (sNavBarAlwaysVisible && !navVisibleNow) {
                v.post(() -> {
                    WindowInsetsController c = getWindow().getInsetsController();
                    if (c != null) c.show(WindowInsets.Type.navigationBars());
                });
            }

            // When the user pinned the nav bar, inset the SDL surface so LVGL
            // doesn't render behind it (otherwise the bar overlays widgets,
            // especially on Fold 7 inner display where the bar can sit on the
            // side in landscape). With targetSdk 35 + edge-to-edge enforced,
            // setDecorFitsSystemWindows(true) is a no-op — we have to pad the
            // content view manually from the navbar insets.
            View content = findViewById(android.R.id.content);
            if (content != null) {
                if (sNavBarAlwaysVisible && navVisibleNow) {
                    android.graphics.Insets navIns =
                            insets.getInsets(WindowInsets.Type.navigationBars());
                    content.setPadding(navIns.left, navIns.top,
                                       navIns.right, navIns.bottom);
                } else {
                    content.setPadding(0, 0, 0, 0);
                }
            }
            return v.onApplyWindowInsets(insets);
        });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applySystemUi();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // A resume means the panel is on again — the signal native code uses to
        // wake a display it put to sleep by releasing FLAG_KEEP_SCREEN_ON (#1245).
        sResumeSeq++;
        // Android may have recreated the window with default flags, so re-apply
        // the desired keep-screen-on state. We are already on the UI thread here.
        applyKeepScreenOn();
        // SDL's COMMAND_CHANGE_WINDOW_STYLE handler applies immersive-sticky
        // flags asynchronously on the main thread during startup.  Post our
        // reapply to run after those messages drain so we win the race.
        View decor = getWindow().getDecorView();
        decor.removeCallbacks(mApplySystemUiRunnable);
        decor.post(mApplySystemUiRunnable);
    }

    @Override
    protected void onPause() {
        mHideHandler.removeCallbacks(mHideRunnable);
        getWindow().getDecorView().removeCallbacks(mApplySystemUiRunnable);
        super.onPause();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // Fold/unfold and other config changes listed in AndroidManifest's
        // android:configChanges do NOT trigger onResume — the activity stays
        // in the resumed state. Without this re-apply, Android resets the
        // system-bar visibility to its defaults and the user's "Keep
        // Navigation Bar" preference appears to revert. (#951)
        View decor = getWindow().getDecorView();
        decor.removeCallbacks(mApplySystemUiRunnable);
        decor.post(mApplySystemUiRunnable);
    }

    /**
     * Legacy visibility change listener for API 28-29.
     * On API 30+ the WindowInsetsController manages bar state directly, so
     * this callback fires but we rely on our own mNavBarVisible tracking.
     */
    @SuppressWarnings("deprecation")
    @Override
    public void onSystemUiVisibilityChange(int visibility) {
        if (Build.VERSION.SDK_INT >= 30) {
            // WindowInsetsController handles everything; ignore legacy callback
            return;
        }
        boolean navVisibleNow = (visibility & View.SYSTEM_UI_FLAG_HIDE_NAVIGATION) == 0;
        if (navVisibleNow == mNavBarVisible) {
            return;
        }
        mNavBarVisible = navVisibleNow;
        if (navVisibleNow) {
            scheduleAutoHide();
        } else {
            mHideHandler.removeCallbacks(mHideRunnable);
        }
    }

    // =========================================================================
    // JNI theme bridge — called from native theme_manager
    // =========================================================================

    /**
     * Toggle the "keep navigation bar onscreen" preference (issue #908).
     * Called from native code via JNI when the user flips the setting and
     * once at startup with the persisted value. Status bar stays hidden
     * either way — this affects only the navigation bar.
     *
     * @param alwaysVisible  true to pin nav bar onscreen, false to restore
     *                       the default immersive + swipe-to-reveal behavior
     */
    public static void setNavBarAlwaysVisible(final boolean alwaysVisible) {
        // Update the static flag unconditionally so onResume/onWindowFocusChanged
        // pick it up even if the activity is being created right now.
        sNavBarAlwaysVisible = alwaysVisible;

        final android.content.Context ctx = SDLActivity.getContext();
        if (!(ctx instanceof HelixActivity)) return;
        final HelixActivity helix = (HelixActivity) ctx;
        helix.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (alwaysVisible) {
                    helix.mHideHandler.removeCallbacks(helix.mHideRunnable);
                    helix.mNavBarVisible = true;
                } else {
                    // Drop back to immersive on next applySystemUi.
                    helix.mNavBarVisible = false;
                }
                helix.applySystemUi();
            }
        });
    }

    /**
     * Allow or prevent the device from sleeping while HelixScreen is foregrounded
     * (issue #1245). Called from native DisplayManager when its Display Sleep
     * timeout fires (false) and on wake (true).
     *
     * With Display Sleep = "Never" native code never passes false, so a
     * wall-mounted tablet stays lit exactly as before.
     *
     * @param keepOn true to hold the screen awake, false to let Android's own
     *               display timeout power the panel off
     */
    public static void setKeepScreenOn(final boolean keepOn) {
        // Update the static first so onResume picks it up even if the activity is
        // being created right now (same discipline as setNavBarAlwaysVisible).
        sKeepScreenOn = keepOn;

        final android.content.Context ctx = SDLActivity.getContext();
        if (!(ctx instanceof HelixActivity)) return;
        final HelixActivity helix = (HelixActivity) ctx;
        // Window flag changes are UI-thread only.
        helix.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                helix.applyKeepScreenOn();
            }
        });
    }

    /**
     * Number of times {@link #onResume} has run. Read from native code to detect a
     * pause/resume cycle without a touch event. See {@link #sResumeSeq}.
     */
    public static int getResumeSeq() {
        return sResumeSeq;
    }

    /** Push {@link #sKeepScreenOn} onto the window. UI thread only. */
    private void applyKeepScreenOn() {
        Window window = getWindow();
        if (window == null) return;
        if (sKeepScreenOn) {
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        }
    }

    /**
     * Set the window background color to match the active theme.
     * Called from native code via JNI whenever the theme changes so the
     * area behind transparent system bars matches the app's screen_bg.
     *
     * @param argb  0xAARRGGBB color value (alpha typically 0xFF)
     */
    public static void setWindowBackgroundColor(final int argb) {
        final SDLActivity activity = (SDLActivity) SDLActivity.getContext();
        if (activity == null) return;

        // ITU-R BT.709 relative luminance — decides whether system-bar icons
        // should be dark (light bg) or light (dark bg).
        int r = (argb >> 16) & 0xFF;
        int g = (argb >> 8) & 0xFF;
        int b = argb & 0xFF;
        double lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
        sLightAppearance = lum > 0.5;

        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                activity.getWindow().getDecorView()
                        .setBackgroundColor(argb);
                if (activity instanceof HelixActivity) {
                    ((HelixActivity) activity).applySystemUi();
                }
            }
        });
    }

    // =========================================================================
    // JNI HTTPS bridge — called from native crash reporter
    // =========================================================================

    /**
     * Describe a failure for the "0\nERROR" transport-failure contract.
     * getMessage() is null on several common IOExceptions, and "HTTP 0: null"
     * tells the reader nothing; the exception type at least names the failure.
     */
    private static String errorText(Exception e) {
        String msg = e.getMessage();
        return (msg != null && !msg.isEmpty()) ? msg : e.getClass().getName();
    }

    /**
     * Read a response body for a status code that has already been obtained.
     *
     * getErrorStream() returns null whenever an error response carries no body,
     * and getInputStream() throws for most 4xx/5xx. Either one reaching the
     * caller collapses a real status code into the "0" transport-failure path,
     * so a rejected API key (401/403) and an oversized payload (413) become
     * indistinguishable from having no network at all. Every body-read failure
     * is absorbed here and reported as an empty body, leaving the status intact.
     *
     * @param conn    Connection whose response has already been read
     * @param status  Status code from conn.getResponseCode()
     * @return        Response body, or "" when there is none to read
     */
    private static String readResponseBody(HttpURLConnection conn, int status) {
        InputStream in = null;
        try {
            in = (status >= 200 && status < 400) ? conn.getInputStream() : conn.getErrorStream();
        } catch (Exception e) {
            Log.w("HelixHTTPS", "no response stream for status " + status + ": " + e.getMessage());
        }
        if (in == null) {
            return "";
        }
        try (Scanner s = new Scanner(in, "UTF-8")) {
            s.useDelimiter("\\A");
            return s.hasNext() ? s.next() : "";
        } catch (Exception e) {
            Log.w("HelixHTTPS", "response body read failed: " + e.getMessage());
            return "";
        }
    }

    /**
     * Perform an HTTP(S) GET using Android's built-in TLS stack.
     * Called from native code via JNI when libhv lacks SSL support.
     *
     * @param url         Full URL (http:// or https://)
     * @param userAgent   User-Agent header value
     * @param accept      Accept header value (may be empty)
     * @param timeoutSec  Connection + read timeout in seconds
     * @return            "STATUS_CODE\nRESPONSE_BODY" on success,
     *                    "0\nERROR_MESSAGE" on failure
     */
    public static String httpsGet(String url, String userAgent, String accept, int timeoutSec) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setRequestMethod("GET");
            conn.setConnectTimeout(timeoutSec * 1000);
            conn.setReadTimeout(timeoutSec * 1000);
            conn.setRequestProperty("User-Agent", userAgent);
            if (accept != null && !accept.isEmpty()) {
                conn.setRequestProperty("Accept", accept);
            }

            int status = conn.getResponseCode();
            return status + "\n" + readResponseBody(conn, status);
        } catch (Exception e) {
            Log.w("HelixHTTPS", "GET failed: " + errorText(e));
            return "0\n" + errorText(e);
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    /**
     * Perform an HTTPS POST using Android's built-in TLS stack.
     * Called from native code via JNI when libhv lacks SSL support.
     *
     * @param url         Full HTTPS URL
     * @param body        JSON request body
     * @param userAgent   User-Agent header value
     * @param apiKey      X-API-Key header value
     * @param timeoutSec  Connection + read timeout in seconds
     * @return            "STATUS_CODE\nRESPONSE_BODY" on success,
     *                    "0\nERROR_MESSAGE" on failure
     */
    public static String httpsPost(String url, String body, String userAgent,
                                   String apiKey, int timeoutSec) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setRequestMethod("POST");
            conn.setDoOutput(true);
            conn.setConnectTimeout(timeoutSec * 1000);
            conn.setReadTimeout(timeoutSec * 1000);
            conn.setRequestProperty("Content-Type", "application/json");
            conn.setRequestProperty("User-Agent", userAgent);
            conn.setRequestProperty("X-API-Key", apiKey);

            byte[] payload = body.getBytes(StandardCharsets.UTF_8);
            conn.setFixedLengthStreamingMode(payload.length);

            try (OutputStream os = conn.getOutputStream()) {
                os.write(payload);
            }

            int status = conn.getResponseCode();
            return status + "\n" + readResponseBody(conn, status);
        } catch (Exception e) {
            Log.w("HelixHTTPS", "POST failed: " + errorText(e));
            return "0\n" + errorText(e);
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    /**
     * HTTPS POST with a raw binary body — for gzip-compressed payloads where
     * round-tripping bytes through a Java String would corrupt them (debug
     * bundle upload). Same STATUS\nBODY / 0\nERROR contract as httpsPost.
     */
    public static String httpsPostBinary(String url, byte[] body, String contentType,
                                         String contentEncoding, String userAgent,
                                         String apiKey, int timeoutSec) {
        HttpURLConnection conn = null;
        try {
            conn = (HttpURLConnection) new URL(url).openConnection();
            conn.setRequestMethod("POST");
            conn.setDoOutput(true);
            conn.setConnectTimeout(timeoutSec * 1000);
            conn.setReadTimeout(timeoutSec * 1000);
            conn.setRequestProperty("Content-Type", contentType);
            if (contentEncoding != null && !contentEncoding.isEmpty())
                conn.setRequestProperty("Content-Encoding", contentEncoding);
            conn.setRequestProperty("User-Agent", userAgent);
            conn.setRequestProperty("X-API-Key", apiKey);
            conn.setFixedLengthStreamingMode(body.length);
            try (OutputStream os = conn.getOutputStream()) {
                os.write(body);
            }
            int status = conn.getResponseCode();
            return status + "\n" + readResponseBody(conn, status);
        } catch (Exception e) {
            Log.w("HelixHTTPS", "POST binary failed: " + errorText(e));
            return "0\n" + errorText(e);
        } finally {
            if (conn != null) conn.disconnect();
        }
    }
}
