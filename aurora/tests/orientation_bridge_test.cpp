#include "../orientation_bridge.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static void waitForSurface()
{
    QEventLoop loop;
    QTimer::singleShot(200, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    OrientationBridge *bridge = OrientationBridge::instance();
    const megler_display_host host = bridge->host();
    int surfaces = 0, pending = 0, unavailable = 0;
    std::vector<int> requests;
    QObject::connect(bridge, &OrientationBridge::surfaceChanged,
                     [&](int, int, int, int) { ++surfaces; });
    QObject::connect(bridge, &OrientationBridge::surfacePending, [&]() { ++pending; });
    QObject::connect(bridge, &OrientationBridge::surfaceUnavailable, [&]() { ++unavailable; });
    QObject::connect(bridge, &OrientationBridge::requestChanged, [&]() {
        check(QThread::currentThread() == app.thread(), "request delivered off GUI thread");
        requests.push_back(bridge->requested());
    });

    bridge->setOrientations(OrientationBridge::Portrait, OrientationBridge::Portrait);
    bridge->setViewport(360, 640, 3, 480);
    bridge->start();
    check(!bridge->hasSurface(), "unsettled surface was exposed");
    waitForSurface();
    OrientationBridge::Surface s = bridge->surface();
    check(bridge->hasSurface() && s.width == 1080 && s.height == 1920 &&
          s.density == 480 && s.rotation == 0, "native portrait surface incorrect");
    check(surfaces == 1, "initial surface missing");
    requests.clear();

    std::thread worker([&]() {
        host.orientation(host.user, MEGLER_ORIENTATION_LANDSCAPE);
        host.orientation(host.user, MEGLER_ORIENTATION_REVERSE_PORTRAIT);
    });
    worker.join();
    QCoreApplication::processEvents();
    check(requests.size() == 2 && requests[0] == MEGLER_ORIENTATION_LANDSCAPE &&
          requests[1] == MEGLER_ORIENTATION_REVERSE_PORTRAIT, "request order lost");
    check(bridge->requestSupported() && bridge->policy() == OrientationBridge::FixedPortraitInverted,
          "fixed request mapping failed");

    host.orientation(host.user, MEGLER_ORIENTATION_SENSOR_LANDSCAPE);
    QCoreApplication::processEvents();
    check(!bridge->requestSupported() && bridge->policy() == OrientationBridge::FixedPortraitInverted,
          "unsupported mode silently changed policy");

    bridge->setViewport(640, 360, 3, 480);
    bridge->setOrientations(OrientationBridge::Portrait, OrientationBridge::Landscape);
    check(!bridge->hasSurface() && pending >= 2, "resize was not pending");
    waitForSurface();
    s = bridge->surface();
    check(s.width == 1920 && s.height == 1080 && s.density == 480 && s.rotation == 1,
          "landscape surface incorrect");
    check(surfaces == 2, "resize was not coalesced");
    bridge->setViewport(639, 360, 3, 480);
    bridge->setViewport(640, 360, 3, 480);
    waitForSurface();
    check(bridge->hasSurface() && surfaces == 3, "return to same size did not settle");

    bridge->setOrientations(OrientationBridge::Landscape, OrientationBridge::Portrait);
    waitForSurface();
    check(bridge->surface().rotation == 3, "landscape-native rotation incorrect");
    bridge->setOrientations(OrientationBridge::Landscape, OrientationBridge::LandscapeInverted);
    waitForSurface();
    check(bridge->surface().rotation == 2, "inverted orientation incorrect");

    host.orientation(host.user, MEGLER_ORIENTATION_LOCKED);
    QCoreApplication::processEvents();
    check(bridge->policy() == OrientationBridge::FixedLandscapeInverted && bridge->requestSupported(),
          "locked policy did not use actual UI orientation");
    host.orientation(host.user, MEGLER_ORIENTATION_NOSENSOR);
    QCoreApplication::processEvents();
    check(bridge->policy() == OrientationBridge::FixedLandscape, "nosensor ignored native orientation");

    bridge->setOrientations(OrientationBridge::Unknown, OrientationBridge::Portrait);
    host.orientation(host.user, MEGLER_ORIENTATION_NOSENSOR);
    QCoreApplication::processEvents();
    check(!bridge->requestSupported() && bridge->policy() == OrientationBridge::FixedLandscape,
          "unknown native direction accepted");
    waitForSurface();
    check(!bridge->hasSurface() && unavailable == 1, "unknown orientation was fabricated");
    bridge->setOrientations(OrientationBridge::Portrait, OrientationBridge::Portrait);
    bridge->setViewport(std::numeric_limits<double>::infinity(), 360, 3, 480);
    waitForSurface();
    check(!bridge->hasSurface() && unavailable == 2, "invalid viewport was accepted");

    requests.clear();
    host.orientation(host.user, MEGLER_ORIENTATION_UNSPECIFIED);
    QCoreApplication::processEvents();
    check(bridge->requestSupported() && bridge->policy() == OrientationBridge::PlatformDefault,
          "default policy was not restored");
    requests.clear();
    host.orientation(host.user, MEGLER_ORIENTATION_PORTRAIT);
    bridge->stop();
    bridge->start();
    requests.clear();
    QCoreApplication::processEvents();
    check(requests.empty(), "request crossed stop/start boundary");
    bridge->stop();
    const int oldSurfaces = surfaces;
    bridge->setViewport(100, 100, 1, 160);
    waitForSurface();
    check(surfaces == oldSurfaces, "surface published after stop");
    std::puts("Orientation bridge OK");
    return 0;
}
