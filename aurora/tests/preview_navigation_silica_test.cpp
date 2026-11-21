#include <QGuiApplication>
#include <QQuickView>
#include <QQuickItem>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlError>
#include <QTimer>
#include <cstdio>

class TestSurface : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject *player MEMBER player)
    Q_PROPERTY(qreal edgeZoneWidth MEMBER edgeZoneWidth)
    Q_PROPERTY(int pageOrientation MEMBER pageOrientation)
public:
    TestSurface(QQuickItem *parent = nullptr) : QQuickItem(parent) { ++created; }
    ~TestSurface() { ++destroyed; }
    QObject *player = nullptr;
    qreal edgeZoneWidth = 0;
    int pageOrientation = 0;
    static int created;
    static int destroyed;
};
int TestSurface::created = 0;
int TestSurface::destroyed = 0;

class TestPlayer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool showStatus READ showStatus CONSTANT)
    Q_PROPERTY(QString status READ status CONSTANT)
    Q_PROPERTY(int backGestureEdgeDp READ backGestureEdgeDp CONSTANT)
    Q_PROPERTY(bool closeOnBackGesture MEMBER closeOnBackGesture CONSTANT)
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
    void frame(bool fresh) { newFrame = fresh; emit frameChanged(); }
    int backs = 0;
    int closes = 0;
    bool newFrame = false;
    bool closeOnBackGesture = false;
signals:
    void frameChanged();
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
    Q_INVOKABLE QString standaloneDiagnosticFileError() const { return error; }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    const bool diagnosticTest = argc == 3 && QByteArray(argv[2]) == "--diagnostic";
    const bool orientationTest = argc == 3 && QByteArray(argv[2]) == "--orientation";
    if (argc != 2 && !(argc == 3 && QByteArray(argv[2]) == "--microg") &&
        !diagnosticTest && !orientationTest) {
        std::fprintf(stderr, "Expected the absolute path to Preview.qml\n");
        return 1;
    }

    qmlRegisterType<TestSurface>("AndroidCompatibilityLayer.Preview", 1, 0, "PreviewSurface");
    TestPlayer player;
    player.closeOnBackGesture = argc == 3 && !diagnosticTest && !orientationTest;
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
