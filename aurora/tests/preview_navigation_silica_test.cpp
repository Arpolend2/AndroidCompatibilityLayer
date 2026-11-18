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
public:
    TestSurface(QQuickItem *parent = nullptr) : QQuickItem(parent) { ++created; }
    ~TestSurface() { ++destroyed; }
    QObject *player = nullptr;
    qreal edgeZoneWidth = 0;
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
public:
    bool showStatus() const { return false; }
    QString status() const { return QString(); }
    int backGestureEdgeDp() const { return 32; }
    Q_INVOKABLE void back() { ++backs; }
    Q_INVOKABLE bool gestureBack() { ++backs; newFrame = false; return true; }
    Q_INVOKABLE bool hasFrameAfterGestureBack() const { return newFrame; }
    Q_INVOKABLE void endGestureFrameWait() {}
    Q_INVOKABLE void close() { ++closes; }
    void frame(bool fresh) { newFrame = fresh; emit frameChanged(); }
    int backs = 0;
    int closes = 0;
    bool newFrame = false;
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

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 2) {
        std::fprintf(stderr, "Expected the absolute path to Preview.qml\n");
        return 1;
    }

    qmlRegisterType<TestSurface>("AndroidCompatibilityLayer.Preview", 1, 0, "PreviewSurface");
    TestPlayer player;
    TestPicker picker;
    QQuickView view;
    view.rootContext()->setContextProperty("preview", &player);
    view.rootContext()->setContextProperty("pickerHost", &picker);
    view.resize(540, 960);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (view.status() == QQuickView::Error) {
        std::fprintf(stderr, "%s\n", qPrintable(view.errors().first().toString()));
        return 1;
    }
    if (view.rootObject()->findChild<QObject *>(QStringLiteral("backGestureHint"))) return 1;
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
    QTimer::singleShot(7700, [&]() {
        if (player.backs != 4 || player.closes != 0 ||
                action("pageStack.depth").toInt() != 2 || TestSurface::created != 1 ||
                TestSurface::destroyed != 0) {
            std::fprintf(stderr, "timeout: backs=%d closes=%d depth=%d\n", player.backs, player.closes, action("pageStack.depth").toInt());
            failed = true;
        }
        action("pageStack.navigateBack(PageStackAction.Immediate)");
    });
    QTimer::singleShot(7900, [&]() {
        if (player.backs != 5 || action("pageStack.depth").toInt() != 1) {
            std::fprintf(stderr, "last back: backs=%d depth=%d\n", player.backs, action("pageStack.depth").toInt());
            failed = true;
        }
        player.frame(true);
    });
    QTimer::singleShot(8100, [&]() {
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
