package com.csgosource.android;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Insets;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.Gravity;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;

/** Storage is checked before SDL and the engine libraries are loaded. */
public final class LauncherActivity extends Activity {
    static final File GAME_ROOT = new File("/storage/emulated/0/Games/CSGO");
    private TextView status;
    private Button permission, start, language;
    private boolean launching;
    private String lastShownError;

    private static String tr(String chinese, String english) {
        return "schinese".equals(GameLanguage.resolve(GameLanguage.read(GAME_ROOT), java.util.Locale.getDefault())) ? chinese : english;
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER_VERTICAL);
        int padding = Math.round(28 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding, padding, padding);
        layout.setBackgroundColor(0xff18202a);
        layout.setOnApplyWindowInsetsListener((view, insets) -> {
            Insets safe = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            view.setPadding(padding + safe.left, padding + safe.top, padding + safe.right, padding + safe.bottom);
            return insets;
        });
        TextView title = new TextView(this);
        title.setText("CSGO Android");
        title.setTextSize(30);
        title.setTextColor(Color.WHITE);
        layout.addView(title);
        TextView description = new TextView(this);
        description.setText(tr("本地对战与训练。\n\n游戏资源目录：\n", "Offline matches and practice.\n\nGame files:\n") + GAME_ROOT);
        description.setTextSize(16);
        description.setTextColor(0xffc9d4df);
        description.setPadding(0, padding, 0, padding);
        layout.addView(description);
        status = new TextView(this);
        status.setTextSize(16);
        status.setTextColor(0xffc9d4df);
        layout.addView(status);
        language = new Button(this);
        language.setOnClickListener(view -> showLanguageDialog());
        layout.addView(language);
        Button copyError = new Button(this);
        copyError.setText(tr("复制报错", "Copy error"));
        copyError.setOnClickListener(view -> copyError(readError()));
        layout.addView(copyError);
        permission = new Button(this);
        permission.setText(tr("授予文件访问权限", "Grant file access"));
        permission.setOnClickListener(view -> {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (ActivityNotFoundException unavailable) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        });
        layout.addView(permission);
        Button refresh = new Button(this);
        refresh.setText(tr("重新检查资源", "Check game files"));
        refresh.setOnClickListener(view -> refresh());
        layout.addView(refresh);
        start = new Button(this);
        start.setText(tr("启动游戏", "Start game"));
        start.setOnClickListener(view -> launch());
        layout.addView(start);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(layout);
        setContentView(scroll);
    }

    @Override protected void onResume() {
        super.onResume();
        launching = false;
        refresh();
        if (start.isEnabled() && getIntent().getBooleanExtra("auto_start", false)) {
            getIntent().removeExtra("auto_start");
            launch();
            return;
        }
        String error = readError();
        if (error != null && !error.equals(lastShownError)) {
            lastShownError = error;
            new AlertDialog.Builder(this).setTitle(tr("启动失败", "Startup failed")).setMessage(error)
                    .setPositiveButton(tr("复制报错", "Copy error"), (dialog, which) -> copyError(error))
                    .setNegativeButton(tr("关闭", "Close"), null).show();
        }
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    private void refresh() {
        if (BuildConfig.DEBUG && getIntent().getBooleanExtra("vulkan_probe", false)) {
            permission.setVisibility(android.view.View.GONE);
            status.setText(tr("Vulkan 诊断已就绪，无需游戏资源。", "Vulkan diagnostics are ready. Game files are not required."));
            start.setEnabled(true);
            start.setText(GameActivity.isRunning(this) ? tr("返回诊断", "Return to diagnostics") : tr("启动诊断", "Start diagnostics"));
            return;
        }
        boolean allowed = Environment.isExternalStorageManager();
        language.setEnabled(allowed);
        language.setText(tr("游戏语言：", "Game language: ") + GameLanguage.LABELS[GameLanguage.index(GameLanguage.read(GAME_ROOT))]);
        permission.setVisibility(allowed ? android.view.View.GONE : android.view.View.VISIBLE);
        String error = allowed ? resourceError() : tr("需要允许访问游戏目录。", "Allow access to the game folder.");
        status.setText(error == null ? tr("游戏目录已就绪。", "Game files are ready.") : error);
        start.setEnabled(error == null);
        start.setText(GameActivity.isRunning(this) ? tr("返回游戏", "Return to game") : tr("启动游戏", "Start game"));
    }

    static String resourceError() {
        for (String path : new String[] {"csgo/gameinfo.txt", "csgo/pak01_dir.vpk", "csgo/maps/de_dust2.bsp"}) {
            try (FileInputStream stream = new FileInputStream(new File(GAME_ROOT, path))) {
                if (stream.read() < 0) return tr("文件为空：", "Empty file: ") + path;
            } catch (IOException | SecurityException error) {
                return tr("无法读取：", "Cannot read: ") + path
                        + tr("\n请放入完整游戏资源后重新检查。", "\nCopy the complete game files, then check again.");
            }
        }
        if (!new File(GAME_ROOT, "platform").isDirectory()) return tr("缺少 platform 资源目录。", "The platform folder is missing.");
        for (String path : new String[] {"logs", "cache", "csgo/local"}) {
            File directory = new File(GAME_ROOT, path);
            try {
                if (!directory.isDirectory() && !directory.mkdirs()) return tr("无法创建：", "Cannot create: ") + directory;
                File probe = File.createTempFile(".access-", ".tmp", directory);
                if (!probe.delete()) return tr("无法清理写入检查文件：", "Cannot remove the write test file: ") + probe;
            } catch (IOException | SecurityException error) {
                return tr("目录不可写：", "Folder is not writable: ") + directory;
            }
        }
        return null;
    }

    private void showLanguageDialog() {
        new AlertDialog.Builder(this).setTitle(tr("游戏语言", "Game language"))
                .setSingleChoiceItems(GameLanguage.LABELS, GameLanguage.index(GameLanguage.read(GAME_ROOT)), (dialog, which) -> {
                    try {
                        GameLanguage.write(GAME_ROOT, GameLanguage.CODES[which]);
                        android.widget.Toast.makeText(this, tr("已保存，重新启动游戏后生效", "Saved. Restart the game to apply."),
                                android.widget.Toast.LENGTH_LONG).show();
                        recreate();
                    } catch (IOException | SecurityException error) {
                        new AlertDialog.Builder(this).setTitle("保存失败 / Could not save")
                                .setMessage(error.getMessage()).setPositiveButton("确定 / OK", null).show();
                    }
                    dialog.dismiss();
                }).setNegativeButton("取消 / Cancel", null).show();
    }

    private void launch() {
        if (launching) return;
        refresh();
        if (!start.isEnabled()) return;
        launching = true;
        if (!GameActivity.isRunning(this)) {
            // Returning to a running game must preserve its report.
            new File(GAME_ROOT, "logs/error.txt").delete();
            new File(getFilesDir(), "error.txt").delete();
            getPreferences(MODE_PRIVATE).edit().putLong("last_attempt", System.currentTimeMillis()).apply();
            lastShownError = null;
        }
        start.setEnabled(false);
        Intent intent = new Intent(this, GameActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT);
        if (BuildConfig.DEBUG) {
            intent.putExtra("graphics_probe", getIntent().getBooleanExtra("graphics_probe", false));
            intent.putExtra("vulkan_probe", getIntent().getBooleanExtra("vulkan_probe", false));
            intent.putExtra("vulkan_validation", getIntent().getBooleanExtra("vulkan_validation", false));
            intent.putExtra("probe_seconds", Math.max(0, Math.min(600, getIntent().getIntExtra("probe_seconds", 0))));
        }
        intent.putExtra("engine_args", getIntent().getStringArrayExtra("engine_args"));
        intent.putExtra("engine_args_json", getIntent().getStringExtra("engine_args_json"));
        startActivity(intent);
    }

    private String readError() {
        for (File report : new File[] {new File(getFilesDir(), "error.txt"), new File(GAME_ROOT, "logs/error.txt")}) {
            try {
                if (report.isFile()) {
                    byte[] bytes = new byte[65536];
                    try (FileInputStream input = new FileInputStream(report)) {
                        int count = input.read(bytes);
                        if (count > 0) return new String(bytes, 0, count, StandardCharsets.UTF_8);
                    }
                }
            } catch (IOException | SecurityException ignored) { }
        }
        long attempt = getPreferences(MODE_PRIVATE).getLong("last_attempt", 0);
        if (attempt > 0 && !GameActivity.isRunning(this)) {
            ActivityManager manager = (ActivityManager)getSystemService(ACTIVITY_SERVICE);
            for (ApplicationExitInfo exit : manager.getHistoricalProcessExitReasons(getPackageName(), 0, 16)) {
                if (exit.getTimestamp() < attempt || !(getPackageName() + ":game").equals(exit.getProcessName())) continue;
                int reason = exit.getReason();
                if (reason == ApplicationExitInfo.REASON_CRASH || reason == ApplicationExitInfo.REASON_CRASH_NATIVE
                        || reason == ApplicationExitInfo.REASON_ANR || reason == ApplicationExitInfo.REASON_INITIALIZATION_FAILURE) {
                    return tr("游戏进程异常退出。\npid=", "The game exited unexpectedly.\npid=") + exit.getPid() + "; reason=" + reason
                            + "; status=" + exit.getStatus() + "\n" + exit.getDescription();
                }
            }
        }
        return null;
    }

    private void copyError(String error) {
        String text = "CSGO Android " + BuildConfig.BUILD_TYPE + " / " + BuildConfig.BUILD_ID + "\n"
                + android.os.Build.MODEL + " / Android "
                + android.os.Build.VERSION.RELEASE + "\n" + GAME_ROOT + "\n\n"
                + (error == null ? status.getText().toString() : error);
        ((ClipboardManager)getSystemService(CLIPBOARD_SERVICE)).setPrimaryClip(ClipData.newPlainText(tr("CSGO 报错", "CSGO error"), text));
        android.widget.Toast.makeText(this, tr("报错已复制", "Error copied"), android.widget.Toast.LENGTH_SHORT).show();
    }
}
