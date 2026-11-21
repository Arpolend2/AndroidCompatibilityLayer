#include "../standalone_microg_diagnostic_page.h"

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QEventLoop>
#include <QPointer>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>

#include <cstdio>

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "minimal");
    QGuiApplication app(argc, argv);
    auto *view = new QQuickView;
    view->rootContext()->setContextProperty(QStringLiteral("ownerStatus"), QStringLiteral("absent"));
    view->rootContext()->setContextProperty(QStringLiteral("replyStatus"), QStringLiteral("synthetic-ok"));
    QPointer<QQmlComponent> component = StandaloneMicrogDiagnostic::createPage(view);
    QObject *object = component->create(view->rootContext());
    if (!object) {
        std::fprintf(stderr, "%s\n", qPrintable(component->errorString()));
        delete view;
        return 1;
    }
    view->setContent(QUrl(), component.data(), object);
    if (component->parent() != view || view->status() != QQuickView::Ready) {
        delete view;
        return 1;
    }
    QQuickItem *root = qobject_cast<QQuickItem *>(object);
    QObject *content = object->findChild<QObject *>(QStringLiteral("content"));
    QObject *scroll = object->findChild<QObject *>(QStringLiteral("scroll"));
    QObject *button = object->findChild<QObject *>(QStringLiteral("continueButton"));
    if (!root || !content || !scroll || !button) {
        delete view;
        return 1;
    }
    for (const QSize &size : {QSize(960, 540), QSize(540, 960), QSize(480, 240)}) {
        root->setWidth(size.width());
        root->setHeight(size.height());
        app.processEvents();
        const qreal width = content->property("width").toReal();
        const qreal bottom = button->property("y").toReal() + button->property("height").toReal();
        if (width <= 0 || button->property("width").toReal() > width ||
            bottom > content->property("height").toReal() ||
            content->property("height").toReal() + 40 > scroll->property("contentHeight").toReal()) {
            std::fprintf(stderr, "Diagnostic layout failed at %dx%d\n", size.width(), size.height());
            delete view;
            return 1;
        }
    }
    QEventLoop loop;
    QObject::connect(object, SIGNAL(continueRequested()), &loop, SLOT(quit()));
    QTimer::singleShot(0, object, SIGNAL(continueRequested()));
    loop.exec();
    if (!component || view->status() != QQuickView::Ready) {
        delete view;
        return 1;
    }
    delete view;
    if (component) return 1;
    std::puts("Standalone diagnostic layout tests passed");
    return 0;
}
