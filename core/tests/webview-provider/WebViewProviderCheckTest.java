package android.webkit;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

public final class WebViewProviderCheckTest {
    private static final String LIBRARY = "libwebviewchromium.so";

    private static File apk(byte[] dex, String abi, byte[] elf) throws IOException {
        File file = File.createTempFile("webview-provider-", ".apk");
        try (ZipOutputStream zip = new ZipOutputStream(Files.newOutputStream(file.toPath()))) {
            if (dex != null) {
                zip.putNextEntry(new ZipEntry("classes.dex"));
                zip.write(dex);
                zip.closeEntry();
            }
            if (elf != null) {
                zip.putNextEntry(new ZipEntry("lib/" + abi + "/" + LIBRARY));
                zip.write(elf);
                zip.closeEntry();
            }
        }
        return file;
    }

    private static byte[] elf(int elfClass, int machine) {
        byte[] bytes = new byte[64];
        bytes[0] = 0x7f; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
        bytes[4] = (byte) elfClass; bytes[5] = 1; bytes[6] = 1; bytes[16] = 3;
        bytes[18] = (byte) machine; bytes[19] = (byte) (machine >> 8);
        return bytes;
    }

    private static void rejected(File apk, String abi, String library, String reason) throws IOException {
        try {
            WebViewProviderCheck.check(apk, abi, library);
            throw new AssertionError("accepted invalid provider: " + reason);
        } catch (IOException expected) {
            if (!expected.getMessage().contains(reason)) throw new AssertionError(expected);
        } finally {
            Files.delete(apk.toPath());
        }
    }

    public static void main(String[] args) throws Exception {
        byte[] dex = new byte[] { 'd', 'e', 'x', '\n', '0', '3', '5', 0 };
        String[] abis = { "arm64-v8a", "armeabi-v7a", "x86_64", "x86" };
        int[] classes = { 2, 1, 2, 1 }, machines = { 183, 40, 62, 3 };
        for (int i = 0; i < abis.length; i++) {
            File valid = apk(dex, abis[i], elf(classes[i], machines[i]));
            try {
                WebViewProviderCheck.check(valid, abis[i], LIBRARY);
            } finally {
                Files.delete(valid.toPath());
            }
        }
        rejected(apk(null, "arm64-v8a", elf(2, 183)), "arm64-v8a", LIBRARY, "missing classes.dex");
        rejected(apk(new byte[8], "arm64-v8a", elf(2, 183)), "arm64-v8a", LIBRARY, "not DEX");
        rejected(apk(new byte[2], "arm64-v8a", elf(2, 183)), "arm64-v8a", LIBRARY, "truncated classes.dex");
        rejected(apk(dex, "armeabi-v7a", elf(1, 40)), "arm64-v8a", LIBRARY, "missing lib/arm64-v8a/");
        rejected(apk(dex, "arm64-v8a", elf(1, 40)), "arm64-v8a", LIBRARY, "not a little-endian shared ELF");
        rejected(apk(dex, "arm64-v8a", new byte[4]), "arm64-v8a", LIBRARY, "truncated lib/");
        byte[] wrongEndian = elf(2, 183);
        wrongEndian[5] = 2;
        rejected(apk(dex, "arm64-v8a", wrongEndian), "arm64-v8a", LIBRARY, "not a little-endian shared ELF");
        rejected(apk(dex, "arm64-v8a", elf(2, 183)), "arm64-v8a", "../" + LIBRARY, "invalid com.android");
        rejected(apk(dex, "arm64-v8a", elf(2, 183)), "arm64-v8a", null, "invalid com.android");
        File corrupt = File.createTempFile("webview-corrupt-", ".apk");
        Files.write(corrupt.toPath(), new byte[] { 1, 2, 3 });
        rejected(corrupt, "arm64-v8a", LIBRARY, "zip");
        System.out.println("PASS: WebView provider ABI/DEX/ELF preflight (no Chromium executed)");
    }
}
