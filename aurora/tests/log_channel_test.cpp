#include "../log_channel.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <cstdio>
#include <cstring>
#include <unistd.h>

static void pump()
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 150) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    LogChannel channel;
    if (!channel.available()) return 1;
    if (LogChannel::openSender(QStringLiteral("/nonexistent/megler-log-socket")) >= 0) return 1;
    int sender = LogChannel::openSender(channel.path());
    if (sender < 0) return 1;
    LogChannel::sendLine(0, "first \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", &sender);
    pump();
    if (!channel.text().contains(QString::fromUtf8("first \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82"))) return 1;
    ::close(sender);
    sender = LogChannel::openSender(channel.path());
    if (sender < 0) return 1;
    const QByteArray line(1000, 'x');
    for (int i = 0; i < 1000; ++i) {
        LogChannel::sendLine(0, line.constData(), &sender);
        if (i % 32 == 0) QCoreApplication::processEvents();
    }
    pump();
    if (channel.text().size() > 131072 || channel.text().isEmpty()) return 1;
    LogChannel::sendLine(0, "restart \xe2\x82", &sender);
    pump();
    if (!channel.text().contains(QString::fromUtf8("restart \xef\xbf\xbd"))) return 1;
    ::close(sender);
    std::puts("Core log channel OK");
    return 0;
}
