#include "../preview.h"
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
#include <QMouseEvent>
#include <QTemporaryDir>

#include <cstdio>
#include <vector>

static bool codecConfigured;
static std::vector<int> keys;
static megler_run_config runConfig;
static int stops;
static QByteArray localeSet;
static uint32_t themeAccent;
static bool themeDark;
static double fixedLatitude;
static double fixedLongitude;

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
extern "C" bool mg_apk_read_info(const char *, mg_apk_info *) { return false; }
extern "C" bool mg_apk_gms_name(const char *) { return false; }
extern "C" bool mg_apk_is_microg(const char *) { return false; }
extern "C" void megler_set_locale(const char *locale) { localeSet = locale; }
extern "C" void megler_set_theme(uint32_t accent, bool dark) { themeAccent = accent; themeDark = dark; }
extern "C" bool megler_run(const megler_run_config *config)
{
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
extern "C" void megler_set_surface(int, int, int, int) {}
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
bool PreviewKeyboard::back() { return false; }
void PreviewKeyboard::setSurface(QQuickItem *, const QSize &) {}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AndroidCompatibilityLayerTest"));
    QCoreApplication::setApplicationName(QStringLiteral("PreviewRuntime"));
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
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
    PreviewPlayer player(QStringLiteral("."), QStringLiteral("."));
    player.setStandaloneSettings(QStringLiteral("de-DE"), QStringLiteral("light"), QStringLiteral("55.5,37.5"));
    player.setSystemTheme(true, QColor(0x12, 0x34, 0x56));
    player.setViewport(1080, 2340, 1);
    player.start();
    app.processEvents();
    if (localeSet != "de-DE" || themeDark || themeAccent != 0xff123456 ||
        fixedLatitude != 55.5 || fixedLongitude != 37.5 ||
        runConfig.width != 443 || runConfig.height != 960 || runConfig.density_dpi != 148) {
        std::fprintf(stderr, "Runtime settings were not passed to Megler\n");
        return 1;
    }
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
    bool finished = false;
    QObject::connect(&player, &PreviewPlayer::finished, [&]() { finished = true; });
    player.close();
    if (stops != 1 || finished) return 1;
    runConfig.on_exit(runConfig.user, 0, "");
    app.processEvents();
    if (!finished) return 1;
    std::puts("Preview MP3 configuration and Back passed");
    return 0;
}
