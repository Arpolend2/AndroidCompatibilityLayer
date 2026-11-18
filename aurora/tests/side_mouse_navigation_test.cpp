#include "../side_mouse_buttons.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickView>
#include <cstdio>

class TestView : public QQuickView
{
public:
    int ordinaryEvents = 0;
protected:
    bool event(QEvent *event) override {
        if (event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseButtonRelease)
            ++ordinaryEvents;
        return QQuickView::event(event);
    }
};

int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &msg) {
        std::fprintf(stderr, "Qt: %s\n", qPrintable(msg));
    });
    QGuiApplication app(argc, argv);
    if (argc != 2) return 2;
    qmlRegisterType<SideMouseButtons>("AndroidCompatibilityLayer.Navigation", 1, 0, "SideMouseButtons");
    TestView view;
    view.rootContext()->setContextProperty(QStringLiteral("testWindow"), &view);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (view.status() != QQuickView::Ready) {
        for (const QQmlError &error : view.errors())
            std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
        return 1;
    }
    view.show();
    app.processEvents();
    bool failed = false;
    auto eval = [&view, &failed](const char *code) {
        QQmlExpression expression(qmlContext(view.rootObject()), view.rootObject(), QString::fromUtf8(code));
        QVariant result = expression.evaluate();
        if (expression.hasError()) {
            std::fprintf(stderr, "%s\n", qPrintable(expression.error().toString()));
            failed = true;
        }
        return result;
    };
    auto check = [&failed](bool condition, const char *message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
            failed = true;
        }
    };
    auto mouse = [&view](QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent event(type, QPointF(20, 20), button, held, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &event);
    };
    auto click = [&mouse](Qt::MouseButton button) {
        mouse(QEvent::MouseButtonPress, button, button);
        mouse(QEvent::MouseButtonRelease, button, Qt::NoButton);
    };

    SideMouseButtons *filter = view.rootObject()->findChild<SideMouseButtons *>();
    check(filter && filter->window() == &view, "window filter was not installed");
    mouse(QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonPress, Qt::ForwardButton, Qt::LeftButton | Qt::ForwardButton);
    mouse(QEvent::MouseButtonPress, Qt::ForwardButton, Qt::LeftButton | Qt::ForwardButton);
    mouse(QEvent::MouseButtonRelease, Qt::ForwardButton, Qt::LeftButton);
    mouse(QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton);
    check(eval("section === 1 && sectionChanges === 1").toBool(), "forward was not one-shot with left held");
    check(view.ordinaryEvents == 2, "left button was intercepted");
    click(Qt::RightButton);
    click(Qt::MiddleButton);
    check(view.ordinaryEvents == 6, "ordinary buttons were intercepted");
    click(Qt::BackButton);
    click(Qt::BackButton);
    check(eval("section === 0 && sectionChanges === 2").toBool(), "back boundary failed");
    for (int i = 0; i != 5; ++i) click(Qt::ForwardButton);
    check(eval("section === 3 && sectionChanges === 5").toBool(), "forward boundary failed");

    eval("stack.currentPage = page; stack.previous = launcher; stack.next = null");
    click(Qt::BackButton);
    check(eval("stack.currentPage === launcher && stack.backCount === 1 && section === 3").toBool(),
          "back did not navigate the page stack");
    eval("stack.next = page");
    click(Qt::ForwardButton);
    check(eval("stack.currentPage === page && stack.forwardCount === 1 && section === 3").toBool(),
          "forward did not navigate an attached page");
    eval("stack.currentPage = dialog; stack.next = page");
    click(Qt::ForwardButton);
    check(eval("stack.currentPage === dialog && stack.forwardCount === 1").toBool(),
          "forward accepted a dialog");
    eval("stack.currentPage = page; page.backNavigation = false");
    click(Qt::BackButton);
    check(eval("stack.currentPage === page && stack.backCount === 1").toBool(),
          "back ignored the page navigation policy");
    eval("stack.currentPage = launcher; stack.next = null; section = 1; blocked = true");
    click(Qt::ForwardButton);
    check(eval("section === 1").toBool(), "navigation bypassed modal overlay");
    eval("blocked = false; stack.busy = true");
    click(Qt::ForwardButton);
    check(eval("section === 1").toBool(), "navigation ignored a busy stack");
    eval("stack.busy = false");

    mouse(QEvent::MouseButtonPress, Qt::ForwardButton, Qt::ForwardButton);
    QEvent deactivate(QEvent::WindowDeactivate);
    QCoreApplication::sendEvent(&view, &deactivate);
    mouse(QEvent::MouseButtonPress, Qt::ForwardButton, Qt::ForwardButton);
    mouse(QEvent::MouseButtonRelease, Qt::ForwardButton, Qt::NoButton);
    check(eval("section === 3").toBool(), "deactivation did not reset held side buttons");
    return failed ? 1 : 0;
}
