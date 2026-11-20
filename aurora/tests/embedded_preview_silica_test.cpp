#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <cstdio>

class TestSurface : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QObject *player MEMBER player)
    Q_PROPERTY(qreal edgeZoneWidth MEMBER edgeZoneWidth)
public:
    TestSurface(QQuickItem *parent = nullptr) : QQuickItem(parent) {}
    QObject *player = nullptr;
    qreal edgeZoneWidth = 0;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 2) return 2;
    qmlRegisterType<TestSurface>("AndroidCompatibilityLayer.Preview", 1, 0, "PreviewSurface");
    QQuickView view;
    view.resize(540, 960);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (view.status() != QQuickView::Ready) {
        for (const QQmlError &error : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
        return 1;
    }
    bool failed = false;
    auto action = [&view, &failed](const char *code) {
        QQmlExpression expression(qmlContext(view.rootObject()), view.rootObject(), QString::fromUtf8(code));
        QVariant result = expression.evaluate();
        if (expression.hasError()) {
            std::fprintf(stderr, "%s\n", qPrintable(expression.error().toString()));
            failed = true;
        }
        return result;
    };
    auto check = [&failed](bool condition, const char *message) {
        if (!condition) { std::fprintf(stderr, "%s\n", message); failed = true; }
    };
    QTimer::singleShot(300, [&]() { action("openGuest()"); });
    QTimer::singleShot(600, [&]() {
        TestSurface *surface = view.rootObject()->findChild<TestSurface *>();
        check(surface && surface->width() > 0 && surface->edgeZoneWidth == surface->width() / 2,
              "embedded microG surface does not allow back gestures across its full width");
        action("pageStack.updatePeekContainer()");
    });
    QTimer::singleShot(750, [&]() {
        check(action("pageStack._currentContainer.transitionPartner.page === launcher").toBool(),
              "peek did not expose the real manager page");
        check(action("pageStack.currentPage === guest && stops === 0").toBool(), "guest stopped during gesture");
        action("pageStack.canceled()");
    });
    QTimer::singleShot(1400, [&]() {
        check(action("pageStack.currentPage === guest && stops === 0").toBool(), "cancelled gesture closed guest");
        action("openPicker()");
    });
    QTimer::singleShot(2000, [&]() { action("pageStack.navigateBack(PageStackAction.Immediate)"); });
    QTimer::singleShot(2600, [&]() {
        check(action("pageStack.currentPage === guest && stops === 0 && cancels === 1").toBool(), "picker back closed guest");
        action("pageStack.navigateBack(PageStackAction.Animated)");
        check(action("stops === 0").toBool(), "guest stopped before transition");
    });
    QTimer::singleShot(3400, [&]() {
        check(action("pageStack.currentPage === launcher && stops === 1").toBool(), "completed back did not stop guest");
        action("openGuest()");
    });
    QTimer::singleShot(4000, [&]() {
        check(action("pageStack.currentPage === guest && stops === 1").toBool(), "guest could not be reopened");
        action("pageStack.navigateBack(PageStackAction.Immediate)");
    });
    QTimer::singleShot(4600, [&]() {
        check(action("pageStack.currentPage === launcher && stops === 2").toBool(), "reopened guest did not stop");
        action("openGuestAnimated(); openPicker()");
    });
    QTimer::singleShot(6000, [&]() {
        check(action("pageStack.currentPage !== guest && pageStack.depth === 3 && stops === 2").toBool(),
              "picker requested during transition was not queued");
        action("pageStack.navigateBack(PageStackAction.Immediate)");
    });
    QTimer::singleShot(6800, [&]() {
        check(action("pageStack.currentPage === guest && cancels === 2 && stops === 2").toBool(),
              "queued picker back closed guest");
        action("pageStack.navigateBack(PageStackAction.Immediate)");
    });
    QTimer::singleShot(7400, [&]() {
        check(action("pageStack.currentPage === launcher && stops === 3").toBool(), "third guest did not stop");
        app.exit(failed ? 1 : 0);
    });
    return app.exec();
}
#include "embedded_preview_silica_test.moc"
