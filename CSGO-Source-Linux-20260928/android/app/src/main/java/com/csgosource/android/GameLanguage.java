package com.csgosource.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/** Shared with the in-game language selector; engine flags take precedence. */
final class GameLanguage {
    static final String[] CODES = {"auto", "schinese", "english"};
    static final String[] LABELS = {"跟随系统 / System", "简体中文", "English"};

    static File preferenceFile(File gameRoot) {
        return new File(gameRoot, "csgo/local/cfg/language.txt");
    }

    static int index(String code) {
        for (int i = 0; i < CODES.length; ++i) if (CODES[i].equals(code)) return i;
        return 0;
    }

    static String read(File gameRoot) {
        try (FileInputStream input = new FileInputStream(preferenceFile(gameRoot))) {
            byte[] data = new byte[64];
            int length = input.read(data);
            if (length > 0) {
                String value = new String(data, 0, length, StandardCharsets.UTF_8).trim();
                return CODES[index(value)];
            }
        } catch (IOException | SecurityException ignored) { }
        return "auto";
    }

    static void write(File gameRoot, String code) throws IOException {
        if (!CODES[index(code)].equals(code)) throw new IllegalArgumentException("Unsupported language");
        File file = preferenceFile(gameRoot);
        File parent = file.getParentFile();
        if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("Cannot create language settings directory");
        File temporary = new File(parent, "language.txt.tmp");
        try {
            try (FileOutputStream output = new FileOutputStream(temporary)) {
                output.write((code + "\n").getBytes(StandardCharsets.UTF_8));
                output.getFD().sync();
            }
            if (!temporary.renameTo(file)) throw new IOException("Cannot save language setting");
        } finally {
            temporary.delete();
        }
    }

    static String resolve(String preference, Locale systemLocale) {
        if ("schinese".equals(preference) || "english".equals(preference)) return preference;
        return "zh".equals(systemLocale.getLanguage()) ? "schinese" : "english";
    }

    static String[] withLanguage(String[] arguments, String preference, Locale systemLocale) {
        if (arguments == null) arguments = new String[0];
        for (String argument : arguments) if ("-language".equalsIgnoreCase(argument)) return arguments.clone();
        String[] result = new String[arguments.length + 2];
        System.arraycopy(arguments, 0, result, 0, arguments.length);
        result[arguments.length] = "-language";
        result[arguments.length + 1] = resolve(preference, systemLocale);
        return result;
    }
}
