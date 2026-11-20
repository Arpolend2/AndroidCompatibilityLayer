#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickView>
#include <QSignalSpy>
#include <QTest>
#include <QVariant>

#include <cstdio>

class FakeProbe : public QObject
{
    Q_OBJECT
public:
    QString report = QStringLiteral("Safe test report");
    Q_INVOKABLE QString diagnosticReport() const { return report; }
    Q_INVOKABLE QString saveCoreLog() { ++saveCount; return QStringLiteral("Saved"); }
    Q_INVOKABLE QString openYandexLogForm() { ++openYandexCount; return yandexFormError; }
    int saveCount = 0;
    int openYandexCount = 0;
    QString yandexFormError;
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 2) return 2;
    QQuickView view;
    FakeProbe probe;
    view.rootContext()->setContextProperty(QStringLiteral("probe"), &probe);
    view.resize(480, 854);
    view.setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    if (view.status() != QQuickView::Ready) return 1;
    QObject *root = view.rootObject();
    QSignalSpy openFormRequested(root, SIGNAL(openYandexFormRequested()));
    if (!openFormRequested.isValid()) return 1;
    if (root->property("canAct").toBool() ||
        root->findChild<QObject *>(QStringLiteral("logShareAction"))) {
        std::fprintf(stderr, "Initial action state incorrect\n");
        return 1;
    }
    const QString text = QString::fromUtf8("sample \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\n");
    root->setProperty("logText", text);
    root->setProperty("logAvailable", true);
    if (!QMetaObject::invokeMethod(root, "openYandexForm") ||
        probe.openYandexCount != 1 || openFormRequested.count() != 1 ||
        probe.saveCount != 0) {
        std::fprintf(stderr, "Form action did not request the embedded form safely\n");
        return 1;
    }
    probe.yandexFormError = QStringLiteral("Aurora WebView is unavailable.");
    if (!QMetaObject::invokeMethod(root, "openYandexForm") ||
        probe.openYandexCount != 2 || openFormRequested.count() != 1 ||
        root->property("message").toString() != probe.yandexFormError ||
        probe.saveCount != 0) {
        std::fprintf(stderr, "Unavailable form was not reported without opening it\n");
        return 1;
    }
    if (!root->property("canAct").toBool() || !QMetaObject::invokeMethod(root, "copyLog")) {
        std::fprintf(stderr, "Actions did not enable or copy failed\n");
        return 1;
    }
    QQmlExpression clipboard(qmlContext(root), root, QStringLiteral("Clipboard.text"));
    if (clipboard.evaluate().toString() != probe.report || clipboard.hasError()) {
        std::fprintf(stderr, "Clipboard did not receive the safe report\n");
        return 1;
    }
    probe.report = QStringLiteral("Updated safe report");
    if (!QMetaObject::invokeMethod(root, "shareLog")) return 1;
    QTest::qWait(100);
    QObject *action = root->findChild<QObject *>(QStringLiteral("logShareAction"));
    if (!action) {
        std::fprintf(stderr, "Share action did not load: %s\n", qPrintable(root->property("message").toString()));
        return 1;
    }
    const QVariantList resources = action->property("resources").toList();
    if (resources.size() != 1 || resources.first().toMap().value(QStringLiteral("data")).toString() != probe.report) {
        std::fprintf(stderr, "Share action did not receive the updated safe report\n");
        return 1;
    }
    probe.report = QStringLiteral("Another safe report");
    if (!QMetaObject::invokeMethod(root, "shareLog")) return 1;
    const QVariantList updatedResources = action->property("resources").toList();
    if (updatedResources.size() != 1 ||
        updatedResources.first().toMap().value(QStringLiteral("data")).toString() != probe.report) return 1;
    std::puts("Log actions QML OK");
    return 0;
}

#include "log_actions_qml_test.moc"
