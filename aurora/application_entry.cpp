#include "shared_microg_worker.h"
#include "microg_watchdog.h"
#include "preview_text.h"
#include "embedded_gpu_transport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QPainterPath>
#include <QPluginLoader>
#include <QRawFont>

#include <clocale>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <dlfcn.h>

namespace {
// The runtime commits guest pages lazily from its SIGSEGV handler. GStreamer, when it scans plugins
// in-process (the first QCamera, e.g. for a WebView enumerating cameras), swaps in its own SIGSEGV
// handler and aborts on those expected faults, blaming whichever plugin it was loading.
void disableGStreamerSegtrap()
{
    void *gst = dlopen("libgstreamer-1.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!gst) return;
    using SetEnabled = void (*)(int);
    if (auto set = reinterpret_cast<SetEnabled>(dlsym(gst, "gst_segtrap_set_enabled"))) set(0);
}
}

namespace {
struct SharedMicrogPresentation {
    EmbeddedGpuTransport transport;
    std::atomic<bool> failed{false};
    std::atomic<bool> reported{false};

    static bool MEGLER_CB present(void *user, uint32_t texture, int width, int height)
    {
        SharedMicrogPresentation *self = static_cast<SharedMicrogPresentation *>(user);
        if (self->failed.load()) return false;
        uint64_t channel = 0, generation = 0;
        if (!shared_microg_gpu_frame_owner(&channel, &generation)) return true;
        QString error;
        const bool ok = EmbeddedGpuTransport::canExportDmaBuf(nullptr)
            ? self->transport.presentTexture(texture, width, height, &error, channel, generation)
            : self->transport.presentGbmTexture(texture, width, height, &error, channel, generation);
        if (ok) {
            if (!self->reported.exchange(true))
                qInfo("Shared microG frame path: GPU");
            return true;
        }
        self->failed.store(true);
        self->transport.closePeer();
        qWarning("Shared microG frame path: CPU; fallback reason: %s", qPrintable(error));
        return false;
    }
};
}

int runApplication(int argc, char **argv, bool reportWebViewAvailable);

#ifdef ACL_WITH_REPORT_WEBVIEW
void initReportWebView(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
    disableGStreamerSegtrap();
    if (argc > 1 && std::strcmp(argv[1], "--shared-microg-worker") == 0) {
        bool gpu = false;
        for (int i = 2; i < argc; ++i)
            if (std::strcmp(argv[i], "--gpu-canvas") == 0) gpu = true;
        QStringList paths = QCoreApplication::libraryPaths();
        const QString qtPlugins = QLibraryInfo::location(QLibraryInfo::PluginsPath);
        if (!paths.contains(qtPlugins)) paths.append(qtPlugins);
        bool available = false;
        for (const QString &path : paths) {
            QPluginLoader plugin(QDir(path).filePath(QStringLiteral("platforms/libqminimal.so")));
            if (plugin.load()) {
                available = true;
                break;
            }
        }
        if (!available) {
            std::fputs("shared microG: Qt minimal platform plugin unavailable\n", stderr);
            return 1;
        }
        char name[] = "shared-microg-worker";
        char platformOption[] = "-platform";
        char minimalPlatform[] = "minimal:enable_fonts";
        char waylandPlatform[] = "wayland";
        char *guiArgv[] = {name, platformOption, gpu ? waylandPlatform : minimalPlatform, nullptr};
        int guiArgc = 3;
        QGuiApplication gui(guiArgc, guiArgv);
        // Qt applies the user's locale; guest printf (e.g. Skia's SkSL "%f") must keep '.' as on Android.
        std::setlocale(LC_NUMERIC, "C");
        const QRawFont raw = QRawFont::fromFont(QFont());
        const QVector<quint32> glyphs = raw.glyphIndexesForString(QStringLiteral("IW"));
        if (!raw.isValid() || glyphs.size() != 2 || !glyphs[0] || !glyphs[1] ||
            glyphs[0] == glyphs[1] || raw.pathForGlyph(glyphs[0]).isEmpty() ||
            raw.pathForGlyph(glyphs[1]).isEmpty()) {
            std::fputs("shared microG: no usable Qt system font\n", stderr);
            return 1;
        }
        megler_gfx_host graphics = previewGraphics();
        SharedMicrogPresentation presentation;
        bool gpuTransport = false;
        if (gpu) {
            QString error;
            gpuTransport = presentation.transport.adoptInheritedSocket(5, false, &error);
            if (!gpuTransport)
                qWarning("Shared microG frame path: CPU; fallback reason: %s", qPrintable(error));
        } else {
            qInfo("Shared microG frame path: CPU; fallback reason: EGL worker unavailable");
        }
        SharedMicrogGpuCanvas *gpuCanvas = gpu
            ? createSharedMicrogGpuCanvas(&graphics, shared_microg_gpu_present_pixels,
                                         gpuTransport ? SharedMicrogPresentation::present : nullptr,
                                         &presentation) : nullptr;
        if (gpu && !gpuCanvas) {
            std::fputs("shared microG: native EGL Canvas unavailable\n", stderr);
            return 1;
        }
        const int status = shared_microg_worker_main(argc, argv, &graphics);
        destroySharedMicrogGpuCanvas(gpuCanvas);
        return status;
    }
    if (argc > 1 && std::strcmp(argv[1], "--microg-watchdog") == 0)
        return runMicrogWatchdog(argc, argv);
#ifdef ACL_WITH_REPORT_WEBVIEW
    const char *mode = argc > 1 ? argv[1] : "";
    const bool reportWebViewAvailable =
        std::strcmp(mode, "--preview") != 0 &&
        std::strcmp(mode, "--standalone") != 0 &&
        std::strcmp(mode, "--build-standalone") != 0 &&
        std::strcmp(mode, "--unpack-bundle") != 0 &&
        std::strcmp(mode, "--unpack-store-bundle") != 0 &&
        std::strcmp(mode, "--unpack-store-xapk") != 0;
    if (reportWebViewAvailable)
        initReportWebView(argc, argv);
    return runApplication(argc, argv, reportWebViewAvailable);
#else
    return runApplication(argc, argv, false);
#endif
}
