package com.csgosource.android;

import android.os.Bundle;
import android.app.ActivityManager;
import android.content.Context;
import android.widget.TextView;
import android.graphics.Insets;
import android.graphics.Rect;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;
import android.view.WindowInsets;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** The Java support classes are taken from the same pinned SDL3 checkout as libSDL3. */
public final class GameActivity extends SDLActivity {
    @Override protected SDLSurface createSDLSurface(Context context) {
        return new SDLSurface(context) {
            @Override public WindowInsets onApplyWindowInsets(View view, WindowInsets insets) {
                WindowInsets result = super.onApplyWindowInsets(view, insets);
                // Gesture recognition strips are usable pixels, not occlusions.
                // SDL's union otherwise adds large invisible margins on this device.
                Insets safe = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                SDLActivity.onNativeInsetsChanged(safe.left, safe.right, safe.top, safe.bottom);
                return result;
            }

            @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
                super.onSizeChanged(width, height, oldWidth, oldHeight);
                // Let edge controls receive swipes; Android retains its mandatory
                // gesture regions and applies its own exclusion-size limit.
                setSystemGestureExclusionRects(java.util.Collections.singletonList(new Rect(0, 0, width, height)));
            }
        };
    }

    static boolean isRunning(Context context) {
        ActivityManager manager = (ActivityManager)context.getSystemService(Context.ACTIVITY_SERVICE);
        java.util.List<ActivityManager.RunningAppProcessInfo> processes = manager.getRunningAppProcesses();
        if (processes != null) {
            for (ActivityManager.RunningAppProcessInfo process : processes) {
                if (process.processName.equals(context.getPackageName() + ":game")) return true;
            }
        }
        return false;
    }

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        requestHighestRefreshRate();
        boolean vulkan = getIntent().getBooleanExtra("vulkan_probe", false);
        if (!BuildConfig.DEBUG || mLayout == null || (!vulkan && !getIntent().getBooleanExtra("graphics_probe", false))) return;
        TextView label = new TextView(this);
        label.setText((vulkan ? "Vulkan 诊断" : "图形诊断") + "\n返回键退出");
        label.setTextColor(0xffdae8f5);
        label.setTextSize(14);
        label.setPadding(24, 120, 24, 16);
        mLayout.addView(label);
    }

    /** Without a window preference the system runs games at its default 60/90 Hz policy. */
    private void requestHighestRefreshRate() {
        Display display = getDisplay();
        if (display == null) return;
        Display.Mode best = display.getMode();
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == best.getPhysicalWidth() && mode.getPhysicalHeight() == best.getPhysicalHeight()
                    && mode.getRefreshRate() > best.getRefreshRate()) best = mode;
        }
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(attributes);
    }

    @Override protected String[] getLibraries() {
        return new String[] {"c++_shared", "SDL3", "dxvk_d3d9", "csgo_android"};
    }

    @Override protected String[] getArguments() {
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra("vulkan_probe", false)) {
            java.util.ArrayList<String> args = new java.util.ArrayList<>();
            args.add("--vulkan-probe");
            args.add("--probe-seconds");
            args.add(Integer.toString(getIntent().getIntExtra("probe_seconds", 15)));
            if (getIntent().getBooleanExtra("vulkan_validation", false)) args.add("--vulkan-validation");
            return args.toArray(new String[0]);
        }
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra("graphics_probe", false))
            return new String[] {"--graphics-probe", "--probe-seconds", Integer.toString(getIntent().getIntExtra("probe_seconds", 0))};
        String json = getIntent().getStringExtra("engine_args_json");
        if (json != null) {
            try {
                org.json.JSONArray values = new org.json.JSONArray(json);
                String[] args = new String[values.length()];
                for (int i = 0; i < args.length; ++i) args[i] = values.getString(i);
                return GameLanguage.withLanguage(args, GameLanguage.read(LauncherActivity.GAME_ROOT), java.util.Locale.getDefault());
            } catch (org.json.JSONException error) {
                throw new IllegalArgumentException("Invalid engine_args_json", error);
            }
        }
        String[] args = getIntent().getStringArrayExtra("engine_args");
        return GameLanguage.withLanguage(args, GameLanguage.read(LauncherActivity.GAME_ROOT), java.util.Locale.getDefault());
    }

    @Override protected void onDestroy() {
        super.onDestroy();
        // Source owns process-wide callbacks/TLS. A second launch needs a fresh
        // :game process; the launcher remains alive to display any saved error.
        if (isFinishing()) android.os.Process.killProcess(android.os.Process.myPid());
    }
}
