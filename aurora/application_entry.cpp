#include "shared_microg_worker.h"
#include "microg_watchdog.h"
#include "preview_text.h"

#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QPainterPath>
#include <QPluginLoader>
#include <QRawFont>

#include <cstdio>
#include <cstring>

int runApplication(int argc, char **argv, bool reportWebViewAvailable);

#ifdef ACL_WITH_REPORT_WEBVIEW
void initReportWebView(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "--shared-microg-worker") == 0) {
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
        char platform[] = "minimal:enable_fonts";
        char *guiArgv[] = {name, platformOption, platform, nullptr};
        int guiArgc = 3;
        QGuiApplication gui(guiArgc, guiArgv);
        const QRawFont raw = QRawFont::fromFont(QFont());
        const QVector<quint32> glyphs = raw.glyphIndexesForString(QStringLiteral("IW"));
        if (!raw.isValid() || glyphs.size() != 2 || !glyphs[0] || !glyphs[1] ||
            glyphs[0] == glyphs[1] || raw.pathForGlyph(glyphs[0]).isEmpty() ||
            raw.pathForGlyph(glyphs[1]).isEmpty()) {
            std::fputs("shared microG: no usable Qt system font\n", stderr);
            return 1;
        }
        const megler_gfx_host graphics = previewGraphics();
        return shared_microg_worker_main(argc, argv, &graphics);
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
