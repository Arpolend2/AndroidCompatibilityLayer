#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>

#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QGuiApplication app(argc, argv);
    if (argc != 2) return 2;

    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (component.status() != QQmlComponent::Ready) {
        std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
        return 1;
    }

    std::puts("Yandex form QML imports and syntax OK");
    return 0;
}
