package com.csgosource.android;

import java.io.File;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.Locale;

public final class GameLanguageTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("csgo-language-").toFile();
        check("auto".equals(GameLanguage.read(root)), "missing preference uses system language");
        check("schinese".equals(GameLanguage.resolve("auto", Locale.SIMPLIFIED_CHINESE)), "Chinese system locale");
        check("english".equals(GameLanguage.resolve("auto", Locale.US)), "English system locale");
        check("english".equals(GameLanguage.resolve("english", Locale.CHINA)), "explicit English preference");
        GameLanguage.write(root, "schinese");
        check("schinese".equals(GameLanguage.read(root)), "saved preference survives a fresh read");
        GameLanguage.write(root, "english");
        check("english".equals(GameLanguage.read(root)), "atomic replacement of a saved preference");
        check(Arrays.equals(GameLanguage.withLanguage(new String[] {"+map", "de_dust2"}, GameLanguage.read(root), Locale.CHINA),
                new String[] {"+map", "de_dust2", "-language", "english"}), "preference reaches engine arguments");
        String[] explicit = {"-language", "russian", "+echo", "argument with spaces"};
        check(Arrays.equals(GameLanguage.withLanguage(explicit, "schinese", Locale.CHINA), explicit), "explicit CLI language wins");
        try {
            GameLanguage.write(root, "unsupported");
            throw new AssertionError("unsupported preference accepted");
        } catch (IllegalArgumentException expected) { }
        check("english".equals(GameLanguage.read(root)), "invalid selection preserves saved preference");
        Files.write(GameLanguage.preferenceFile(root).toPath(), "broken\n".getBytes(java.nio.charset.StandardCharsets.UTF_8));
        check("auto".equals(GameLanguage.read(root)), "invalid file safely falls back to system language");
        check(GameLanguage.withLanguage(null, "auto", Locale.CHINA).length == 2, "empty arguments gain a language");
        GameLanguage.preferenceFile(root).delete();
        new File(root, "csgo/local/cfg").delete();
        new File(root, "csgo/local").delete();
        new File(root, "csgo").delete();
        root.delete();
        System.out.println("Game language: persistence, replacement, locale fallback and argument precedence passed");
    }
}
