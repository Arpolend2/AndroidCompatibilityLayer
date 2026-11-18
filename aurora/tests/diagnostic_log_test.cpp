#include "../diagnostic_log.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QTest>

#include <cstdio>
#include <cstring>
#include <unistd.h>

static bool waitFor(QProcess &a, QProcess &b, int timeout)
{
    QElapsedTimer timer;
    timer.start();
    while ((a.state() != QProcess::NotRunning || b.state() != QProcess::NotRunning) &&
           timer.elapsed() < timeout)
        QTest::qWait(10);
    return a.state() == QProcess::NotRunning && b.state() == QProcess::NotRunning;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc > 1 && QByteArray(argv[1]) == "--child") {
        const QByteArray id(argv[2]);
        const QByteArray first = id + " \xe2";
        ::write(1, first.constData(), size_t(first.size()));
        QTest::qWait(100);
        ::write(1, "\x82\xac\n", 3);
        ::write(2, "stderr test\n", 12);
        DiagnosticLog::installChildQtMessageHandler();
        qWarning("child Qt warning");
        ::write(2, "\xe2\x82", 2);
        return 0;
    }
    if (argc > 1 && QByteArray(argv[1]) == "--flood") {
        QByteArray data(4096, 'x');
        for (int i = 0; i < 2500; ++i) {
            int pos = 0;
            while (pos < data.size()) {
                const ssize_t written = ::write(1, data.constData() + pos, size_t(data.size() - pos));
                if (written <= 0) return 1;
                pos += int(written);
            }
        }
        ::write(1, "\nlast line\n", 11);
        return 0;
    }
    if (argc > 1 && QByteArray(argv[1]) == "--forward-test") {
        DiagnosticLog log;
        QString error;
        if (!log.start(&error)) return 1;
        ::write(1, "controller out\n", 15);
        ::write(2, "controller err\n", 15);
        QTest::qWait(150);
        QProcess child, unused;
        log.watch(&child, QStringLiteral("app"));
        child.start(app.applicationFilePath(), QStringList() << "--child" << "one");
        if (!waitFor(child, unused, 5000) || child.exitCode() != 0) return 1;
        QTest::qWait(250);
        return 0;
    }
    const bool blocked = argc > 1 && QByteArray(argv[1]) == "--blocked-test";
    if (!blocked) {
        QProcess forwarding, unused;
        forwarding.start(app.applicationFilePath(), QStringList() << "--forward-test");
        if (!waitFor(forwarding, unused, 10000) || forwarding.exitCode() != 0 ||
            forwarding.readAllStandardOutput() != QByteArray("controller out\none \xe2\x82\xac\n") ||
            forwarding.readAllStandardError() != QByteArray("controller err\nstderr test\nchild Qt warning\n\xe2\x82")) {
            std::fprintf(stderr, "IDE output did not preserve both byte streams\n");
            return 1;
        }
    }
    int pipeFd[2] = {-1, -1};
    int saved = -1;
    if (blocked) {
        if (::pipe(pipeFd) < 0 || (saved = ::dup(1)) < 0 || ::dup2(pipeFd[1], 1) < 0) return 1;
        ::close(pipeFd[1]);
    }
    int result = 0;
    {
        DiagnosticLog log;
        QString error;
        if (!log.start(&error)) {
            std::fprintf(stderr, "Capture setup failed: %s\n", qPrintable(error));
            return 1;
        }
        log.startNotifications();
        if (!blocked) {
            ::write(1, "controller out\n", 15);
            qWarning("controller warning");
            QProcess first, second;
            QByteArray observedOut, observedErr;
            log.watch(&first, QStringLiteral("app-one"), [&](bool isError, const QByteArray &bytes) {
                (isError ? observedErr : observedOut) += bytes;
            });
            log.watch(&second, QStringLiteral("app-two"));
            first.start(app.applicationFilePath(), QStringList() << "--child" << "one");
            second.start(app.applicationFilePath(), QStringList() << "--child" << "two");
            if (!waitFor(first, second, 10000)) result = 1;
            QTest::qWait(200);
            const QString text = log.text();
            if (!text.contains(QStringLiteral("[app-one stdout] one ")) ||
                !text.contains(QStringLiteral("[app-two stdout] two ")) ||
                text.count(QChar(0x20ac)) != 2 ||
                !text.contains(QStringLiteral("[app-one stderr] stderr test")) ||
                !text.contains(QStringLiteral("[app-two stderr] stderr test")) ||
                text.count(QStringLiteral("child Qt warning")) != 2 ||
                !text.contains(QChar::ReplacementCharacter) ||
                !text.contains(QStringLiteral("controller warning")) ||
                !text.contains(QStringLiteral("controller out")) ||
                observedOut != QByteArray("one \xe2\x82\xac\n") ||
                observedErr != QByteArray("stderr test\nchild Qt warning\n\xe2\x82")) {
                std::fprintf(stderr, "Missing or corrupted captured output: %s\n", qPrintable(text));
                result = 1;
            }
        }
        QProcess flood, unused;
        log.watch(&flood, QStringLiteral("flood"));
        flood.start(app.applicationFilePath(), QStringList() << "--flood");
        if (!waitFor(flood, unused, 30000)) {
            std::fprintf(stderr, "Logging blocked the child\n");
            result = 1;
        }
        QTest::qWait(200);
        const QString text = log.text();
        if (!text.contains(QStringLiteral("[Earlier log output was truncated or lost.]")) ||
            !text.contains(QStringLiteral("last line")) || text.size() > 132000 ||
            (blocked && !text.contains(QStringLiteral("[Some output could not be forwarded to the IDE.]")))) {
            std::fprintf(stderr, "Truncation or forwarding loss not reported\n");
            result = 1;
        }
    }
    if (blocked) {
        ::dup2(saved, 1);
        ::close(saved);
        ::close(pipeFd[0]);
    }
    if (!result) std::fprintf(stderr, "Diagnostic log OK\n");
    return result;
}
