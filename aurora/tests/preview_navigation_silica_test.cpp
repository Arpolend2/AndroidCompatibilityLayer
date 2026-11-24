#include <QGuiApplication>
#include <QMouseEvent>
#include <QQuickView>
#include <QQuickItem>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlError>
#include <QTimer>
#include <QTouchDevice>
#include <QTouchEvent>
#include <cstdio>

class TestSurface : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject *player MEMBER player)
    Q_PROPERTY(qreal edgeZoneWidth MEMBER edgeZoneWidth)
    Q_PROPERTY(int pageOrientation MEMBER pageOrientation)
public:
    TestSurface(QQuickItem *parent = nullptr) : QQuickItem(parent) {
        setAcceptedMouseButtons(Qt::LeftButton);
        ++created;
    }
    ~TestSurface() { ++destroyed; }
    QObject *player = nullptr;
    qreal edgeZoneWidth = 0;
    int pageOrientation = 0;
    static int created;
    static int destroyed;
    int presses = 0;
    int touches = 0;
protected:
    void mousePressEvent(QMouseEvent *event) override { ++presses; event->accept(); }
    void touchEvent(QTouchEvent *event) override { if (event->type() == QEvent::TouchBegin) ++touches; event->accept(); }
};
int TestSurface::created = 0;
int TestSurface::destroyed = 0;

class TestPlayer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool showStatus READ showStatus CONSTANT)
    Q_PROPERTY(QString status READ status CONSTANT)
    Q_PROPERTY(int backGestureEdgeDp READ backGestureEdgeDp CONSTANT)
    Q_PROPERTY(bool closeOnBackGesture MEMBER closeOnBackGesture CONSTANT)
    Q_PROPERTY(int orientationMask READ orientationMask WRITE setOrientationMask NOTIFY orientationMaskChanged)
    Q_PROPERTY(bool frameCaptureEnabled READ frameCaptureEnabled WRITE setFrameCaptureEnabled NOTIFY frameCaptureEnabledChanged)
    Q_PROPERTY(bool frameCapturePending READ frameCapturePending NOTIFY frameCaptureStatusChanged)
    Q_PROPERTY(QString frameCaptureStatus READ frameCaptureStatus NOTIFY frameCaptureStatusChanged)
public:
    bool showStatus() const { return false; }
    QString status() const { return QString(); }
    int backGestureEdgeDp() const { return 32; }
    Q_INVOKABLE void back() { ++backs; }
    Q_INVOKABLE bool gestureBack() { ++backs; newFrame = false; return true; }
    Q_INVOKABLE bool hasFrameAfterGestureBack() const { return newFrame; }
    Q_INVOKABLE void endGestureFrameWait() {}
    Q_INVOKABLE void close() { ++closes; }
    Q_INVOKABLE void closeToManager() { ++closes; }
    Q_INVOKABLE void captureFramePair() {}
    bool frameCaptureEnabled() const { return m_frameCaptureEnabled; }
    bool frameCapturePending() const { return false; }
    QString frameCaptureStatus() const { return QString(); }
    void setFrameCaptureEnabled(bool enabled)
    {
        if (m_frameCaptureEnabled == enabled) return;
        m_frameCaptureEnabled = enabled;
        emit frameCaptureEnabledChanged();
    }
    void frame(bool fresh) { newFrame = fresh; emit frameChanged(); }
    int backs = 0;
    int closes = 0;
    bool newFrame = false;
    bool closeOnBackGesture = false;
    int orientationMask() const { return m_orientationMask; }
    void setOrientationMask(int mask)
    {
        if (m_orientationMask == mask) return;
        m_orientationMask = mask;
        emit orientationMaskChanged();
    }
signals:
    void frameChanged();
    void orientationMaskChanged();
    void frameCaptureEnabledChanged();
    void frameCaptureStatusChanged();
private:
    int m_orientationMask = -1;
    bool m_frameCaptureEnabled = false;
};

class TestPicker : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE void setUiReady(bool) {}
    Q_INVOKABLE void showFailed(const QString &) { ++failed; }
    Q_INVOKABLE void canceled(const QString &) { ++cancels; }
    Q_INVOKABLE void selected(const QString &, const QString &) {}
    void request() { emit showPicker(QStringLiteral("42"), QStringList() << QStringLiteral("*")); }
    int failed = 0;
    int cancels = 0;
signals:
    void showPicker(const QString &token, const QStringList &filters);
};

class TestDiagnostics : public QObject {
    Q_OBJECT
public:
    QString error = QStringLiteral("test write failure");
    QString overlay;
    Q_INVOKABLE QString standaloneDiagnosticFileError() const { return error; }
    Q_INVOKABLE QString overlayText() const { return overlay; }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const bool diagnosticTest = argc == 3 && QByteArray(argv[2]) == "--diagnostic";
    const bool orientationTest = argc == 3 && QByteArray(argv[2]) == "--orientation";
    const bool orientationPolicyTest = argc == 3 && QByteArray(argv[2]) == "--orientation-policy";
    const bool frameCaptureTest = argc == 3 && QByteArray(argv[2]) == "--frame-capture";
    const bool overlayTest = argc == 3 && QByteArray(argv[2]) == "--overlay";
    if (argc != 2 && !(argc == 3 && QByteArray(argv[2]) == "--microg") &&
        !diagnosticTest && !orientationTest && !orientationPolicyTest && !frameCaptureTest && !overlayTest) {
        std::fprintf(stderr, "Expected the absolute path to Preview.qml\n");
        return 1;
    }

    qmlRegisterType<TestSurface>("AndroidCompatibilityLayer.Preview", 1, 0, "PreviewSurface");
    TestPlayer player;
    player.closeOnBackGesture = argc == 3 && !diagnosticTest && !orientationTest && !orientationPolicyTest && !frameCaptureTest && !overlayTest;
    TestPicker picker;
    TestDiagnostics diagnostics;
    QQuickView view;
    view.rootContext()->setContextProperty("preview", &player);
    view.rootContext()->setContextProperty("pickerHost", &picker);
    view.rootContext()->setContextProperty("standaloneFileDiagnostics", diagnosticTest);
    view.rootContext()->setContextProperty("diagnosticLog", &diagnostics);
    view.resize(540, 960);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (view.status() == QQuickView::Error) {
        std::fprintf(stderr, "%s\n", qPrintable(view.errors().first().toString()));
        return 1;
    }
    if (view.rootObject()->findChild<QObject *>(QStringLiteral("backGestureHint"))) return 1;
    if (overlayTest) {
        QTimer::singleShot(700, [&]() {
            QObject *overlay = view.rootObject()->findChild<QObject *>(QStringLiteral("diagnosticOverlay"));
            QObject *text = view.rootObject()->findChild<QObject *>(QStringLiteral("diagnosticOverlayText"));
            if (!overlay || !text || overlay->property("visible").toBool()) {
                app.exit(1);
                return;
            }
            diagnostics.overlay = QStringLiteral("droidmedia started");
            QTimer::singleShot(700, [&, overlay, text]() {
                if (!overlay->property("visible").toBool() ||
                    text->property("text").toString() != diagnostics.overlay) {
                    app.exit(1);
                    return;
                }
                auto *item = qobject_cast<QQuickItem *>(overlay);
                TestSurface *surface = view.rootObject()->findChild<TestSurface *>();
                if (!item || !surface || item->height() > view.height() * 0.25) {
                    app.exit(1);
                    return;
                }
                QTouchDevice device;
                device.setType(QTouchDevice::TouchScreen);
                device.setCapabilities(QTouchDevice::Position);
                const auto sendTouch = [&](const QPointF &position) {
                    QTouchEvent::TouchPoint touch(0);
                    touch.setState(Qt::TouchPointPressed);
                    touch.setPos(position);
                    touch.setScenePos(position);
                    touch.setScreenPos(position);
                    QTouchEvent begin(QEvent::TouchBegin, &device, Qt::NoModifier,
                                      Qt::TouchPointPressed, QList<QTouchEvent::TouchPoint>() << touch);
                    QCoreApplication::sendEvent(&view, &begin);
                    touch.setState(Qt::TouchPointReleased);
                    QTouchEvent end(QEvent::TouchEnd, &device, Qt::NoModifier,
                                    Qt::TouchPointReleased, QList<QTouchEvent::TouchPoint>() << touch);
                    QCoreApplication::sendEvent(&view, &end);
                };
                const QPointF point = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
                sendTouch(point);
                if (surface->touches != 1) {
                    std::fprintf(stderr, "overlay intercepted portrait touch\n");
                    app.exit(1);
                    return;
                }
                QMouseEvent press(QEvent::MouseButtonPress, point, Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&view, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, point, Qt::LeftButton,
                                    Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&view, &release);
                if (surface->presses != 1) {
                    std::fprintf(stderr, "overlay intercepted input\n");
                    app.exit(1);
                    return;
                }
                view.resize(960, 540);
                QCoreApplication::processEvents();
                if (item->width() > 960 || item->height() > 540 * 0.25) {
                    app.exit(1);
                    return;
                }
                const QPointF landscapePoint = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
                QMouseEvent landscapePress(QEvent::MouseButtonPress, landscapePoint, Qt::LeftButton,
                                           Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&view, &landscapePress);
                QMouseEvent landscapeRelease(QEvent::MouseButtonRelease, landscapePoint, Qt::LeftButton,
                                             Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(&view, &landscapeRelease);
                if (surface->presses != 2) {
                    std::fprintf(stderr, "overlay intercepted landscape input\n");
                    app.exit(1);
                    return;
                }
                sendTouch(landscapePoint);
                if (surface->touches != 2) {
                    std::fprintf(stderr, "overlay intercepted landscape touch\n");
                    app.exit(1);
                    return;
                }
                diagnostics.overlay.clear();
                QTimer::singleShot(700, [&, overlay]() {
                    app.exit(overlay->property("visible").toBool() ? 1 : 0);
                });
            });
        });
        return app.exec();
    }
    if (frameCaptureTest) {
        QTimer::singleShot(700, [&]() {
            QObject *button = view.rootObject()->findChild<QObject *>(QStringLiteral("captureDiagnosticFrames"));
            if (!button || button->property("visible").toBool()) {
                std::fprintf(stderr, "frame capture visible with logging disabled\n");
                app.exit(1);
                return;
            }
            player.setFrameCaptureEnabled(true);
            QCoreApplication::processEvents();
            if (!button->property("visible").toBool()) {
                std::fprintf(stderr, "frame capture hidden with logging enabled\n");
                app.exit(1);
                return;
            }
            view.resize(960, 540);
            QCoreApplication::processEvents();
            if (!button->property("visible").toBool()) {
                std::fprintf(stderr, "frame capture hidden in landscape\n");
                app.exit(1);
                return;
            }
            player.setFrameCaptureEnabled(false);
            QCoreApplication::processEvents();
            app.exit(button->property("visible").toBool() ? 1 : 0);
        });
        return app.exec();
    }
    if (diagnosticTest) {
        QTimer::singleShot(800, [&]() {
            QObject *error = view.rootObject()->findChild<QObject *>(QStringLiteral("standaloneDiagnosticError"));
            if (!error || !error->property("visible").toBool()) {
                std::fprintf(stderr, "standalone diagnostic error is not visible\n");
                app.exit(1);
                return;
            }
            diagnostics.error.clear();
            QTimer::singleShot(1400, [&]() {
                QObject *current = view.rootObject()->findChild<QObject *>(QStringLiteral("standaloneDiagnosticError"));
                app.exit(current && !current->property("visible").toBool() ? 0 : 1);
            });
        });
        return app.exec();
    }
    bool failed = false;
    auto action = [&view, &failed](const char *code) {
        QQmlExpression expression(qmlContext(view.rootObject()), view.rootObject(),
                                  QString::fromUtf8(code));
        QVariant result = expression.evaluate();
        if (expression.hasError()) {
            std::fprintf(stderr, "%s\n", qPrintable(expression.error().toString()));
            failed = true;
        }
        return result;
    };
    if (orientationTest) {
        QTimer::singleShot(500, [&]() {
            TestSurface *surface = view.rootObject()->findChild<TestSurface *>();
            const char *names[] = { "Portrait", "Landscape", "PortraitInverted", "LandscapeInverted" };
            const int expected[] = { Qt::PortraitOrientation, Qt::LandscapeOrientation,
                Qt::InvertedPortraitOrientation, Qt::InvertedLandscapeOrientation };
            if (!surface) failed = true;
            for (int i = 0; surface && i < 4; ++i) {
                const QByteArray expression = QByteArray("pageStack.currentPage.orientation = Orientation.") + names[i];
                action(expression.constData());
                QCoreApplication::processEvents();
                if (surface->pageOrientation != expected[i]) {
                    std::fprintf(stderr, "page orientation mapping failed for %s: %d\n",
                                 names[i], surface->pageOrientation);
                    failed = true;
                }
            }
            app.exit(failed ? 1 : 0);
        });
        return app.exec();
    }
    if (orientationPolicyTest) {
        QTimer::singleShot(500, [&]() {
            const QByteArray expectedLock = qgetenv("ACL_TEST_EXPECT_ORIENTATION_LOCK");
            if (!expectedLock.isEmpty() &&
                view.rootObject()->property("systemOrientationLock").toString() !=
                    QString::fromLatin1(expectedLock)) {
                std::fprintf(stderr, "system orientation setting was not read\n");
                failed = true;
            }
            if (expectedLock == "landscape") {
                player.setOrientationMask(int(Qt::PortraitOrientation));
                QCoreApplication::processEvents();
                const int expected = action("Orientation.Landscape").toInt();
                if (view.rootObject()->property("allowedOrientations").toInt() != expected ||
                    action("pageStack.currentPage.allowedOrientations").toInt() != expected) {
                    std::fprintf(stderr, "system orientation setting did not override Android policy\n");
                    failed = true;
                }
            }
            view.rootObject()->setProperty("systemOrientationLock", QStringLiteral("dynamic"));
            struct PolicyCase {
                int mask;
                const char *expression;
            };
            const PolicyCase cases[] = {
                { -1, "defaultAllowedOrientations" },
                { int(Qt::PortraitOrientation), "Orientation.Portrait" },
                { int(Qt::LandscapeOrientation), "Orientation.Landscape" },
                { int(Qt::InvertedPortraitOrientation), "Orientation.PortraitInverted" },
                { int(Qt::InvertedLandscapeOrientation), "Orientation.LandscapeInverted" },
                { int(Qt::LandscapeOrientation | Qt::InvertedLandscapeOrientation),
                  "Orientation.Landscape | Orientation.LandscapeInverted" },
                { int(Qt::PortraitOrientation | Qt::LandscapeOrientation |
                      Qt::InvertedPortraitOrientation | Qt::InvertedLandscapeOrientation),
                  "Orientation.All" }
            };
            for (const PolicyCase &policy : cases) {
                player.setOrientationMask(policy.mask);
                QCoreApplication::processEvents();
                const int expected = action(policy.expression).toInt();
                if (view.rootObject()->property("allowedOrientations").toInt() != expected ||
                    action("pageStack.currentPage.allowedOrientations").toInt() != expected) {
                    std::fprintf(stderr, "orientation policy mapping failed for mask %d\n", policy.mask);
                    failed = true;
                }
            }
            const struct {
                const char *lock;
                const char *expression;
            } lockedCases[] = {
                { "portrait", "Orientation.Portrait" },
                { "landscape", "Orientation.Landscape" },
                { "portrait-inverted", "Orientation.PortraitInverted" },
                { "landscape-inverted", "Orientation.LandscapeInverted" }
            };
            for (const auto &locked : lockedCases) {
                view.rootObject()->setProperty("systemOrientationLock", QString::fromLatin1(locked.lock));
                for (const PolicyCase &policy : cases) {
                    player.setOrientationMask(policy.mask);
                    QCoreApplication::processEvents();
                    const int expected = action(locked.expression).toInt();
                    if (view.rootObject()->property("allowedOrientations").toInt() != expected ||
                        action("pageStack.currentPage.allowedOrientations").toInt() != expected) {
                        std::fprintf(stderr, "system orientation lock %s ignored for mask %d\n",
                                     locked.lock, policy.mask);
                        failed = true;
                    }
                }
            }
            player.setOrientationMask(int(Qt::PortraitOrientation));
            view.rootObject()->setProperty("systemOrientationLock", QStringLiteral("dynamic"));
            QCoreApplication::processEvents();
            const int portrait = action("Orientation.Portrait").toInt();
            if (view.rootObject()->property("allowedOrientations").toInt() != portrait ||
                action("pageStack.currentPage.allowedOrientations").toInt() != portrait) {
                std::fprintf(stderr, "Android policy not restored after unlocking orientation\n");
                failed = true;
            }
            app.exit(failed ? 1 : 0);
        });
        return app.exec();
    }
    if (player.closeOnBackGesture) {
        QTimer::singleShot(700, [&]() {
            picker.request();
        });
        QTimer::singleShot(2000, [&]() {
            action("pageStack.navigateBack(PageStackAction.Immediate)");
        });
        QTimer::singleShot(3200, [&]() {
            if (player.backs != 0 || player.closes != 0 || picker.cancels != 1) {
                std::fprintf(stderr, "microG picker: backs=%d closes=%d cancels=%d failed=%d depth=%d\n",
                             player.backs, player.closes, picker.cancels, picker.failed,
                             action("pageStack.depth").toInt());
                failed = true;
            }
            action("pageStack.navigateBack(PageStackAction.Immediate)");
        });
        QTimer::singleShot(4400, [&]() {
            if (player.backs != 0 || player.closes != 1) {
                std::fprintf(stderr, "microG gesture: backs=%d closes=%d\n", player.backs, player.closes);
                failed = true;
            }
            app.exit(failed ? 1 : 0);
        });
        return app.exec();
    }
    QTimer::singleShot(100, [&]() {
        action("pageStack.navigateBack(PageStackAction.Immediate)");
    });
    QTimer::singleShot(150, [&]() {
        player.frame(false);
        if (player.backs != 1 || action("pageStack.depth").toInt() != 1) {
            std::fprintf(stderr, "first back: backs=%d depth=%d\n", player.backs, action("pageStack.depth").toInt());
            failed = true;
        }
        player.frame(true);
    });
    QTimer::singleShot(200, [&]() {
        action("pageStack.navigateBack(PageStackAction.Immediate)");
        picker.request();
    });
    QTimer::singleShot(700, [&]() {
        if (player.backs != 2 || action("pageStack.depth").toInt() != 3) {
            std::fprintf(stderr, "picker: backs=%d depth=%d\n", player.backs, action("pageStack.depth").toInt());
            failed = true;
        }
        action("pageStack.navigateBack(PageStackAction.Immediate)");
    });
    QTimer::singleShot(1000, [&]() {
        if (player.backs != 2 || picker.cancels != 1 || picker.failed != 0) {
            std::fprintf(stderr, "picker close: backs=%d cancels=%d failed=%d\n", player.backs, picker.cancels, picker.failed);
            failed = true;
        }
        action("pageStack.navigateBack(PageStackAction.Immediate)");
    });
    QTimer::singleShot(1200, [&]() {
        if (player.backs != 3 || action("pageStack.depth").toInt() != 1) {
            std::fprintf(stderr, "third back: backs=%d depth=%d\n", player.backs, action("pageStack.depth").toInt());
            failed = true;
        }
        player.frame(true);
    });
    QTimer::singleShot(1400, [&]() {
        if (player.backs != 3 || picker.cancels != 1 || picker.failed != 0 ||
                TestSurface::created != 1 || TestSurface::destroyed != 0) {
            std::fprintf(stderr, "restored: backs=%d cancels=%d failed=%d created=%d destroyed=%d\n",
                         player.backs, picker.cancels, picker.failed, TestSurface::created, TestSurface::destroyed);
            failed = true;
        }
        action("pageStack.navigateBack(PageStackAction.Immediate)");
    });
    QTimer::singleShot(9000, [&]() {
        if (player.backs != 4 || player.closes != 0 ||
                action("pageStack.depth").toInt() != 2 || TestSurface::created != 1 ||
                TestSurface::destroyed != 0) {
            std::fprintf(stderr, "timeout: backs=%d closes=%d depth=%d\n", player.backs, player.closes, action("pageStack.depth").toInt());
            failed = true;
        }
        action("pageStack.navigateBack(PageStackAction.Immediate)");
    });
    QTimer::singleShot(9600, [&]() {
        if (player.backs != 5 || action("pageStack.depth").toInt() != 1) {
            std::fprintf(stderr, "last back: backs=%d depth=%d\n", player.backs, action("pageStack.depth").toInt());
            failed = true;
        }
        player.frame(true);
    });
    QTimer::singleShot(10200, [&]() {
        if (player.backs != 5 || player.closes != 0 ||
                action("pageStack.depth").toInt() != 2) {
            std::fprintf(stderr, "last restore: backs=%d closes=%d depth=%d\n", player.backs, player.closes, action("pageStack.depth").toInt());
            failed = true;
        }
        std::fprintf(stderr, "preview navigation Silica test %s\n", failed ? "failed" : "passed");
        app.exit(failed ? 1 : 0);
    });
    return app.exec();
}
#include "preview_navigation_silica_test.moc"
