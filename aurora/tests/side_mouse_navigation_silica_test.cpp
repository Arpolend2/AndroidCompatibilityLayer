#include "../side_mouse_buttons.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv)
{
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &msg) {
        std::fprintf(stderr, "Qt: %s\n", qPrintable(msg));
    });
    QGuiApplication app(argc, argv);
    if (argc != 2) return 2;
    qmlRegisterType<SideMouseButtons>("Megler.Navigation", 1, 0, "SideMouseButtons");
    QQuickView view;
    view.rootContext()->setContextProperty(QStringLiteral("testWindow"), &view);
    view.resize(540, 960);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (view.status() != QQuickView::Ready) {
        for (const QQmlError &error : view.errors())
            std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
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
    auto click = [&view](Qt::MouseButton button) {
        QMouseEvent down(QEvent::MouseButtonPress, QPointF(20, 20), button, button, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &down);
        QMouseEvent up(QEvent::MouseButtonRelease, QPointF(20, 20), button, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(&view, &up);
    };
    auto check = [&failed](bool condition, const char *message) {
        if (!condition) {
            std::fprintf(stderr, "%s\n", message);
            failed = true;
        }
    };
    QTimer::singleShot(200, [&]() {
        click(Qt::ForwardButton);
        check(action("launcher.section === 1").toBool(), "forward did not change tab");
        action("openPage()");
    });
    QTimer::singleShot(400, [&]() {
        click(Qt::BackButton);
        check(action("pageStack.currentPage === launcher && launcher.section === 1").toBool(),
              "back did not leave details page");
        action("openDialog()");
    });
    QTimer::singleShot(600, [&]() {
        click(Qt::ForwardButton);
        check(action("accepted === 0 && pageStack.currentPage !== launcher").toBool(),
              "forward accepted or closed dialog");
        click(Qt::BackButton);
    });
    QTimer::singleShot(800, [&]() {
        check(action("accepted === 0 && rejected === 1 && pageStack.currentPage === launcher").toBool(),
              "back did not safely reject dialog");
        app.exit(failed ? 1 : 0);
    });
    return app.exec();
}
