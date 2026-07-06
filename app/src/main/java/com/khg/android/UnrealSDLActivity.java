package com.khg.android;

import android.content.Context;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.File;

import org.libsdl.app.SDLActivity;

public class UnrealSDLActivity extends SDLActivity implements InputManager.InputDeviceListener {
    private static final String TAG = "UE1Controller";

    private File selectedRoot;
    private InputManager inputManager;
    private UnrealTouchOverlayViewV124 touchOverlayViewV124; // UNREAL_ANDROID_TOUCH_OVERLAY_V125

    // ANDROID_NATIVE_CONTROLLER_BACKEND_V88
    private static native boolean nativeAndroidControllerIsEnabled();

    private static native boolean nativeAndroidControllerKey(
            int deviceId,
            int vendorId,
            int productId,
            int keyCode,
            int scanCode,
            int action,
            int repeatCount,
            int source,
            String deviceName);

    private static native boolean nativeAndroidControllerMotion(
            int deviceId,
            int vendorId,
            int productId,
            int source,
            String deviceName,
            float axisX,
            float axisY,
            float axisZ,
            float axisRZ,
            float axisLTrigger,
            float axisRTrigger,
            float axisBrake,
            float axisGas,
            float axisHatX,
            float axisHatY);

    private static native void nativeAndroidControllerDeviceChanged(
            int deviceId,
            int vendorId,
            int productId,
            int source,
            String deviceName,
            int eventType);

    private static native void nativeAndroidControllerReset(); // ANDROID_CONTROLLER_NATIVE_RESET_V88
    private static native boolean nativeAndroidIsMenuV124(); // UNREAL_ANDROID_TOUCH_OVERLAY_V125 (broad: incl. Console)
    private static native boolean nativeAndroidIsUiMenuV142(); // UNREAL_ANDROID_TOUCH_LCARS_V142 (strict: Menuing only)
    private static native boolean nativeAndroidIsCutsceneV142(); // UNREAL_ANDROID_TOUCH_FMV_SKIP_V142
    private static native void nativeAndroidTouchLookV131(float x, float y); // UNREAL_ANDROID_TOUCH_RIGHT_LOOK_UT99_V131 explicit native path
    private static native void nativeAndroidTouchLookV101(float x, float y); // UNREAL_ANDROID_TOUCH_RIGHT_LOOK_UT99_V129 fallback
    private static native void nativeAndroidTouchLookV124(float x, float y); // UNREAL_ANDROID_TOUCH_OVERLAY_V125 fallback

    private void resetAndroidNativeControllerState() {
        try {
            nativeAndroidControllerReset();
        } catch (UnsatisfiedLinkError ignored) {
            // Library may not be ready during early Activity startup. SDL remains fallback.
        }
    }

    private File selectedRootFromIntentOrScan() {
        if (selectedRoot != null) return selectedRoot;
        String fromIntent = getIntent() != null ? getIntent().getStringExtra(UnrealDataPaths.EXTRA_UNREAL_ROOT) : null;
        if (fromIntent != null && fromIntent.length() > 0) {
            File candidate = new File(fromIntent);
            if (UnrealDataPaths.hasRequiredData(candidate, true)) {
                android.util.Log.i(UnrealDataPaths.TAG_STARTUP, "using data root from intent: " + candidate.getAbsolutePath());
                selectedRoot = candidate;
                return selectedRoot;
            }
            android.util.Log.w(UnrealDataPaths.TAG_STARTUP, "intent data root invalid, rescanning: " + candidate.getAbsolutePath());
        }
        selectedRoot = UnrealDataPaths.findBestUnrealRoot(this);
        return selectedRoot;
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "openal", "Unreal" };
    }

    @Override
    protected String[] getArguments() {
        // Pass the selected data root only to SDL_main. The native Android
        // launcher consumes --ue1-root before appSetCmdLine(), so UE1 itself
        // never sees this as a map URL or unknown command token.
        selectedRoot = selectedRootFromIntentOrScan();
        return new String[] { "--ue1-root", selectedRoot.getAbsolutePath() };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().getDecorView().setBackgroundColor(Color.BLACK);
        getWindow().setStatusBarColor(Color.TRANSPARENT);
        getWindow().setNavigationBarColor(Color.TRANSPARENT);
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            getWindow().setDecorFitsSystemWindows(false);
        }
        if (android.os.Build.VERSION.SDK_INT >= 28) {
            WindowManager.LayoutParams attrs = getWindow().getAttributes();
            attrs.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(attrs);
        }
        hideSystemUi();
        selectedRoot = selectedRootFromIntentOrScan();
        UnrealDataPaths.ensureWritableConfigFiles(this, selectedRoot); // UNREAL_ANDROID_CONFIG_BOOTSTRAP_REV31_PATH_FALLBACK_MORE_ROOTS
        super.onCreate(savedInstanceState);

        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(this, new Handler(Looper.getMainLooper()));
            logConnectedControllerDevices();
        }

        hideSystemUi();
        scheduleImmersiveRefresh();
        installUnrealTouchOverlayV124(); // UNREAL_ANDROID_TOUCH_OVERLAY_V125
    }

    @Override
    protected void onDestroy() {
        if (inputManager != null) {
            inputManager.unregisterInputDeviceListener(this);
            inputManager = null;
        }
        super.onDestroy();
    }

    // UNREAL_ANDROID_SUSPEND_RESUME_HARDEN_V149: a helper throwing must never tear down the Activity
    // mid-suspend/resume. Each step runs isolated; super.on{Pause,Resume}() (which drives SDL's
    // background/foreground transition and surface teardown/recreation) always runs.
    @Override
    protected void onPause() {
        safe(this::resetAndroidNativeControllerState); // ANDROID_CONTROLLER_NATIVE_RESET_V88
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        forceLandscapeV149(); // re-lock: SDL video init may have changed the requested orientation
        safe(this::resetAndroidNativeControllerState); // ANDROID_CONTROLLER_NATIVE_RESET_V88
        safe(this::hideSystemUi);
        safe(this::scheduleImmersiveRefresh);
        safe(this::installUnrealTouchOverlayV124); // UNREAL_ANDROID_TOUCH_OVERLAY_V125
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            forceLandscapeV149(); // re-lock after returning to foreground
            safe(this::resetAndroidNativeControllerState); // ANDROID_CONTROLLER_NATIVE_RESET_V88
            safe(this::hideSystemUi);
            safe(this::scheduleImmersiveRefresh);
            safe(this::bringTouchOverlayToFrontV125); // UNREAL_ANDROID_TOUCH_OVERLAY_V125
        }
    }

    // UNREAL_ANDROID_FORCE_LANDSCAPE_V149: SDL's video init calls SDLActivity.setOrientation()/
    // setOrientationBis(), which recomputes the requested orientation from the window size /
    // SDL_HINT_ORIENTATIONS and OVERRIDES both the manifest and our onCreate lock — that is why the game
    // would not stay in landscape. Override the hook and force strict landscape unconditionally (no
    // portrait, no 180 flip). Also re-asserted on resume/focus in case anything else changes it.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        forceLandscapeV149();
    }

    private void forceLandscapeV149() {
        try {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
        } catch (Throwable ignored) {
        }
    }

    // Run a lifecycle helper without letting it crash the suspend/resume transition.
    private static void safe(Runnable r) {
        try {
            r.run();
        } catch (Throwable ignored) {
        }
    }

    // UNREAL_ANDROID_TOUCH_BACK_MENU_V142: route the hardware/gesture Back to the in-game
    // menu (Escape) instead of finishing the Activity. KEYCODE_MENU maps to IK_Escape
    // natively, which opens the menu in gameplay and backs out one level inside menus.
    private void sendNativeMenuToggleV142() {
        // UNREAL_ANDROID_BACK_MENU_DIRECT_V149: route Back through the SAME reliable direct-touch path as the
        // on-screen MENU button (DIRECT_MENU=910200 -> native sets Bindings[IK_Escape]=ShowMenu +
        // CauseInputEvent). The old KEYCODE_MENU controller path needed config bindings loaded and a
        // registered controller device, so Back could silently do nothing. This opens the menu in gameplay
        // and backs out one level inside menus, in every state.
        try {
            nativeAndroidControllerKey(-136, 0, 0, 910200, 0, KeyEvent.ACTION_DOWN, 0, InputDevice.SOURCE_GAMEPAD, "UnrealBackMenuDirectV149");
            nativeAndroidControllerKey(-136, 0, 0, 910200, 0, KeyEvent.ACTION_UP, 0, InputDevice.SOURCE_GAMEPAD, "UnrealBackMenuDirectV149");
        } catch (Throwable ignored) {
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        // Fallback for gesture-nav devices that deliver Back here instead of via dispatchKeyEvent.
        // Do NOT call super (that would finish the Activity / quit the game).
        sendNativeMenuToggleV142();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // UNREAL_ANDROID_TOUCH_BACK_MENU_V142: hardware Back opens/closes the UE1 menu.
        if (event != null && event.getKeyCode() == KeyEvent.KEYCODE_BACK) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                sendNativeMenuToggleV142();
            }
            return true; // consume both DOWN and UP so Android never finishes the Activity
        }

        if (isMenuStartKeyV124(event.getKeyCode())) {
            // UNREAL_ANDROID_START_MENU_TAP_V124:
            // Some Android/OUYA controllers lose the matching KEY_UP for START/MENU.
            // Queue one native press+release on the first ACTION_DOWN so opening the menu
            // never needs a second physical press.
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
                if (sendNativeKeyTapV124(event)) return true;
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                return true;
            }
        }

        if (isControllerSource(event.getSource()) || isGamepadButton(event.getKeyCode()) || isOuyaMenuKey(event.getKeyCode())) {
            InputDevice device = event.getDevice();
            if (device != null) {
                try {
                    // Android pads frequently report L2/R2 both as analog axes and as digital keys.
                    // If the native backend is active and analog trigger ranges exist, prefer the
                    // MotionEvent axis path and suppress the duplicate digital trigger key.
                    // If the backend is disabled, never consume here; SDL remains the fallback.
                    // ANDROID_NATIVE_CONTROLLER_TRIGGER_DEDUPE_V87
                    if (isTriggerKey(event.getKeyCode())
                            && nativeAndroidControllerIsEnabled()
                            && hasAnalogTriggerAxis(device, event.getKeyCode())) {
                        return true;
                    }
                    boolean consumed = nativeAndroidControllerKey(
                            event.getDeviceId(),
                            device.getVendorId(),
                            device.getProductId(),
                            event.getKeyCode(),
                            event.getScanCode(),
                            event.getAction(),
                            event.getRepeatCount(),
                            event.getSource(),
                            device.getName());
                    if (consumed) return true;
                } catch (UnsatisfiedLinkError ignored) {
                    // Library not ready: fall back to SDLActivity.
                }
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getActionMasked() == MotionEvent.ACTION_MOVE
                || event.getActionMasked() == MotionEvent.ACTION_HOVER_MOVE)
                && isControllerSource(event.getSource())) {
            InputDevice device = event.getDevice();
            if (device != null) {
                try {
                    boolean consumed = nativeAndroidControllerMotion(
                            event.getDeviceId(),
                            device.getVendorId(),
                            device.getProductId(),
                            event.getSource(),
                            device.getName(),
                            event.getAxisValue(MotionEvent.AXIS_X),
                            event.getAxisValue(MotionEvent.AXIS_Y),
                            getSignedControllerAxisWithFallback(event, device, MotionEvent.AXIS_Z, MotionEvent.AXIS_RX), // ANDROID_NATIVE_CONTROLLER_RIGHT_STICK_RXRY_FALLBACK_V116
                            getSignedControllerAxisWithFallback(event, device, MotionEvent.AXIS_RZ, MotionEvent.AXIS_RY), // ANDROID_NATIVE_CONTROLLER_RIGHT_STICK_RXRY_FALLBACK_V116
                            event.getAxisValue(MotionEvent.AXIS_LTRIGGER),
                            event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                            event.getAxisValue(MotionEvent.AXIS_BRAKE),
                            event.getAxisValue(MotionEvent.AXIS_GAS),
                            event.getAxisValue(MotionEvent.AXIS_HAT_X),
                            event.getAxisValue(MotionEvent.AXIS_HAT_Y));
                    if (consumed) return true;
                } catch (UnsatisfiedLinkError ignored) {
                    // Library not ready: fall back to SDLActivity.
                }
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }

    private static float getSignedControllerAxisWithFallback(MotionEvent event, InputDevice device, int primaryAxis, int fallbackAxis) {
        // ANDROID_NATIVE_CONTROLLER_RIGHT_STICK_RXRY_FALLBACK_V116
        // Some Android devices expose the right stick as Z/RZ, others as RX/RY.
        // Prefer an axis only when Android reports it as signed (-1..+1), so
        // trigger-style 0..1 axes cannot accidentally rotate the camera.
        float primary = getSignedControllerAxis(event, device, primaryAxis);
        float fallback = getSignedControllerAxis(event, device, fallbackAxis);
        return Math.abs(fallback) > Math.abs(primary) ? fallback : primary;
    }

    private static float getSignedControllerAxis(MotionEvent event, InputDevice device, int axis) {
        if (device == null) return 0.0f;
        InputDevice.MotionRange range = device.getMotionRange(axis, event.getSource());
        if (range == null) {
            range = device.getMotionRange(axis);
        }
        if (range == null) return 0.0f;
        if (!(range.getMin() < 0.0f && range.getMax() > 0.0f)) return 0.0f;
        return event.getAxisValue(axis);
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        notifyControllerDevice(deviceId, 1);
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        try {
            nativeAndroidControllerDeviceChanged(deviceId, 0, 0, 0, "", 2);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        notifyControllerDevice(deviceId, 3);
    }

    private void notifyControllerDevice(int deviceId, int eventType) {
        InputDevice device = InputDevice.getDevice(deviceId);
        if (device == null) return;
        if (!isControllerSource(device.getSources())) return;
        try {
            nativeAndroidControllerDeviceChanged(
                    device.getId(),
                    device.getVendorId(),
                    device.getProductId(),
                    device.getSources(),
                    device.getName(),
                    eventType);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private void logConnectedControllerDevices() {
        for (int id : InputDevice.getDeviceIds()) {
            notifyControllerDevice(id, 0);
        }
    }

    private boolean isControllerSource(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    private boolean isGamepadButton(int keyCode) {
        return keyCode >= KeyEvent.KEYCODE_BUTTON_A && keyCode <= KeyEvent.KEYCODE_BUTTON_MODE
                || keyCode >= KeyEvent.KEYCODE_DPAD_UP && keyCode <= KeyEvent.KEYCODE_DPAD_CENTER;
    }

    private boolean isOuyaMenuKey(int keyCode) {
        // UNREAL_ANDROID_CONTROLLER_DIRECT_V122
        // OUYA and several Android-TV pads report their center/system button as
        // KEYCODE_MENU without a gamepad source flag. Route it through the native
        // controller path anyway, matching the proven UT99 controller handling.
        return keyCode == KeyEvent.KEYCODE_MENU || keyCode == KeyEvent.KEYCODE_BUTTON_MODE;
    }

    private boolean isMenuStartKeyV124(int keyCode) {
        return keyCode == KeyEvent.KEYCODE_MENU
                || keyCode == KeyEvent.KEYCODE_BUTTON_START
                || keyCode == KeyEvent.KEYCODE_BUTTON_MODE;
    }

    private boolean sendNativeKeyTapV124(KeyEvent event) {
        InputDevice device = event != null ? event.getDevice() : null;
        int deviceId = event != null ? event.getDeviceId() : -124;
        int vendorId = device != null ? device.getVendorId() : 0;
        int productId = device != null ? device.getProductId() : 0;
        int source = event != null ? event.getSource() : InputDevice.SOURCE_GAMEPAD;
        String name = device != null ? device.getName() : "UnrealTouchStart";
        int keyCode = event != null ? event.getKeyCode() : KeyEvent.KEYCODE_MENU;
        int scanCode = event != null ? event.getScanCode() : 0;
        try {
            boolean down = nativeAndroidControllerKey(deviceId, vendorId, productId, keyCode, scanCode, KeyEvent.ACTION_DOWN, 0, source, name);
            boolean up = nativeAndroidControllerKey(deviceId, vendorId, productId, keyCode, scanCode, KeyEvent.ACTION_UP, 0, source, name);
            return down || up;
        } catch (UnsatisfiedLinkError ignored) {
            return false;
        }
    }

    private boolean isTriggerKey(int keyCode) {
        return keyCode == KeyEvent.KEYCODE_BUTTON_L2 || keyCode == KeyEvent.KEYCODE_BUTTON_R2;
    }

    private boolean hasAnalogTriggerAxis(InputDevice device, int keyCode) {
        if (device == null) return false;
        int primaryAxis = keyCode == KeyEvent.KEYCODE_BUTTON_L2
                ? MotionEvent.AXIS_LTRIGGER
                : MotionEvent.AXIS_RTRIGGER;
        int aliasAxis = keyCode == KeyEvent.KEYCODE_BUTTON_L2
                ? MotionEvent.AXIS_BRAKE
                : MotionEvent.AXIS_GAS;
        for (InputDevice.MotionRange range : device.getMotionRanges()) {
            int axis = range.getAxis();
            if (axis == primaryAxis || axis == aliasAxis) return true;
        }
        return false;
    }


    // UNREAL_ANDROID_TOUCH_OVERLAY_V125 / re-enabled by UNREAL_ANDROID_TOUCH_LCARS_V142
    private void installUnrealTouchOverlayV124() {
        // [KHG] V142: re-enabled with a purpose-built vector LCARS overlay (Voyager SP port).
        // The old stock overlay was disabled because (1) it claimed every screen touch as
        // stick/look so menu taps never reached SDL, and (2) it needed PNG icon assets that
        // do not ship in KHG so its buttons drew blank. The V142 overlay fixes both.
        try {
            if (touchOverlayViewV124 != null) {
                bringTouchOverlayToFrontV125();
                return;
            }
            ensureTouchControlsConfigDefaultV124();
            touchOverlayViewV124 = new UnrealTouchOverlayViewV124(this);
            android.view.ViewGroup.LayoutParams lp = new android.view.ViewGroup.LayoutParams(
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT);

            // Add directly to SDLActivity's own root layout, above the SDL SurfaceView.
            // addContentView() may end up below the Surface on some Android 8+ devices/ROMs.
            if (mLayout != null) {
                mLayout.addView(touchOverlayViewV124, lp);
            } else {
                addContentView(touchOverlayViewV124, lp);
            }
            bringTouchOverlayToFrontV125();
            android.util.Log.i(TAG, "UNREAL_ANDROID_TOUCH_LCARS_V142 installed in SDL root layout");
        } catch (Throwable t) {
            android.util.Log.e(TAG, "UNREAL_ANDROID_TOUCH_LCARS_V142 install failed", t);
        }
    }

    // UNREAL_ANDROID_TOUCH_OVERLAY_V125
    private void bringTouchOverlayToFrontV125() {
        if (touchOverlayViewV124 == null) return;
        try {
            touchOverlayViewV124.bringToFront();
            if (android.os.Build.VERSION.SDK_INT >= 21) {
                touchOverlayViewV124.setElevation(10000.0f);
                touchOverlayViewV124.setTranslationZ(10000.0f);
            }
            touchOverlayViewV124.requestLayout();
            touchOverlayViewV124.invalidate();
        } catch (Throwable ignored) {
        }
    }

    private File unrealSystemDirV124() {
        File root = selectedRootFromIntentOrScan();
        if (root == null) return null;
        return new File(root, "System");
    }

    private void ensureTouchControlsConfigDefaultV124() {
        File systemDir = unrealSystemDirV124();
        if (systemDir == null) return;
        File ini = new File(systemDir, "User.ini");
        try {
            if (!systemDir.exists()) systemDir.mkdirs();
            String text = ini.exists() ? readSmallTextFileV124(ini) : "";
            if (text.indexOf("bTouchControls=") < 0) {
                java.io.FileWriter fw = new java.io.FileWriter(ini, true);
                try {
                    fw.write("\n; UNREAL_ANDROID_TOUCH_OVERLAY_V125 default enabled on first start\n");
                    fw.write("[Unreal.UnrealOptionsMenu]\n");
                    fw.write("bTouchControls=True\n");
                } finally {
                    fw.close();
                }
                android.util.Log.i(TAG, "UNREAL_ANDROID_TOUCH_OVERLAY_V125 default config appended to " + ini.getAbsolutePath());
            }
        } catch (Throwable t) {
            android.util.Log.w(TAG, "UNREAL_ANDROID_TOUCH_OVERLAY_V125 could not ensure default", t);
        }
    }

    private String readSmallTextFileV124(File file) throws java.io.IOException {
        java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(file));
        try {
            StringBuilder sb = new StringBuilder();
            String line;
            int lines = 0;
            while ((line = br.readLine()) != null && lines++ < 4096) {
                sb.append(line).append('\n');
            }
            return sb.toString();
        } finally {
            br.close();
        }
    }

    private boolean readTouchControlsEnabledV124() {
        File systemDir = unrealSystemDirV124();
        Boolean found = null;
        if (systemDir != null) {
            // UNREAL_ANDROID_SINGLE_INI_V148: Unreal.ini is the authoritative source (the in-game options
            // toggle SaveConfig-writes bTouchControls there). Read it LAST so it wins. User.ini/AndroidUI.ini
            // are legacy fallbacks for older installs that seeded the flag elsewhere; Default.ini is gone.
            found = readTouchControlsFlagV124(new File(systemDir, "User.ini"), found);
            found = readTouchControlsFlagV124(new File(systemDir, "AndroidUI.ini"), found);
            found = readTouchControlsFlagV124(new File(systemDir, "Unreal.ini"), found);
        }
        return found != null ? found.booleanValue() : true;
    }

    private Boolean readTouchControlsFlagV124(File ini, Boolean current) {
        if (ini == null || !ini.exists()) return current;
        try {
            java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(ini));
            try {
                String line;
                boolean inSection = false;
                Boolean found = current;
                while ((line = br.readLine()) != null) {
                    String t = line.trim();
                    if (t.length() == 0 || t.startsWith(";") || t.startsWith("#")) continue;
                    if (t.startsWith("[") && t.endsWith("]")) {
                        inSection = t.equalsIgnoreCase("[Unreal.UnrealOptionsMenu]")
                                || t.equalsIgnoreCase("[UnrealOptionsMenu]");
                        continue;
                    }
                    int eq = t.indexOf('=');
                    if (eq > 0) {
                        String key = t.substring(0, eq).trim();
                        String value = t.substring(eq + 1).trim();
                        if ((inSection && key.equalsIgnoreCase("bTouchControls")) || key.equalsIgnoreCase("bTouchControls")
                                || (key.equalsIgnoreCase("UseJoystick") && found == null)) {
                            // v125: old Unreal.u still uses the legacy Joystick row.  Treat its
                            // saved UseJoystick value as Touch Controls only when bTouchControls
                            // has not been written yet.
                            found = !(value.equalsIgnoreCase("false")
                                    || value.equals("0")
                                    || value.equalsIgnoreCase("no")
                                    || value.equalsIgnoreCase("off"));
                        }
                    }
                }
                return found;
            } finally {
                br.close();
            }
        } catch (Throwable t) {
            return current;
        }
    }

    private static float clampV124(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // ============================================================================
    // UNREAL_ANDROID_TOUCH_LCARS_V142
    // Vector LCARS touch overlay — a faithful port of the Voyager (Elite Force SP)
    // SDL overlay, adapted to KHG's Java overlay + native-injection plumbing.
    //
    //  ONE overlay everywhere (no separate menu strip):
    //   - left anchored move stick (orange ring + gold thumb)
    //   - right action pills FIRE / ALT / SWITCH / JUMP / CROUCH (no USE, no MISSION/SCORE)
    //
    //  In gameplay: stick moves, look on the right half, FIRE/ALT drag-to-aim, CROUCH toggles.
    //  In a UE1 menu (incl. the main menu): the SAME overlay stays up — the stick navigates
    //  (DPAD up/down/left/right) and FIRE confirms (Enter). The other gameplay buttons are
    //  inert there. The hardware Back button opens/closes the menu.
    //
    //  During an FMV/cutscene: the overlay releases the touch (returns false) so the tap
    //  reaches SDL and FMVPlayer::PollSkip skips the clip, exactly like tapping the bare surface.
    // ============================================================================
    private static final class UnrealTouchOverlayViewV124 extends View {
        private final UnrealSDLActivity activity;
        private final android.graphics.Paint paint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private final android.graphics.Paint textPaint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private final android.graphics.RectF rect = new android.graphics.RectF();
        private final android.util.SparseArray<Integer> roles = new android.util.SparseArray<Integer>();

        // LCARS palette (ARGB), matching Voyager touch_controls.h fan-recreation values.
        private static final int LCARS_ORANGE   = 0xFFFF9900;
        private static final int LCARS_GOLD      = 0xFFFFAA00;
        private static final int LCARS_SUNFLOWER = 0xFFFFCC99;
        private static final int LCARS_ICE       = 0xFF99CCFF;
        private static final int LCARS_VIOLET    = 0xFFCC99FF;
        private static final int LCARS_MARS      = 0xFFFF2200;
        private static final int LCARS_HOPBUSH   = 0xFFCC6699; // pressed/active swap

        // Move stick (normalized screen coords, Voyager TC_PAD_*).
        private static final float PAD_CX = 0.135f, PAD_CY = 0.70f, PAD_R = 0.060f;

        // UNREAL_ANDROID_TOUCH_LOOK_SENSITIVITY_V142: relative-swipe look gain (was 0.0210).
        private static final float LOOK_GAIN_V142 = 0.0260f;

        // Native direct gameplay keycodes (handled in NSDLViewport UE1AndroidTouchButtonDirectHandleV136).
        private static final int DIRECT_FIRE = 910105, DIRECT_ALT = 910104, DIRECT_JUMP = 910096, DIRECT_CROUCH = 910097, DIRECT_NEXT = 910103;
        private static final int DIRECT_MENU = 910200; // UNREAL_ANDROID_TOUCH_MENU_DIRECT_V144 -> native injects Escape (ShowMenu)

        // Per-pointer role encoding stored in `roles`.
        private static final int ROLE_STICK = 1, ROLE_LOOK = 2, ROLE_MENU = 3, ROLE_ACTION = 100;

        // Standalone MENU (Escape) button, top-left. Always visible so the menu is reachable even where
        // the hardware Back key is unreliable. A tap sends KEYCODE_MENU, which the native input path
        // maps to IK_Escape (opens/closes the UE1 menu, and confirms "PRESS ESC TO BEGIN").
        private static final float MENU_CX = 0.055f, MENU_CY = 0.060f, MENU_HW = 0.050f, MENU_HH = 0.032f;

        // --- action pills (index order is fixed; used for the state arrays) ---
        private static final int A_FIRE = 0, A_ALT = 1, A_SWITCH = 2, A_JUMP = 3, A_CROUCH = 4, A_COUNT = 5;
        private final float[]   aCx     = { 0.855f, 0.950f, 0.950f, 0.790f, 0.665f };
        private final float[]   aCy     = { 0.770f, 0.640f, 0.520f, 0.930f, 0.930f };
        private final float[]   aHw     = { 0.062f, 0.042f, 0.054f, 0.050f, 0.054f };
        private final float[]   aHh     = { 0.050f, 0.034f, 0.034f, 0.038f, 0.034f };
        private final int[]     aCol    = { LCARS_MARS, LCARS_ORANGE, LCARS_SUNFLOWER, LCARS_ICE, LCARS_VIOLET };
        private final String[]  aLbl    = { "FIRE", "ALT", "SWITCH", "JUMP", "CROUCH" };
        private final int[]     aKey    = { DIRECT_FIRE, DIRECT_ALT, DIRECT_NEXT, DIRECT_JUMP, DIRECT_CROUCH };
        private final boolean[] aTap    = { false, false, true,  false, false }; // SWITCH = momentary
        private final boolean[] aAim    = { true,  true,  false, false, false }; // FIRE/ALT also drive look while held
        private final boolean[] aToggle = { false, false, false, false, true  }; // CROUCH toggles (matches the pad)
        private final boolean[] aPressed = new boolean[A_COUNT];
        private final int[]     aFinger  = new int[A_COUNT];
        private final float[]   aLastX   = new float[A_COUNT];
        private final float[]   aLastY   = new float[A_COUNT];
        private boolean crouchToggled = false;

        // Move stick + free-look state.
        private int   moveFinger = -1;
        private float leftBaseX, leftBaseY, lx, ly, thumbDX, thumbDY;
        private int   lookFinger = -1;
        private float lookLastX, lookLastY;

        // Menu navigation from the stick (edge + auto-repeat). 0=none,1=up,2=down,3=left,4=right.
        private int  menuNavDir = 0;
        private long menuNavNextMs;

        private boolean enabled = true;
        private boolean menuVisible = false;
        private boolean lastMenuVisible = false;
        private long    lastConfigReadMs, lastMenuReadMs, lastTouchMs;

        UnrealTouchOverlayViewV124(UnrealSDLActivity activity) {
            super(activity);
            this.activity = activity;
            setWillNotDraw(false);
            setFocusable(false);
            setFocusableInTouchMode(false);
            setClickable(true);
            if (android.os.Build.VERSION.SDK_INT >= 16) {
                setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
            }
            for (int i = 0; i < A_COUNT; ++i) aFinger[i] = -1;
            textPaint.setColor(0xFF000000);
            textPaint.setTextAlign(android.graphics.Paint.Align.CENTER);
            textPaint.setFakeBoldText(true);
            android.util.Log.i(TAG, "UNREAL_ANDROID_TOUCH_LCARS_V142 unified overlay ready (menu: stick navigates, FIRE confirms; FMV tap-to-skip; look gain " + LOOK_GAIN_V142 + ")");
            postDelayed(redrawRunnable, 66L);
        }

        private final Runnable redrawRunnable = new Runnable() {
            @Override public void run() {
                invalidate();
                postDelayed(this, 66L);
            }
        };

        private void refreshState() {
            long now = android.os.SystemClock.uptimeMillis();
            if (now - lastConfigReadMs > 900L) {
                lastConfigReadMs = now;
                enabled = activity.readTouchControlsEnabledV124();
            }
            if (now - lastMenuReadMs > 120L) {
                lastMenuReadMs = now;
                try {
                    // Strict "navigable menu" only (Menuing). The broad flag stays true in
                    // gameplay (KHG console state), which would break in-game controls + auto-fade.
                    menuVisible = nativeAndroidIsUiMenuV142();
                } catch (Throwable ignored) {
                    menuVisible = false;
                }
                if (menuVisible != lastMenuVisible) {
                    // Drop anything held across a menu<->game transition so no key sticks.
                    lastMenuVisible = menuVisible;
                    releaseAll();
                    roles.clear();
                }
            }
        }

        // ---------------------------------------------------------------- drawing

        @Override protected void onDraw(android.graphics.Canvas canvas) {
            super.onDraw(canvas);
            refreshState();
            if (!enabled || canvas == null) return;
            float w = getWidth(), h = getHeight();
            if (w <= 0 || h <= 0) return;

            // Auto-hide ~4s after the last touch in BOTH gameplay and menus (was gameplay-only, which left
            // the controls stuck on top of the in-game menu). A finger down keeps them up; a fresh tap
            // re-shows and acts immediately (the MENU button is hit-tested even while hidden).
            {
                long now = android.os.SystemClock.uptimeMillis();
                if (lastTouchMs == 0) lastTouchMs = now;
                boolean held = moveFinger != -1 || lookFinger != -1 || anyActionFinger();
                if (!held && now - lastTouchMs > 4000L) return;
            }
            drawGameplay(canvas, w, h, 209);
        }

        private void drawGameplay(android.graphics.Canvas canvas, float w, float h, int alpha) {
            // Move stick: orange ring at the (anchored or default) centre + gold thumb.
            float cx = (moveFinger != -1 ? leftBaseX : PAD_CX * w);
            float cy = (moveFinger != -1 ? leftBaseY : PAD_CY * h);
            float rpx = PAD_R * w;
            paint.setStyle(android.graphics.Paint.Style.STROKE);
            paint.setStrokeWidth(Math.max(3f, rpx * 0.09f));
            paint.setColor(withAlpha(LCARS_ORANGE, (int)(alpha * 0.85f)));
            canvas.drawCircle(cx, cy, rpx, paint);
            paint.setStyle(android.graphics.Paint.Style.FILL);
            paint.setColor(withAlpha(LCARS_GOLD, alpha));
            canvas.drawCircle(cx + thumbDX, cy + thumbDY, rpx * 0.45f, paint);

            for (int i = 0; i < A_COUNT; ++i) {
                drawPill(canvas, w, h, aCx[i], aCy[i], aHw[i], aHh[i],
                        aPressed[i] ? LCARS_HOPBUSH : aCol[i], aLbl[i], alpha);
            }

            // MENU (Escape) button, top-left; auto-hides with the rest of the controls.
            drawPill(canvas, w, h, MENU_CX, MENU_CY, MENU_HW, MENU_HH, LCARS_ICE, "MENU", alpha);
        }

        private void drawPill(android.graphics.Canvas canvas, float w, float h,
                              float cx, float cy, float hw, float hh, int color, String label, int alpha) {
            float l = (cx - hw) * w, t = (cy - hh) * h, r = (cx + hw) * w, b = (cy + hh) * h;
            rect.set(l, t, r, b);
            float rad = (b - t) * 0.5f;
            paint.setStyle(android.graphics.Paint.Style.FILL);
            paint.setColor(withAlpha(color, alpha));
            canvas.drawRoundRect(rect, rad, rad, paint);
            paint.setStyle(android.graphics.Paint.Style.STROKE);
            paint.setStrokeWidth(Math.max(2f, rad * 0.10f));
            paint.setColor(withAlpha(0xFFFFFFFF, alpha / 3));
            canvas.drawRoundRect(rect, rad, rad, paint);
            drawLabel(canvas, label, (l + r) * 0.5f, (t + b) * 0.5f, (r - l) - 2f * rad, (b - t), alpha);
        }

        private void drawLabel(android.graphics.Canvas canvas, String label, float cx, float cy,
                               float availW, float availH, int alpha) {
            if (label == null || label.length() == 0) return;
            float size = availH * 0.62f;
            textPaint.setTextSize(size);
            float tw = textPaint.measureText(label);
            if (tw > availW && tw > 0f) {
                size *= availW / tw;
                textPaint.setTextSize(size);
            }
            textPaint.setAlpha(Math.min(255, alpha + 40));
            android.graphics.Paint.FontMetrics fm = textPaint.getFontMetrics();
            float baseline = cy - (fm.ascent + fm.descent) * 0.5f;
            canvas.drawText(label, cx, baseline, textPaint);
        }

        private static int withAlpha(int color, int alpha) {
            if (alpha < 0) alpha = 0; else if (alpha > 255) alpha = 255;
            return (color & 0x00FFFFFF) | (alpha << 24);
        }

        // ---------------------------------------------------------------- input

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (event == null) return false;
            refreshState();
            if (!enabled) {
                releaseAll();
                roles.clear();
                return false; // controls off: do not intercept; let SDL handle the screen
            }
            // The MENU (Escape) button must work in EVERY state, including the intro/title cutscene where
            // the block below hands touches to SDL. Test it first so ESC always reaches the game.
            int menuAction = event.getActionMasked();
            if (menuAction == MotionEvent.ACTION_DOWN || menuAction == MotionEvent.ACTION_POINTER_DOWN) {
                int mi = event.getActionIndex();
                if (hitPill(event.getX(mi), event.getY(mi), getWidth(), getHeight(), MENU_CX, MENU_CY, MENU_HW, MENU_HH)) {
                    lastTouchMs = android.os.SystemClock.uptimeMillis();
                    sendMenuEscTap();
                    return true;
                }
            }
            // FMV/cutscene: release the touch so SDL sees SDL_FINGERDOWN and PollSkip skips it.
            try {
                if (nativeAndroidIsCutsceneV142()) {
                    releaseAll();
                    roles.clear();
                    return false;
                }
            } catch (Throwable ignored) {
            }

            int action = event.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
                int index = event.getActionIndex();
                return onPointerDown(event.getPointerId(index), event.getX(index), event.getY(index));
            }
            if (action == MotionEvent.ACTION_MOVE) {
                boolean consumed = false;
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    Integer role = roles.get(event.getPointerId(i));
                    if (role == null) continue;
                    onPointerMove(role.intValue(), event.getX(i), event.getY(i));
                    consumed = true;
                }
                return consumed;
            }
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
                int index = event.getActionIndex();
                int pid = event.getPointerId(index);
                Integer role = roles.get(pid);
                roles.remove(pid);
                if (role != null) {
                    releaseRole(role.intValue());
                    return true;
                }
                return false;
            }
            if (action == MotionEvent.ACTION_CANCEL) {
                releaseAll();
                roles.clear();
                return true;
            }
            return false;
        }

        private boolean onPointerDown(int pid, float x, float y) {
            int role = resolveDown(x, y);
            if (role == 0) return false; // empty area: pass through to SDL
            lastTouchMs = android.os.SystemClock.uptimeMillis();
            if (role == ROLE_MENU) {
                sendMenuEscTap(); // discrete ESC tap; not finger-tracked
                return true;
            }
            if (role >= ROLE_ACTION) {
                int i = role - ROLE_ACTION;
                if (applyActionDown(i, pid, x, y)) roles.put(pid, Integer.valueOf(role));
                return true;
            }
            if (role == ROLE_STICK) {
                moveFinger = pid;
                leftBaseX = x; leftBaseY = y;
                lx = ly = thumbDX = thumbDY = 0f;
                menuNavDir = 0;
                if (!menuVisible) sendNativeMotion();
                roles.put(pid, Integer.valueOf(role));
                return true;
            }
            // ROLE_LOOK
            lookFinger = pid;
            lookLastX = x; lookLastY = y;
            roles.put(pid, Integer.valueOf(role));
            return true;
        }

        private int resolveDown(float x, float y) {
            float w = getWidth(), h = getHeight();
            if (hitPill(x, y, w, h, MENU_CX, MENU_CY, MENU_HW, MENU_HH)) return ROLE_MENU;
            for (int i = 0; i < A_COUNT; ++i) {
                if (hitPill(x, y, w, h, aCx[i], aCy[i], aHw[i], aHh[i])) return ROLE_ACTION + i;
            }
            if (x < w * 0.5f && moveFinger == -1) return ROLE_STICK;
            return ROLE_LOOK;
        }

        private boolean applyActionDown(int i, int pid, float x, float y) {
            if (menuVisible) {
                // In menus only FIRE is live: it confirms (KEYCODE_BUTTON_A -> IK_Enter in UI,
                // which the native UI path turns into a discrete tap). Other pills are inert.
                if (i == A_FIRE) setButton(KeyEvent.KEYCODE_BUTTON_A, true);
                return false; // nothing finger-tracked in menus
            }
            if (aToggle[i]) {
                crouchToggled = !crouchToggled;
                aPressed[i] = crouchToggled;
                setDirectTouchButtonV136(aKey[i], crouchToggled);
                return false; // toggle is not finger-tracked
            }
            aFinger[i] = pid;
            aPressed[i] = true;
            aLastX[i] = x; aLastY[i] = y;
            setDirectTouchButtonV136(aKey[i], true);
            if (aTap[i]) setDirectTouchButtonV136(aKey[i], false); // momentary
            return true;
        }

        private void onPointerMove(int role, float x, float y) {
            if (role == ROLE_STICK) {
                updateStick(x, y);
            } else if (role == ROLE_LOOK) {
                if (!menuVisible) pushLook(x - lookLastX, y - lookLastY);
                lookLastX = x; lookLastY = y;
                lastTouchMs = android.os.SystemClock.uptimeMillis();
            } else if (role >= ROLE_ACTION) {
                int i = role - ROLE_ACTION;
                if (!menuVisible && aAim[i]) { // drag-to-aim on FIRE/ALT
                    pushLook(x - aLastX[i], y - aLastY[i]);
                    aLastX[i] = x; aLastY[i] = y;
                }
            }
        }

        private void updateStick(float x, float y) {
            float w = getWidth(), h = getHeight();
            float s = Math.min(w, h);
            float radius = Math.max(112f, s * 0.145f);
            float dx = x - leftBaseX, dy = y - leftBaseY;

            // Thumb visual (both modes).
            float rpx = PAD_R * w;
            float len = (float) Math.sqrt(dx * dx + dy * dy);
            if (len > rpx && len > 0f) { thumbDX = dx * rpx / len; thumbDY = dy * rpx / len; }
            else { thumbDX = dx; thumbDY = dy; }

            long now = android.os.SystemClock.uptimeMillis();
            if (menuVisible) {
                // Stick becomes a menu d-pad: edge-trigger on deflection, then auto-repeat.
                float nx = dx / radius, ny = dy / radius;
                int dir = 0;
                if (Math.abs(ny) >= Math.abs(nx)) {
                    if (ny <= -0.5f) dir = 1; else if (ny >= 0.5f) dir = 2;
                } else {
                    if (nx <= -0.5f) dir = 3; else if (nx >= 0.5f) dir = 4;
                }
                if (dir != 0) {
                    if (dir != menuNavDir) { menuNavDir = dir; sendMenuNav(dir); menuNavNextMs = now + 380L; }
                    else if (now >= menuNavNextMs) { sendMenuNav(dir); menuNavNextMs = now + 220L; }
                } else {
                    menuNavDir = 0;
                }
            } else {
                lx = analogValue(dx, radius, 0.075f, 0.85f);
                ly = analogValue(dy, radius, 0.075f, 0.85f);
                sendNativeMotion();
            }
            lastTouchMs = now;
        }

        private void sendMenuNav(int dir) {
            int kc = dir == 1 ? KeyEvent.KEYCODE_DPAD_UP
                   : dir == 2 ? KeyEvent.KEYCODE_DPAD_DOWN
                   : dir == 3 ? KeyEvent.KEYCODE_DPAD_LEFT
                   : KeyEvent.KEYCODE_DPAD_RIGHT;
            setButton(kc, true); // native UI path turns a DOWN into a discrete menu tap
        }

        // On-screen MENU button: route through the reliable native direct-touch path (same mechanism as
        // FIRE/JUMP), which injects a real Escape key press (Bindings[IK_Escape]=ShowMenu + CauseInputEvent).
        // This works in every state, unlike the Android KeyEvent/controller path which needs config bindings
        // loaded and a registered controller device.
        private void sendMenuEscTap() {
            try {
                setDirectTouchButtonV136(DIRECT_MENU, true);
                setDirectTouchButtonV136(DIRECT_MENU, false);
            } catch (Throwable ignored) {
            }
        }

        private void pushLook(float dxPx, float dyPx) {
            float rx = lookDelta(dxPx), ry = lookDelta(dyPx);
            if (rx != 0f || ry != 0f) sendTouchLookV129(rx, ry);
        }

        private static float lookDelta(float deltaPx) {
            // Relative-swipe look (UT99/Voyager principle): jitter filter + gain.
            if (Math.abs(deltaPx) < 0.25f) return 0f;
            return clampV124(deltaPx * LOOK_GAIN_V142, -1f, 1f);
        }

        private static float analogValue(float delta, float radius, float dead, float scale) {
            float v = clampV124(delta / radius, -1f, 1f);
            if (Math.abs(v) < dead) return 0f;
            if (v > 0f) v = (v - dead) / (1f - dead);
            else v = (v + dead) / (1f - dead);
            return clampV124(v * scale, -1f, 1f);
        }

        private boolean anyActionFinger() {
            for (int i = 0; i < A_COUNT; ++i) if (aFinger[i] != -1) return true;
            return false;
        }

        private void releaseRole(int role) {
            if (role == ROLE_STICK) {
                moveFinger = -1; lx = ly = thumbDX = thumbDY = 0f; menuNavDir = 0;
                if (!menuVisible) sendNativeMotion();
            } else if (role == ROLE_LOOK) {
                lookFinger = -1; if (!menuVisible) sendTouchLookV129(0f, 0f);
            } else if (role >= ROLE_ACTION) {
                int i = role - ROLE_ACTION;
                aFinger[i] = -1;
                aPressed[i] = false;
                if (!aTap[i]) setDirectTouchButtonV136(aKey[i], false);
            }
        }

        private void releaseAll() {
            if (moveFinger != -1 || lx != 0f || ly != 0f) {
                moveFinger = -1; lx = ly = thumbDX = thumbDY = 0f; sendNativeMotion();
            }
            menuNavDir = 0;
            if (lookFinger != -1) { lookFinger = -1; sendTouchLookV129(0f, 0f); }
            for (int i = 0; i < A_COUNT; ++i) {
                boolean wasDown = aFinger[i] != -1;
                aFinger[i] = -1;
                if (wasDown && !aTap[i] && !aToggle[i]) setDirectTouchButtonV136(aKey[i], false);
                if (!aToggle[i]) aPressed[i] = false;
            }
            if (crouchToggled) { setDirectTouchButtonV136(DIRECT_CROUCH, false); crouchToggled = false; }
            aPressed[A_CROUCH] = false;
        }

        // ---------------------------------------------------------------- hit tests

        private boolean hitPill(float x, float y, float w, float h, float cx, float cy, float hw, float hh) {
            // AABB with a small finger-friendly margin (Voyager IN_TouchInRect + slack).
            float mx = hw * 1.12f, my = hh * 1.12f;
            return x >= (cx - mx) * w && x <= (cx + mx) * w
                && y >= (cy - my) * h && y <= (cy + my) * h;
        }

        // ---------------------------------------------------------------- native bridge

        private void setDirectTouchButtonV136(int directKeyCode, boolean down) {
            // UNREAL_ANDROID_TOUCH_BUTTON_DIRECT_V136: native picks the friendly controller
            // binding or a safe PC fallback for these artificial gameplay keycodes.
            try {
                nativeAndroidControllerKey(
                        -136, 0, 0, directKeyCode, 0,
                        down ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, 0,
                        InputDevice.SOURCE_GAMEPAD, "UnrealTouchButtonDirectV136");
            } catch (Throwable ignored) {
            }
        }

        private void setButton(int keyCode, boolean down) {
            try {
                nativeAndroidControllerKey(
                        -124, 0, 0, keyCode, 0,
                        down ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, 0,
                        InputDevice.SOURCE_GAMEPAD, "UnrealTouchOverlay");
            } catch (Throwable ignored) {
            }
        }

        private void sendNativeMotion() {
            try {
                nativeAndroidControllerMotion(
                        -124, 0, 0, InputDevice.SOURCE_JOYSTICK, "UnrealTouchOverlay",
                        lx, ly, 0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f);
            } catch (Throwable ignored) {
            }
        }

        private void sendTouchLookV129(float x, float y) {
            try {
                nativeAndroidTouchLookV131(x, y);
            } catch (Throwable v131) {
                try {
                    nativeAndroidTouchLookV101(x, y);
                } catch (Throwable v101) {
                    try {
                        nativeAndroidTouchLookV124(x, y);
                    } catch (Throwable ignored) {
                    }
                }
            }
        }
    }

    private void scheduleImmersiveRefresh() {
        Handler handler = new Handler(Looper.getMainLooper());
        handler.postDelayed(this::hideSystemUi, 50);
        handler.postDelayed(this::hideSystemUi, 250);
        handler.postDelayed(this::hideSystemUi, 750);
    }

    private void hideSystemUi() {
        View decor = getWindow().getDecorView();
        decor.setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        );
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = decor.getWindowInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            decor.setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_FULLSCREEN |
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                    View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN |
                    View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION |
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            );
        }
    }
}
