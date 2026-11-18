#include "../preview.h"
#include "../audio_output.h"
#include "../camera_host.h"
#include "../picker_host.h"
#include "../preview_keyboard.h"
#include "../preview_text.h"
#include "../sensor_host.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include <cstdio>
#include <cstdlib>
#include <vector>

struct Touch { int phase; float x, y; };
static std::vector<Touch> touches;
static std::vector<int> keys;

extern "C" bool megler_init(const megler_config *) { return true; }
extern "C" bool megler_run(const megler_run_config *config)
{
    std::vector<unsigned> pixels(size_t(config->width) * size_t(config->height));
    config->window.present_pixels(config->window.user, pixels.data(), config->width, config->height, config->width * 4);
    return true;
}
extern "C" const char *megler_last_error() { return ""; }
extern "C" void megler_touch(int phase, int, float x, float y) { touches.push_back({phase, x, y}); }
extern "C" void megler_key(int code, bool down) { keys.push_back(down ? code : -code); }
extern "C" void megler_set_surface(int, int, int, int) {}
extern "C" void megler_stop() {}
megler_gfx_host previewGraphics() { return {}; }
megler_camera_host auroraCameraHost() { return {}; }
megler_sensor_host auroraSensorHost() { return {}; }
megler_audio_host AuroraAudioOutput::callbacks() { return {}; }
megler_system_host AuroraPickerHost::host() { return {}; }
void AuroraPickerHost::stop() {}
megler_keyboard_host PreviewKeyboard::host() { return {}; }
void PreviewKeyboard::start() {}
void PreviewKeyboard::stop() {}
bool PreviewKeyboard::back() { return false; }
void PreviewKeyboard::setSurface(QQuickItem *, const QSize &) {}

class TestSurface : public PreviewSurface
{
public:
    using PreviewSurface::wheelEvent;
    using PreviewSurface::mousePressEvent;
    using PreviewSurface::mouseMoveEvent;
    using PreviewSurface::mouseReleaseEvent;
};

static void check(bool ok, const char *message)
{
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

static void waitForWheel()
{
    QEventLoop loop;
    QTimer::singleShot(180, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    TestSurface surface;
    surface.setWidth(400);
    surface.setHeight(400);
    PreviewPlayer player(QStringLiteral("."), QStringLiteral("."));
    surface.setPlayer(&player);
    player.start();

    QWheelEvent wheel(QPointF(100, 100), 120, Qt::NoButton, Qt::NoModifier);
    surface.wheelEvent(&wheel);
    waitForWheel();
    check(touches.size() == 6 && touches[0].phase == 0 && touches[4].phase == 1 &&
          touches[5].phase == 2 && touches[0].x == 100 && touches[0].y == 100 &&
          touches[5].x == 100 && touches[5].y == 164, "wheel swipe was not delivered");

    touches.clear();
    QWheelEvent horizontal(QPointF(200, 200), -120, Qt::NoButton, Qt::NoModifier, Qt::Horizontal);
    surface.wheelEvent(&horizontal);
    waitForWheel();
    check(touches.size() == 6 && touches[5].phase == 2 && touches[5].x == 136 && touches[5].y == 200,
          "horizontal wheel swipe was not delivered");

    touches.clear();
    QWheelEvent small(QPointF(100, 100), QPointF(100, 100), QPoint(0, 5), QPoint(), 0,
                      Qt::Vertical, Qt::NoButton, Qt::NoModifier);
    surface.wheelEvent(&small);
    check(touches.empty(), "small wheel delta started a premature swipe");
    surface.wheelEvent(&small);
    waitForWheel();
    check(touches.size() == 6 && touches[5].y == 124, "small wheel deltas did not accumulate");

    touches.clear();
    surface.wheelEvent(&wheel);
    surface.wheelEvent(&wheel);
    waitForWheel();
    check(touches.size() == 12 && touches[5].phase == 2 && touches[6].phase == 0 &&
          touches[11].phase == 2, "queued wheel steps were lost");

    touches.clear();
    surface.wheelEvent(&wheel);
    surface.setWidth(390);
    check(touches.size() == 2 && touches[0].phase == 0 && touches[1].phase == 3,
          "resize did not cancel the wheel swipe");
    surface.setWidth(400);

    touches.clear();
    surface.wheelEvent(&wheel);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(150, 150), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    surface.mousePressEvent(&press);
    QWheelEvent duringTouch(QPointF(150, 150), 120, Qt::NoButton, Qt::NoModifier);
    surface.wheelEvent(&duringTouch);
    QMouseEvent move(QEvent::MouseMove, QPointF(160, 170), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    surface.mouseMoveEvent(&move);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(160, 170), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    surface.mouseReleaseEvent(&release);
    waitForWheel();
    check(!duringTouch.isAccepted() && touches.size() == 5 && touches[0].phase == 0 &&
          touches[1].phase == 3 && touches[2].phase == 0 && touches[3].phase == 1 &&
          touches[4].phase == 2 && touches[4].x == 160 && touches[4].y == 170,
          "wheel cancellation interfered with touch dragging");

    touches.clear();
    surface.mousePressEvent(&press);
    QMouseEvent backDown(QEvent::MouseButtonPress, QPointF(150, 150), Qt::BackButton,
                         Qt::LeftButton | Qt::BackButton, Qt::NoModifier);
    QMouseEvent backUp(QEvent::MouseButtonRelease, QPointF(150, 150), Qt::BackButton, Qt::LeftButton, Qt::NoModifier);
    surface.mousePressEvent(&backDown);
    surface.mousePressEvent(&backDown);
    surface.mouseReleaseEvent(&backUp);
    surface.mouseMoveEvent(&move);
    surface.mouseReleaseEvent(&release);
    check(keys.size() == 2 && keys[0] == 4 && keys[1] == -4 &&
          touches.size() == 3 && touches[0].phase == 0 && touches[1].phase == 1 && touches[2].phase == 2,
          "mouse Back repeated or interrupted the primary-button gesture");
    touches.clear();
    surface.wheelEvent(&wheel);
    surface.mousePressEvent(&backDown);
    surface.mouseReleaseEvent(&backUp);
    waitForWheel();
    check(keys.size() == 4 && keys[2] == 4 && keys[3] == -4 && touches.size() == 6 &&
          touches[0].phase == 0 && touches[5].phase == 2,
          "mouse Back did not reset or interrupted wheel scrolling");
    return 0;
}
