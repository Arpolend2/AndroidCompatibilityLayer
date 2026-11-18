#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>

#include <cstdio>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 2) return 2;
    QQmlPropertyMap probe;
    probe.insert(QStringLiteral("deviceOperatingSystem"), QStringLiteral("Test OS"));
    probe.insert(QStringLiteral("deviceCpuArchitecture"), QStringLiteral("test-cpu"));
    QQuickView view;
    view.rootContext()->setContextProperty(QStringLiteral("probe"), &probe);
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.resize(480, 854);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (view.status() != QQuickView::Ready) return 1;
    QTest::qWait(100);

    QQuickItem *root = view.rootObject();
    for (QQuickItem *item : root->childItems()) {
        const QString text = item->property("text").toString();
        if (text == QStringLiteral("Device") || text == QStringLiteral("Display")) {
            std::fprintf(stderr, "Unexpected device information heading\n");
            return 1;
        }
    }
    QObject *model = root->findChild<QObject *>(QStringLiteral("deviceModel"));
    QObject *os = root->findChild<QObject *>(QStringLiteral("deviceOperatingSystem"));
    QObject *arch = root->findChild<QObject *>(QStringLiteral("deviceArchitecture"));
    QObject *screen = root->findChild<QObject *>(QStringLiteral("deviceScreenSize"));
    if (model || !os || !arch || !screen ||
        os->property("value").toString() != QStringLiteral("Test OS") ||
        arch->property("value").toString() != QStringLiteral("test-cpu") ||
        screen->property("value").toString().isEmpty()) {
        std::fprintf(stderr, "Device information did not display the supplied data\n");
        return 1;
    }

    probe.insert(QStringLiteral("deviceOperatingSystem"), QString());
    probe.insert(QStringLiteral("deviceCpuArchitecture"), QString());
    QCoreApplication::processEvents();
    if (os->property("visible").toBool() || arch->property("visible").toBool()) {
        std::fprintf(stderr, "Unavailable device data was not hidden\n");
        return 1;
    }
    std::puts("Device information QML OK");
    return 0;
}
