#include "../embedded_preview.h"
#include "../store_preview_supervisor.h"
#include "../audio_output.h"
#include "../bluetooth_host.h"
#include "../camera_host.h"
#include "../install_request_host.h"
#include "../location_host.h"
#include "../picker_host.h"
#include "../preview_keyboard.h"
#include "../preview_text.h"
#include "../sensor_host.h"
#include "../../core/src/apk/mg_apk.h"
extern "C" {
#include "../../core/src/drivers/mg_store_restart.h"
}

#include <QFile>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QProcess>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QTimer>
#include <QEventLoop>
#include <QThread>
#include <cstdio>
#include <csignal>
#include <vector>

static megler_run_config config;
static void output(const char *text) { std::printf("%s\n", text); std::fflush(stdout); }
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
struct TestCanvas {
    void *pixels;
    int width;
    int height;
    int stride;
};
static std::vector<unsigned int> testPixels;
static void *testCanvas = nullptr;

static void *MEGLER_CB cpuCanvasNew(void *, void *pixels, int width, int height, int stride)
{
    return new TestCanvas{pixels, width, height, stride};
}

static void MEGLER_CB cpuCanvasFree(void *, void *canvas)
{
    delete static_cast<TestCanvas *>(canvas);
}

static void MEGLER_CB cpuDrawColor(void *, void *canvas, uint32_t color, int)
{
    TestCanvas *target = static_cast<TestCanvas *>(canvas);
    for (int y = 0; y < target->height; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(static_cast<char *>(target->pixels) + y * target->stride);
        for (int x = 0; x < target->width; ++x)
            row[x] = color;
    }
}

extern "C" const megler_gfx_host *mg_gfx_standin()
{
    static megler_gfx_host host = {};
    host.canvas_new = cpuCanvasNew;
    host.canvas_free = cpuCanvasFree;
    host.draw_color = cpuDrawColor;
    return &host;
}

extern "C" bool mg_egl_set_native_display(void *) { return true; }

static bool drawTestFrame(uint32_t color)
{
    config.gfx.draw_color(config.gfx.user, testCanvas, color, MEGLER_GFX_BLEND_SRC);
    if (config.gfx.present && config.gfx.present(config.gfx.user, testCanvas, 0, 0, config.width, config.height))
        return true;
    if (config.gfx.flush)
        config.gfx.flush(config.gfx.user, testCanvas);
    config.window.present_pixels(config.window.user, testPixels.data(), config.width, config.height, config.width * 4);
    return false;
}
#endif
extern "C" bool megler_init(const megler_config *) { return true; }
extern "C" bool mg_apk_read_info(const char *, mg_apk_info *) { return false; }
extern "C" bool mg_apk_gms_name(const char *) { return false; }
extern "C" bool mg_apk_is_microg(const char *) { return false; }
extern "C" void megler_set_locale(const char *) {}
extern "C" void megler_set_theme(uint32_t, bool) {}
extern "C" bool megler_run(const megler_run_config *run)
{
    config = *run;
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    testPixels.resize(size_t(run->width) * run->height);
    testCanvas = config.gfx.canvas_new(config.gfx.user, testPixels.data(), run->width, run->height, run->width * 4);
    if (!testCanvas) return false;
    const bool presented = drawTestFrame(0xff010203);
    output(presented ? "gpu" : config.gfx.present ? "gpu-fallback" : "cpu-canvas");
    QTimer::singleShot(150, []() {
        if (testCanvas) drawTestFrame(0xff040506);
    });
#else
    std::vector<unsigned int> pixels(size_t(run->width) * run->height, 0xff010203);
    config.window.present_pixels(config.window.user, pixels.data(), run->width, run->height, run->width * 4);
    QTimer::singleShot(150, []() {
        std::vector<unsigned int> next(size_t(config.width) * config.height, 0xff040506);
        config.window.present_pixels(config.window.user, next.data(), config.width, config.height, config.width * 4);
    });
#endif
    config.keyboard.show(config.keyboard.user, MEGLER_KEYBOARD_EMAIL, 0, MEGLER_ENTER_DONE);
    output("started");
    return true;
}
extern "C" const char *megler_last_error() { return ""; }
extern "C" void megler_touch(int phase, int, float, float) { if (phase == 0) output("touch"); }
extern "C" void megler_key(int, bool) {}
extern "C" void megler_set_surface(int, int, int, int) {}
extern "C" void megler_stop() {
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
    if (testCanvas) {
        config.gfx.canvas_free(config.gfx.user, testCanvas);
        testCanvas = nullptr;
    }
#endif
    output("stopped"); config.on_exit(config.user, 0, "");
}
extern "C" void megler_text_commit(const char *text) { if (QByteArray(text) == "hello") output("text"); }
extern "C" void megler_text_key(int) {}
extern "C" void megler_keyboard_closed() { output("keyboardClosed"); }
extern "C" void megler_keyboard_height(int) {}
extern "C" void megler_pick_done(unsigned, int status, int count, const char *const *names)
{
    if (status == MEGLER_PICK_OK && count == 1 && names && QByteArray(names[0]) == "picked.txt") output("picked");
}
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
megler_gfx_host previewGraphics() { return *mg_gfx_standin(); }
megler_gfx_host previewGraphics(PreviewGpuCanvasHost *gpu) {
    megler_gfx_host host = previewGraphics();
    gpu->install(&host);
    return host;
}
#else
megler_gfx_host previewGraphics() { return {}; }
#endif
megler_camera_host auroraCameraHost() { return {}; }
megler_bluetooth_host auroraBluetoothHost() { return {}; }
megler_sensor_host auroraSensorHost() { return {}; }
megler_location_host auroraLocationHost() { return {}; }
bool auroraLocationSetFixedLocation(double, double, float) { return true; }
void auroraLocationUseDeviceLocation() {}
megler_audio_host AuroraAudioOutput::callbacks() { return {}; }
megler_system_host InstallRequestHost::host() { return {}; }
QString InstallRequestHost::appsDir() const { return {}; }

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("EmbeddedPreviewTest"));
    QCoreApplication::setApplicationName(QStringLiteral("EmbeddedPreviewTest"));
    if (argc == 5 && QByteArray(argv[1]) == "--store-child") {
        const QByteArray mode(argv[2]);
        QFile count(QString::fromLocal8Bit(argv[3]));
        int n = 0;
        if (count.open(QIODevice::ReadOnly)) { n = count.readAll().toInt(); count.close(); }
        if (!count.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 4;
        count.write(QByteArray::number(++n));
        count.close();
        if (mode == "frames" || mode == "teardown-hang" || mode == "teardown-normal") {
            PreviewPlayer player(QStringLiteral("."), QStringLiteral("."));
            AuroraPickerHost picker;
            player.setPicker(&picker);
            EmbeddedPreviewChild bridge(&player, &picker);
            player.setRemoteKeyboard(bridge.keyboardHost());
            QString error;
            if (!bridge.connectTo(QString::fromLocal8Bit(argv[4]), &error)) return 8;
            if (mode == "frames") {
                QTimer::singleShot(350, &app, [&]() {
                    if (n == 1 && !mg_store_restart_request("com.aurora.store")) app.exit(9);
                    else app.quit();
                });
            } else {
                QObject::connect(&player, &PreviewPlayer::finished, &app, [&]() {
                    QTimer::singleShot(800, &app, &QCoreApplication::quit);
                });
            }
            const int result = app.exec();
            if (mode != "frames") bridge.notifyShutdownComplete();
            if (mode == "teardown-hang") QThread::sleep(30);
            return result;
        }
        if (mode == "restart" || mode == "hang" || mode == "hang-close" ||
                mode == "hang-preview-close" || mode == "stale-watchdog" || mode == "rate" ||
                mode == "requested-crash" || mode == "close" || mode == "other") {
            if ((mode != "restart" && mode != "hang" && mode != "stale-watchdog") || n == 1) {
                const bool accepted = mg_store_restart_request(mode == "other" ? "other.app" : "com.aurora.store");
                if (accepted != !(mode == "other" || (mode == "rate" && n == 4))) return 6;
            }
        }
        if ((mode == "hang" && n == 1) || mode == "hang-close" || mode == "hang-preview-close")
            QThread::sleep(30);
        if (mode == "stale-watchdog" && n == 2) QThread::msleep(1500);
        if (mode == "close") QThread::msleep(600);
        if (mode == "crash" || mode == "requested-crash") std::raise(SIGABRT);
        if (mode == "error") return 7;
        return 0;
    }
    if (argc == 2 && QByteArray(argv[1]) == "--supervisor-test") {
        QTemporaryDir directory;
        if (!directory.isValid()) return 1;
        const QByteArray modes[] = {"restart", "hang", "hang-close", "hang-preview-close",
                                    "stale-watchdog", "normal", "error", "crash",
                                    "requested-crash", "close", "rate", "other", "frames",
                                    "teardown-hang", "teardown-normal"};
        const int expected[] = {2, 2, 1, 1, 2, 1, 1, 1, 1, 1, 4, 1, 2, 1, 1};
        for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
            const QString countName = directory.path() + QLatin1Char('/') + QString::fromLatin1(modes[i]);
            EmbeddedPreview preview;
            preview.setCpuFrameTransportOnly();
            preview.setViewport(540, 960, 1);
            preview.setUiReady(true);
            StorePreviewSupervisor supervisor(&preview, QCoreApplication::applicationFilePath(),
                QStringList() << QStringLiteral("--store-child") << QString::fromLatin1(modes[i]) << countName);
            QEventLoop loop;
            bool done = false;
            int frames = 0;
            bool sawFirstCpuFrame = false;
            bool sawSecondCpuFrame = false;
            qint64 lastFrameKey = 0;
            QObject::connect(&preview, &EmbeddedPreview::frameChanged, &loop, [&]() {
                const QImage frame = preview.frame();
                if (!frame.isNull() && frame.cacheKey() != lastFrameKey) {
                    lastFrameKey = frame.cacheKey();
                    ++frames;
                    sawFirstCpuFrame |= frame.pixel(0, 0) == 0xff010203;
                    sawSecondCpuFrame |= frame.pixel(0, 0) == 0xff040506;
                }
            });
            int code = -1;
            bool crashed = false;
            QObject::connect(&supervisor, &StorePreviewSupervisor::finished, &loop,
                [&](int result, bool failure) { code = result; crashed = failure; done = true; loop.quit(); });
            QString error;
            QElapsedTimer elapsed;
            qint64 stoppedAt = -1;
            qint64 confirmedAt = -1;
            const bool teardown = modes[i].startsWith("teardown-");
            QObject::connect(&preview, &EmbeddedPreview::shutdownComplete, &loop,
                [&]() { confirmedAt = elapsed.elapsed(); });
            if (teardown) {
                QObject::connect(&preview, &EmbeddedPreview::frameChanged, &loop, [&]() {
                    if (stoppedAt >= 0 || preview.frame().isNull()) return;
                    stoppedAt = elapsed.elapsed();
                    supervisor.stop();
                });
            }
            elapsed.start();
            if (!supervisor.start(&error)) { std::fprintf(stderr, "%s\n", qPrintable(error)); return 1; }
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
            if (QFileInfo(QFileInfo(preview.endpoint()).dir().filePath(QStringLiteral("gpu"))).exists()) {
                std::fprintf(stderr, "CPU-only Store unexpectedly opened a GPU endpoint\n");
                return 1;
            }
#endif
            if (modes[i] == "close" || modes[i] == "hang-close")
                QTimer::singleShot(200, &supervisor, &StorePreviewSupervisor::stop);
            if (modes[i] == "hang-preview-close")
                QTimer::singleShot(200, &preview, &EmbeddedPreview::close);
            QTimer::singleShot(12000, &loop, &QEventLoop::quit);
            loop.exec();
            const qint64 teardownMs = elapsed.elapsed() - confirmedAt;
            if (teardown && (stoppedAt < 0 || confirmedAt - stoppedAt < 700 ||
                    (modes[i] == "teardown-hang" && (teardownMs < 500 || teardownMs >= 2500)))) {
                std::fprintf(stderr, "%s: stop=%lld confirmation=%lld teardown=%lld ms\n",
                             modes[i].constData(), static_cast<long long>(stoppedAt),
                             static_cast<long long>(confirmedAt), static_cast<long long>(teardownMs));
                return 1;
            }
            if (modes[i] == "hang" && elapsed.elapsed() >= 4000) {
                std::fprintf(stderr, "expected a prompt restart of the unresponsive Store worker\n");
                return 1;
            }
            QFile count(countName);
            if (!count.open(QIODevice::ReadOnly)) return 1;
            const int runs = count.readAll().toInt();
            if (!done || runs != expected[i] ||
                    (modes[i] == "frames" && (frames < 2 || !sawFirstCpuFrame || !sawSecondCpuFrame)) ||
                    (modes[i] == "error" && code != 7) ||
                    ((modes[i] == "crash" || modes[i] == "requested-crash" ||
                      modes[i] == "hang-close" || modes[i] == "hang-preview-close" ||
                      modes[i] == "teardown-hang") != crashed)) {
                std::fprintf(stderr, "supervisor %s: runs=%d code=%d crashed=%d done=%d\n",
                             modes[i].constData(), runs, code, crashed, done);
                return 1;
            }
        }
        return 0;
    }
    if (argc == 4 && QByteArray(argv[1]) == "--child") {
        PreviewPlayer player(QStringLiteral("."), QStringLiteral("."));
        player.setCpuPresentationOnly(true);
        AuroraPickerHost picker;
        player.setPicker(&picker);
        EmbeddedPreviewChild bridge(&player, &picker);
        player.setRemoteKeyboard(bridge.keyboardHost());
        QString error;
        if (!bridge.connectTo(QString::fromLocal8Bit(argv[2]), &error)) return 2;
        QObject::connect(&bridge, &EmbeddedPreviewChild::failed, &app, [&app](const QString &) { app.exit(3); });
        QObject::connect(&player, &PreviewPlayer::finished, &app, &QCoreApplication::quit);
        QTimer::singleShot(200, &player, [&]() {
            megler_pick_request request = {};
            request.id = 42;
            request.kind = MEGLER_PICK_FILES;
            request.folder = argv[3];
            request.types = "text/plain";
            if (!picker.host().pick(picker.host().user, &request)) output("pickerFailed");
        });
        return app.exec();
    }

    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QString fileName = directory.path() + QStringLiteral("/picked.txt");
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly) || file.write("content") != 7) return 1;
    file.close();
    if (!QDir().mkdir(directory.path() + QStringLiteral("/output"))) return 1;
    QQuickItem root;
    PreviewKeyboard keyboard(&root);
    EmbeddedPreview preview;
    preview.setRemoteKeyboard(&keyboard);
    QProcess process;
    QLocalSocket rogue;
    bool rogueRejected = false;
    bool failed = false;
    int runs = 0;
    int frames = 0;
    int picks = 0;
    qint64 lastFrameKey = 0;
    QByteArray outputText;
    QObject::connect(&preview, &EmbeddedPreview::frameChanged, [&]() {
        QImage image = preview.frame();
        // GPU transport status updates can also signal without delivering new pixels.
        if (image.isNull() || image.cacheKey() == lastFrameKey) return;
        lastFrameKey = image.cacheKey();
        const QRgb expected = frames % 2 == 0 ? 0xff010203 : 0xff040506;
        if (image.pixel(0, 0) != expected) failed = true;
        ++frames;
        preview.touch(0, 1, 1);
    });
    QObject::connect(&preview, &EmbeddedPreview::showPicker, [&](const QString &token, const QStringList &) {
        ++picks;
        preview.selected(token, fileName);
    });
    QObject::connect(&process, &QProcess::readyReadStandardOutput, [&]() {
        outputText += process.readAllStandardOutput();
    });
    QObject::connect(&preview, &EmbeddedPreview::channelError, [&](const QString &error) {
        std::fprintf(stderr, "channel: %s\n", qPrintable(error));
        failed = true;
    });
    QObject::connect(&rogue, &QLocalSocket::disconnected, [&]() { rogueRejected = true; });
    auto start = [&]() {
        QString error;
        if (!preview.listen(&process, &error)) { std::fprintf(stderr, "%s\n", qPrintable(error)); failed = true; return; }
        if (preview.stopping()) failed = true;
        preview.setViewport(540, 960, 1);
        preview.setUiReady(true);
        if (runs == 0) {
            rogue.connectToServer(preview.endpoint());
            if (!rogue.waitForConnected(1000)) failed = true;
        }
        process.start(QCoreApplication::applicationFilePath(),
                      QStringList() << QStringLiteral("--child") << preview.endpoint()
                                    << directory.path() + QStringLiteral("/output"));
    };
    QObject::connect(&process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     [&](int code, QProcess::ExitStatus status) {
        outputText += process.readAllStandardOutput();
        const bool normalRun = outputText.contains("started\n") && outputText.contains("touch\n") &&
                outputText.contains("text\n") && outputText.contains("picked\n") && outputText.contains("stopped\n");
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
        const QByteArray expectedRenderer = qgetenv("ACL_TEST_EXPECT_GPU") == "1" ? "gpu\n" : "cpu-canvas\n";
        if (runs < 2 && !outputText.contains(expectedRenderer)) {
            std::fprintf(stderr, "expected renderer %s; child output: %s\n",
                         expectedRenderer.constData(), outputText.constData());
            failed = true;
        }
#endif
        if (status != QProcess::NormalExit || code != 0 || (runs < 2 ? !normalRun : !outputText.isEmpty())) {
            std::fprintf(stderr, "child output: %s, code %d\n", outputText.constData(), code);
            failed = true;
        }
        ++runs;
        if (runs < 3) {
            QFile::remove(directory.path() + QStringLiteral("/output/picked.txt"));
            outputText.clear();
            QTimer::singleShot(0, [&]() {
                start();
                if (runs == 2) {
                    preview.stop();
                    if (!preview.stopping()) failed = true;
                }
            });
        } else app.exit(failed || !rogueRejected || frames != 4 || picks != 2 ? 1 : 0);
    });
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        if (keyboard.inputMethodQuery(Qt::ImEnabled).toBool() &&
            outputText.contains("started\n") && outputText.contains("touch\n") && picks > runs) {
            QInputMethodEvent event;
            event.setCommitString(QStringLiteral("hello"));
            QCoreApplication::sendEvent(&keyboard, &event);
            if (outputText.contains("picked\n") && outputText.contains("text\n") && frames >= (runs + 1) * 2)
                preview.stop();
        }
    });
    timer.start(50);
    QTimer::singleShot(10000, [&]() {
        std::fprintf(stderr, "embedded preview timed out: %s\n", outputText.constData());
        process.disconnect();
        process.kill();
        process.waitForFinished();
        app.exit(1);
    });
    start();
    return app.exec();
}
