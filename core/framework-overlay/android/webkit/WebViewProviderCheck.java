package android.webkit;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Structural preflight only; this does not authenticate a provider or establish Chromium sandboxing. */
final class WebViewProviderCheck {
    private WebViewProviderCheck() {}

    static void check(File apk, String abi, String library) throws IOException {
        int elfClass, machine;
        switch (abi) {
            case "arm64-v8a": elfClass = 2; machine = 183; break;
            case "armeabi-v7a": elfClass = 1; machine = 40; break;
            case "x86_64": elfClass = 2; machine = 62; break;
            case "x86": elfClass = 1; machine = 3; break;
            default: throw new IOException("unsupported WebView runtime ABI: " + abi);
        }
        if (library == null || !library.startsWith("lib") || !library.endsWith(".so") ||
                library.indexOf('/') >= 0 || library.indexOf('\\') >= 0) {
            throw new IOException("invalid com.android.webview.WebViewLibrary metadata: " + library);
        }
        try (ZipFile zip = new ZipFile(apk)) {
            byte[] dex = header(zip, "classes.dex", 8);
            if (dex[0] != 'd' || dex[1] != 'e' || dex[2] != 'x' || dex[3] != '\n' ||
                    dex[4] < '0' || dex[4] > '9' || dex[5] < '0' || dex[5] > '9' ||
                    dex[6] < '0' || dex[6] > '9' || dex[7] != 0) {
                throw new IOException("WebView classes.dex is not DEX bytecode; an odex/vdex-only image is unsupported");
            }
            String path = "lib/" + abi + "/" + library;
            byte[] elf = header(zip, path, 20);
            int actualMachine = (elf[18] & 255) | ((elf[19] & 255) << 8);
            if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' ||
                    elf[4] != elfClass || elf[5] != 1 || elf[6] != 1 ||
                    elf[16] != 3 || elf[17] != 0 || actualMachine != machine) {
                throw new IOException("WebView " + path + " is not a little-endian shared ELF for " + abi +
                        " (class=" + (elf[4] & 255) + ", machine=" + actualMachine + ")");
            }
        }
    }

    private static byte[] header(ZipFile zip, String path, int size) throws IOException {
        ZipEntry entry = zip.getEntry(path);
        if (entry == null || entry.isDirectory()) {
            throw new IOException("WebView APK is missing " + path + "; a standalone provider APK is required, not a split");
        }
        byte[] bytes = new byte[size];
        try (InputStream in = zip.getInputStream(entry)) {
            int offset = 0, count;
            while (offset < size && (count = in.read(bytes, offset, size - offset)) > 0) offset += count;
            if (offset != size) throw new IOException("WebView APK has a truncated " + path);
        }
        return bytes;
    }
}
