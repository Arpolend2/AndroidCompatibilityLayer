#include "../standalone_start.h"
#include <RuntimeManager/IntentsInvoker>

#include <QCoreApplication>
#include <QTimer>
#include <cstdio>

#define CHECK(expression) do { if (!(expression)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #expression); return 1; \
} } while (false)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString id = QStringLiteral("ru.androidcompatibilitylayer.Android0123456789abcdef01234567");
    QString error;
    StandaloneStart start;
    auto *invoker = start.findChild<RuntimeManager::IntentsInvoker *>();
    auto *timeout = start.findChild<QTimer *>();
    CHECK(invoker && timeout);
    int calls = 0;
    bool accepted = false;
    QString result;
    QObject::connect(&start, &StandaloneStart::finished, [&](bool ok, const QString &message) {
        ++calls; accepted = ok; result = message;
    });
    CHECK(!start.start(QStringLiteral("ru.example.Other"), &error) && !error.isEmpty());
    CHECK(!start.start(id + QLatin1Char('x'), &error));
    CHECK(start.start(id, &error) && error.isEmpty());
    CHECK(invoker->intentName == QLatin1String("Start") &&
          invoker->intentHints.value(QStringLiteral("preferredHandler")).toString() == id &&
          invoker->intentHints.value(QStringLiteral("onlyPreferred")).toBool() &&
          invoker->intentParams.isEmpty() && timeout->isActive());
    CHECK(!start.start(id, &error));
    invoker->complete();
    CHECK(calls == 1 && accepted && result.isEmpty() && !timeout->isActive());
    invoker->complete(RuntimeManager::Error(QStringLiteral("late")));
    CHECK(calls == 1);

    StandaloneStart failure;
    auto *failureInvoker = failure.findChild<RuntimeManager::IntentsInvoker *>();
    QObject::connect(&failure, &StandaloneStart::finished, [&](bool ok, const QString &message) {
        ++calls; accepted = ok; result = message;
    });
    CHECK(failure.start(id, &error));
    failureInvoker->complete(RuntimeManager::Error(QStringLiteral("unavailable")));
    CHECK(calls == 2 && !accepted && result == QLatin1String("unavailable"));

    StandaloneStart timed;
    auto *timedInvoker = timed.findChild<RuntimeManager::IntentsInvoker *>();
    auto *timer = timed.findChild<QTimer *>();
    QObject::connect(&timed, &StandaloneStart::finished, [&](bool ok, const QString &message) {
        ++calls; accepted = ok; result = message;
    });
    CHECK(timed.start(id, &error));
    CHECK(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
    CHECK(calls == 3 && !accepted && !result.isEmpty());
    timedInvoker->complete();
    CHECK(calls == 3);
    return 0;
}
