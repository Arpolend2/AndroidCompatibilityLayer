#include "../preview.h"
#include "../preview_orientation.h"
#include "../audio_output.h"
#include "../camera_host.h"
#include "../bluetooth_host.h"
#include "../install_request_host.h"
#include "../picker_host.h"
#include "../preview_keyboard.h"
#include "../preview_text.h"
#include "../sensor_host.h"
#include "../location_host.h"
#include "../runtime_settings.h"
#include "../log_channel.h"
#include "../../core/src/apk/mg_apk.h"

#include <QGuiApplication>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QThread>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

static bool codecConfigured;
static std::vector<int> keys;
static megler_run_config runConfig;
static int stops;
static QByteArray localeSet;
static uint32_t themeAccent;
static bool themeDark;
static double fixedLatitude;
static double fixedLongitude;
static int surfaceCalls;
static int surfaceRotation;
static int surfaceWidth;
static int surfaceHeight;
static bool cameraDirectoryAlias;
static QByteArray cameraAliasPackage;
static bool cameraStorageRequested;
static bool cameraMetadataValid = true;
static int cameraTargetSdk = 28;
static int cameraMinSdk;
static int cameraWriteMaxSdk;
static int runCalls;

class TestSurface : public PreviewSurface
{
public:
    using PreviewSurface::mousePressEvent;
    using PreviewSurface::mouseReleaseEvent;
};

extern "C" bool megler_init(const megler_config *config)
{
    if (config->log) config->log(0, "core integration test", config->log_user);
    return true;
}
extern "C" bool mg_apk_read_info(const char *, mg_apk_info *info)
{
    if (!cameraMetadataValid) return false;
    std::memset(info, 0, sizeof(*info));
    std::strcpy(info->package, "org.example.app");
    info->target_sdk = cameraTargetSdk;
    info->min_sdk = cameraMinSdk;
    if (cameraStorageRequested) {
        info->npermissions = 2;
        std::strcpy(info->permissions[0], "android.permission.CAMERA");
        std::strcpy(info->permissions[1], "android.permission.WRITE_EXTERNAL_STORAGE");
        info->permission_max_sdk[1] = cameraWriteMaxSdk;
    }
    return true;
}
extern "C" void mg_apk_info_free(mg_apk_info *) {}
extern "C" bool megler_set_camera_storage_alias(const char *package, bool enabled)
{
    cameraDirectoryAlias = enabled;
    cameraAliasPackage = enabled && package ? QByteArray(package) : QByteArray();
    return true;
}
extern "C" bool mg_apk_gms_name(const char *) { return false; }
extern "C" bool mg_apk_is_microg(const char *) { return false; }
extern "C" bool mg_java_set_shared_microg_client(int, int, int, const char *) { return false; }
extern "C" void mg_java_close_shared_microg_client() {}
extern "C" bool megler_apk_describe_activities(const char *, const char *, const char *) { return false; }
extern "C" bool megler_background_work_active() { return false; }
extern "C" void megler_set_paused(bool) {}
int takeSharedMicrogStream(QDBusUnixFileDescriptor *, QString *error)
{
    if (error) *error = QStringLiteral("Shared microG is not available in this test");
    return -1;
}
extern "C" void megler_set_locale(const char *locale) { localeSet = locale; }
extern "C" void megler_set_theme(uint32_t accent, bool dark) { themeAccent = accent; themeDark = dark; }
extern "C" bool megler_run(const megler_run_config *config)
{
    ++runCalls;
    runConfig = *config;
    const megler_codec_host &codec = config->codec;
    codecConfigured = codec.supports && codec.open && codec.push && codec.pull_sound &&
                      codec.flush && codec.drain && codec.close &&
                      codec.supports(codec.user, MEGLER_CODEC_MP3) &&
                      !codec.supports(codec.user, MEGLER_CODEC_AAC) &&
                      !codec.supports(codec.user, MEGLER_CODEC_H264) &&
                      config->media.open && config->media.read_audio &&
                      config->media.seek && config->media.close && !config->media.read_video;
    return true;
}
extern "C" const char *megler_last_error() { return ""; }
extern "C" void megler_touch(int, int, float, float) {}
extern "C" void megler_key(int code, bool down) { keys.push_back(down ? code : -code); }
extern "C" void megler_set_surface(int width, int height, int, int rotation)
{
    ++surfaceCalls;
    surfaceWidth = width;
    surfaceHeight = height;
    surfaceRotation = rotation;
}
extern "C" void megler_stop() { ++stops; }
megler_gfx_host previewGraphics() { return {}; }
megler_camera_host auroraCameraHost() { return {}; }
megler_bluetooth_host auroraBluetoothHost() { return {}; }
megler_sensor_host auroraSensorHost() { return {}; }
megler_location_host auroraLocationHost() { return {}; }
bool auroraLocationSetFixedLocation(double lat, double lon, float) { fixedLatitude = lat; fixedLongitude = lon; return true; }
void auroraLocationUseDeviceLocation() {}
megler_audio_host AuroraAudioOutput::callbacks() { return {}; }
megler_system_host AuroraPickerHost::host() { return {}; }
void AuroraPickerHost::stop() {}
megler_system_host InstallRequestHost::host() { return {}; }
QString InstallRequestHost::appsDir() const { return {}; }
megler_keyboard_host PreviewKeyboard::host() { return {}; }
void PreviewKeyboard::start() {}
void PreviewKeyboard::stop() {}
void PreviewKeyboard::hideKeyboard() {}
bool PreviewKeyboard::back() { return false; }
void PreviewKeyboard::setSurface(QQuickItem *, const QSize &) {}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AndroidCompatibilityLayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PreviewRuntime"));
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    qputenv("HOME", directory.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
    if (!QDir().mkpath(directory.path() + QStringLiteral("/config"))) return 1;
    QFile userDirs(directory.path() + QStringLiteral("/config/user-dirs.dirs"));
    if (!userDirs.open(QIODevice::WriteOnly)) return 1;
    userDirs.write("XDG_PICTURES_DIR=\"" + directory.path().toUtf8() + "/Pictures\"\n");
    userDirs.close();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    if (!RuntimeSettings::save(QStringLiteral("runtime/renderCap"), 960) ||
        !RuntimeSettings::save(QStringLiteral("runtime/locale"), QStringLiteral("ru-RU")) ||
        !RuntimeSettings::save(QStringLiteral("library/apps/org.example.app/edgeOnlyBack"), false)) return 1;
    LogChannel log;
    if (!log.available()) return 1;
    qputenv("ANDROID_COMPATIBILITY_LAYER_CORE_LOG_SOCKET", log.path().toLocal8Bit());
    {
        PreviewPlayer systemPlayer(QStringLiteral("."), QStringLiteral("."));
        systemPlayer.setSystemTheme(true, QColor(0x12, 0x34, 0x56));
        systemPlayer.start();
        if (!themeDark || localeSet != "ru-RU") return 1;
    }
    if (!RuntimeSettings::save(QStringLiteral("runtime/theme"), QStringLiteral("dark"))) return 1;
    {
        PreviewPlayer darkPlayer(QStringLiteral("."), QStringLiteral("."));
        darkPlayer.setSystemTheme(false, QColor(0x12, 0x34, 0x56));
        darkPlayer.start();
        if (!themeDark) return 1;
    }
    {
        PreviewPlayer standalone(QStringLiteral("."), QStringLiteral("."));
        standalone.setCloseOnExit(true);
        int finished = 0;
        QObject::connect(&standalone, &PreviewPlayer::finished, [&]() { ++finished; });
        standalone.start();
        runConfig.on_exit(runConfig.user, 0, "");
        app.processEvents();
        if (finished != 1) {
            std::fprintf(stderr, "Standalone guest exit did not close the preview\n");
            return 1;
        }
    }
    {
        PreviewPlayer preview(QStringLiteral("."), QStringLiteral("."));
        int finished = 0;
        QObject::connect(&preview, &PreviewPlayer::finished, [&]() { ++finished; });
        preview.start();
        runConfig.on_exit(runConfig.user, 0, "");
        app.processEvents();
        if (finished != 0 || !preview.status().contains(QStringLiteral("ended"))) return 1;
    }
    {
        PreviewPlayer worker(QStringLiteral("."), QStringLiteral("."));
        worker.setCpuPresentationOnly(true);
        worker.start();
        if (runConfig.display.orientation || runConfig.display.user) {
            std::fprintf(stderr, "CPU-only embedded worker must not register a window orientation callback\n");
            return 1;
        }
    }
    PreviewPlayer player(QStringLiteral("."), QStringLiteral("."));
    const Qt::ScreenOrientation directions[] = { Qt::PortraitOrientation, Qt::LandscapeOrientation,
        Qt::InvertedPortraitOrientation, Qt::InvertedLandscapeOrientation };
    for (int n = 0; n < 4; ++n)
        for (int a = 0; a < 4; ++a)
            if (previewDisplayRotation(directions[n], directions[a]) != (a - n + 4) % 4) return 1;
    if (previewDisplayRotation(Qt::PrimaryOrientation, Qt::LandscapeOrientation) != -1 ||
        previewDisplayRotation(Qt::PortraitOrientation, Qt::PrimaryOrientation) != -1) return 1;
    const Qt::ScreenOrientation waylandNative = previewNativeOrientation(Qt::PrimaryOrientation, true);
    for (int a = 0; a < 4; ++a)
        if (previewDisplayRotation(waylandNative, directions[a]) != a) return 1;
    if (previewNativeOrientation(Qt::PrimaryOrientation, false) != Qt::PrimaryOrientation ||
        previewNativeOrientation(Qt::LandscapeOrientation, true) != Qt::LandscapeOrientation ||
        previewDisplayRotation(waylandNative, Qt::PrimaryOrientation) != -1) return 1;
    player.setStandaloneSettings(QStringLiteral("de-DE"), QStringLiteral("light"), QStringLiteral("55.5,37.5"));
    player.setSystemTheme(true, QColor(0x12, 0x34, 0x56));
    player.setViewport(1080, 2340, 1);
    player.setDisplayRotation(1);
    player.start();
    app.processEvents();
    if (localeSet != "de-DE" || themeDark || themeAccent != 0xff123456 ||
        fixedLatitude != 55.5 || fixedLongitude != 37.5 ||
        runConfig.width != 443 || runConfig.height != 960 || runConfig.density_dpi != 148 ||
        runConfig.rotation != 1) {
        std::fprintf(stderr, "Runtime settings were not passed to Megler\n");
        return 1;
    }
    if (!runConfig.display.orientation || runConfig.display.user != &player) {
        std::fprintf(stderr, "Android orientation callback was not installed\n");
        return 1;
    }
    bool orientationOnGuiThread = false;
    QObject::connect(&player, &PreviewPlayer::orientationMaskChanged, [&]() {
        orientationOnGuiThread = QThread::currentThread() == app.thread();
    });
    std::thread orientationThread([&]() {
        runConfig.display.orientation(runConfig.display.user, MEGLER_ORIENTATION_LANDSCAPE);
    });
    orientationThread.join();
    if (player.orientationMask() != -1) return 1;
    app.processEvents();
    if (player.orientationMask() != int(Qt::LandscapeOrientation) || !orientationOnGuiThread) return 1;
    const int allOrientations = int(Qt::PortraitOrientation | Qt::LandscapeOrientation |
                                    Qt::InvertedPortraitOrientation | Qt::InvertedLandscapeOrientation);
    const int portraitOrientations = int(Qt::PortraitOrientation | Qt::InvertedPortraitOrientation);
    const int landscapeOrientations = int(Qt::LandscapeOrientation | Qt::InvertedLandscapeOrientation);
    const auto requestOrientation = [&](int requested, int expected) {
        runConfig.display.orientation(runConfig.display.user, requested);
        app.processEvents();
        return player.orientationMask() == expected;
    };
    const struct { int requested; int expected; } orientationCases[] = {
        { MEGLER_ORIENTATION_PORTRAIT, int(Qt::PortraitOrientation) },
        { MEGLER_ORIENTATION_REVERSE_PORTRAIT, int(Qt::InvertedPortraitOrientation) },
        { MEGLER_ORIENTATION_REVERSE_LANDSCAPE, int(Qt::InvertedLandscapeOrientation) },
        { MEGLER_ORIENTATION_SENSOR, allOrientations },
        { MEGLER_ORIENTATION_FULL_SENSOR, allOrientations },
        { MEGLER_ORIENTATION_SENSOR_LANDSCAPE, landscapeOrientations },
        { MEGLER_ORIENTATION_USER_LANDSCAPE, landscapeOrientations },
        { MEGLER_ORIENTATION_SENSOR_PORTRAIT, portraitOrientations },
        { MEGLER_ORIENTATION_USER_PORTRAIT, portraitOrientations },
        { MEGLER_ORIENTATION_USER, -1 },
        { MEGLER_ORIENTATION_FULL_USER, -1 },
        { MEGLER_ORIENTATION_UNSPECIFIED, -1 }
    };
    for (const auto &orientation : orientationCases)
        if (!requestOrientation(orientation.requested, orientation.expected)) return 1;
    player.setScreenOrientations(int(Qt::LandscapeOrientation), int(Qt::InvertedPortraitOrientation));
    if (!requestOrientation(MEGLER_ORIENTATION_NOSENSOR, int(Qt::LandscapeOrientation)) ||
        !requestOrientation(MEGLER_ORIENTATION_LOCKED, int(Qt::InvertedPortraitOrientation)) ||
        !requestOrientation(MEGLER_ORIENTATION_BEHIND, int(Qt::InvertedPortraitOrientation)) ||
        !requestOrientation(99, int(Qt::InvertedPortraitOrientation))) return 1;
    const int previousSurfaceCalls = surfaceCalls;
    player.setDisplayRotation(3);
    if (surfaceCalls != previousSurfaceCalls + 1 || surfaceWidth != 443 ||
        surfaceHeight != 960 || surfaceRotation != 3) return 1;
    player.setDisplayRotation(3);
    player.setDisplayRotation(4);
    if (surfaceCalls != previousSurfaceCalls + 1 || player.displayRotation() != 3) return 1;
    if (!log.text().contains(QStringLiteral("core integration test"))) {
        std::fprintf(stderr, "Core messages were not delivered to the Log page\n");
        return 1;
    }
    if (app.arguments().contains(QStringLiteral("--hang"))) {
        player.close();
        return app.exec();
    }
    player.back();
    if (!codecConfigured || keys.size() != 2 || keys[0] != 4 || keys[1] != -4) {
        std::fprintf(stderr, "Preview did not configure MP3 or preserve Back\n");
        return 1;
    }
    auto present = [&](unsigned int pixel) {
        runConfig.window.present_pixels(runConfig.window.user, &pixel, 1, 1, 4);
    };
    present(0xff000000);
    if (!player.gestureBack() || player.hasFrameAfterGestureBack()) return 1;
    app.processEvents();
    if (player.hasFrameAfterGestureBack()) return 1;
    present(0xff000000);
    if (player.hasFrameAfterGestureBack()) return 1;
    present(0xffffffff);
    if (!player.hasFrameAfterGestureBack()) return 1;
    present(0xff000000);
    if (player.hasFrameAfterGestureBack()) return 1;
    present(0xffffffff);
    if (!player.hasFrameAfterGestureBack()) return 1;
    player.endGestureFrameWait();
    if (player.hasFrameAfterGestureBack()) return 1;
    if (!player.gestureBack() || player.hasFrameAfterGestureBack()) return 1;
    present(0xffffffff);
    if (player.hasFrameAfterGestureBack()) return 1;
    present(0xff000000);
    if (!player.hasFrameAfterGestureBack()) return 1;
    player.endGestureFrameWait();
    if (!player.gestureBack()) return 1;
    player.endGestureFrameWait();

    TestSurface surface;
    surface.setWidth(400);
    surface.setHeight(400);
    QMouseEvent emptyPress(QEvent::MouseButtonPress, QPointF(150, 150), Qt::LeftButton,
                           Qt::LeftButton, Qt::NoModifier);
    surface.mousePressEvent(&emptyPress);
    if (!emptyPress.isAccepted() || !surface.keepMouseGrab()) {
        std::fprintf(stderr, "Center swipe escaped when the Android frame was unavailable\n");
        return 1;
    }
    QMouseEvent emptyRelease(QEvent::MouseButtonRelease, QPointF(150, 150), Qt::LeftButton,
                             Qt::NoButton, Qt::NoModifier);
    surface.mouseReleaseEvent(&emptyRelease);
    if (!emptyRelease.isAccepted() || surface.keepMouseGrab()) return 1;
    surface.setPlayer(&player);
    std::vector<unsigned int> surfacePixels(400 * 400, 0xff000000);
    runConfig.window.present_pixels(runConfig.window.user, surfacePixels.data(), 400, 400, 400 * 4);
    if (!surface.setProperty("edgeZoneWidth", 32.0)) return 1;
    auto press = [&](qreal x) {
        QMouseEvent event(QEvent::MouseButtonPress, QPointF(x, 150), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        surface.mousePressEvent(&event);
        return surface.keepMouseGrab();
    };
    auto release = [&]() {
        QMouseEvent event(QEvent::MouseButtonRelease, QPointF(150, 150), Qt::LeftButton,
                          Qt::NoButton, Qt::NoModifier);
        surface.mouseReleaseEvent(&event);
    };
    if (!press(150)) { std::fprintf(stderr, "Center swipe did not retain grab\n"); return 1; }
    release();
    if (surface.keepMouseGrab() || press(10)) { std::fprintf(stderr, "Left edge grab failed\n"); return 1; }
    release();
    if (press(390)) { std::fprintf(stderr, "Right edge grab failed\n"); return 1; }
    release();
    if (!press(32)) { std::fprintf(stderr, "Left edge boundary failed\n"); return 1; }
    release();
    if (press(368)) { std::fprintf(stderr, "Right edge boundary failed\n"); return 1; }
    release();
    if (!surface.setProperty("edgeZoneWidth", 16.0)) return 1;
    if (press(15)) { std::fprintf(stderr, "Narrow left edge grab failed\n"); return 1; }
    release();
    if (!press(16)) { std::fprintf(stderr, "Narrow left edge boundary failed\n"); return 1; }
    release();
    if (press(384)) { std::fprintf(stderr, "Narrow right edge boundary failed\n"); return 1; }
    release();
    if (!surface.setProperty("edgeZoneWidth", surface.width() / 2)) return 1;
    for (qreal x : {0.0, 150.0, 200.0, 399.0}) {
        if (press(x)) {
            std::fprintf(stderr, "Full-width back gesture retained the mouse grab\n");
            return 1;
        }
        release();
    }
    bool finished = false;
    QObject::connect(&player, &PreviewPlayer::finished, [&]() { finished = true; });
    int managerRequests = 0;
    QObject::connect(&player, &PreviewPlayer::returnToManagerRequested, [&]() { ++managerRequests; });
    const size_t keyCount = keys.size();
    player.closeToManager();
    if (managerRequests || stops) return 1;
    player.setCloseOnBackGesture(true);
    player.closeToManager();
    player.closeToManager();
    if (managerRequests != 1 || keys.size() != keyCount) return 1;
    if (stops != 1 || finished) return 1;
    runConfig.on_exit(runConfig.user, 0, "");
    app.processEvents();
    if (!finished) return 1;
    cameraStorageRequested = true;
    {
        PreviewPlayer camera(QStringLiteral("."), directory.path());
        camera.start();
        if (!cameraDirectoryAlias || cameraAliasPackage != "org.example.app" ||
            runConfig.storage.link_count != 1 ||
            std::strcmp(runConfig.storage.links[0].name, "Pictures") ||
            !runConfig.storage.links[0].writable) return 1;
    }
    {
        PreviewPlayer denied(QStringLiteral("."), directory.path());
        denied.setDeniedPermissions(QStringList() << QStringLiteral("storage"));
        denied.start();
        int error = -1;
        if (!cameraDirectoryAlias || runConfig.storage.link_count != 1 ||
            runConfig.storage.links[0].writable ||
            runConfig.storage.open(runConfig.storage.user, 0, "new.jpg",
                                   MEGLER_STORAGE_WRITE | MEGLER_STORAGE_CREATE,
                                   MEGLER_STORAGE_FILE, &error) ||
            error != MEGLER_STORAGE_DENIED) return 1;
    }
    {
        PreviewPlayer denied(QStringLiteral("."), directory.path());
        denied.setDeniedPermissions(QStringList() << QStringLiteral("camera"));
        denied.start();
        if (cameraDirectoryAlias || runConfig.storage.link_count) return 1;
    }
    cameraWriteMaxSdk = 28;
    {
        PreviewPlayer expired(QStringLiteral("."), directory.path());
        expired.start();
        if (!cameraDirectoryAlias || runConfig.storage.link_count != 1 ||
            runConfig.storage.links[0].writable) return 1;
    }
    cameraTargetSdk = 0;
    cameraMinSdk = 34;
    QSettings().setValue(QStringLiteral("storage/sharedFolders"), QStringList() << QStringLiteral("Pictures"));
    QFile foreign(directory.path() + QStringLiteral("/Pictures/foreign.txt"));
    if (!foreign.open(QIODevice::WriteOnly) || foreign.write("foreign fixture") != 15) return 1;
    foreign.close();
    {
        PreviewPlayer modern(QStringLiteral("."), directory.path());
        modern.setDeniedPermissions(QStringList() << QStringLiteral("storage")
                                                 << QStringLiteral("read_media_visual"));
        modern.start();
        int error = -1;
        if (!cameraDirectoryAlias || runConfig.storage.link_count != 1 ||
            !runConfig.storage.links[0].writable ||
            runConfig.storage.open(runConfig.storage.user, 0, "foreign.txt",
                                   MEGLER_STORAGE_READ, MEGLER_STORAGE_FILE, &error)) return 1;
        void *own = runConfig.storage.open(runConfig.storage.user, 0, "own.jpg",
                                          MEGLER_STORAGE_WRITE | MEGLER_STORAGE_CREATE,
                                          MEGLER_STORAGE_FILE, &error);
        if (!own || !QFile::exists(directory.path() +
                QStringLiteral("/Pictures/AndroidCompatibilityLayer/org.example.app/own.jpg"))) return 1;
#ifdef Q_OS_UNIX
        ::close(static_cast<int>(reinterpret_cast<intptr_t>(own) - 1));
#endif
    }
    cameraTargetSdk = 28;
    cameraWriteMaxSdk = 0;
    {
        PreviewPlayer selected(QStringLiteral("."), directory.path());
        selected.start();
        int error = -1;
        void *existing = runConfig.storage.open(runConfig.storage.user, 0, "foreign.txt",
                                                MEGLER_STORAGE_READ, MEGLER_STORAGE_FILE, &error);
        if (!cameraDirectoryAlias || !existing) return 1;
#ifdef Q_OS_UNIX
        ::close(static_cast<int>(reinterpret_cast<intptr_t>(existing) - 1));
#endif
    }
    QSettings().setValue(QStringLiteral("storage/sharedFolders"), QStringList());
    cameraStorageRequested = false;
    {
        PreviewPlayer ordinary(QStringLiteral("."), directory.path());
        ordinary.start();
        if (cameraDirectoryAlias || runConfig.storage.link_count) return 1;
    }
    cameraMetadataValid = false;
    {
        const int before = runCalls;
        PreviewPlayer unknown(QStringLiteral("."), directory.path());
        unknown.start();
        if (runCalls != before || cameraDirectoryAlias ||
            !unknown.status().contains(QStringLiteral("camera storage"))) return 1;
    }
    cameraMetadataValid = true;
    cameraTargetSdk = -1;
    {
        const int before = runCalls;
        PreviewPlayer invalid(QStringLiteral("."), directory.path());
        invalid.start();
        if (runCalls != before || cameraDirectoryAlias ||
            !invalid.status().contains(QStringLiteral("camera storage"))) return 1;
    }
    std::puts("Preview MP3 configuration and Back passed");
    return 0;
}
