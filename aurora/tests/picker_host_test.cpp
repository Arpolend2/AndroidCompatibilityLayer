#include "../picker_host.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
int replies = 0;
unsigned replyId;
int replyStatus;
QByteArray replyName;

void check(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

bool waitForReply(int count)
{
    QElapsedTimer timer;
    timer.start();
    while (replies < count && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return replies == count;
}
}

extern "C" void megler_pick_done(unsigned id, int status, int count, const char *const *names)
{
    ++replies;
    replyId = id;
    replyStatus = status;
    replyName = count == 1 && names ? QByteArray(names[0]) : QByteArray();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory failed");
    const QString folder = temporary.path() + QStringLiteral("/out");
    check(QDir().mkdir(folder), "destination directory failed");
    const QString source = temporary.path() + QStringLiteral("/selected.txt");
    QFile file(source);
    check(file.open(QIODevice::WriteOnly), "source open failed");
    check(file.write("picked") == 6, "source write failed");
    file.close();
    const QString wrongType = temporary.path() + QStringLiteral("/wrong.pdf");
    QFile wrong(wrongType);
    check(wrong.open(QIODevice::WriteOnly), "wrong type source open failed");
    wrong.write("not a text file");
    wrong.close();

    AuroraPickerHost picker;
    megler_system_host host = picker.host();
    check(host.permission && host.permission(host.user, "camera", false) == MEGLER_PERMISSION_GRANTED,
          "default permission changed");
    picker.setDeniedPermissions(QStringList() << QStringLiteral("camera"));
    check(host.permission(host.user, "camera", true) == MEGLER_PERMISSION_DENIED_FIXED &&
          host.permission(host.user, "location", false) == MEGLER_PERMISSION_GRANTED,
          "permission policy not applied");
    megler_pick_request request = {};
    request.id = 42;
    const QByteArray folderUtf8 = folder.toUtf8();
    request.folder = folderUtf8.constData();
    request.types = "text/plain\n";
    QString token;
    QStringList filters;
    int shown = 0;
    QObject::connect(&picker, &AuroraPickerHost::showPicker,
                     [&](const QString &t, const QStringList &f) { token = t; filters = f; ++shown; });
    check(!host.pick(host.user, &request), "accepted without UI");
    picker.setUiReady(true);
    request.multiple = true;
    check(!host.pick(host.user, &request), "accepted multiple");
    request.multiple = false;
    request.kind = MEGLER_PICK_PHOTO;
    check(!host.pick(host.user, &request), "accepted camera");
    request.kind = MEGLER_PICK_FILES;
    bool accepted = false;
    std::thread worker([&]() { accepted = host.pick(host.user, &request); });
    worker.join();
    check(accepted && !host.pick(host.user, &request), "busy handling failed");
    QCoreApplication::processEvents();
    check(shown == 1 && filters.contains(QStringLiteral("*.txt")), "picker not dispatched with MIME filter");
    picker.selected(token, source);
    check(waitForReply(1), "copy did not finish");
    check(replyId == 42 && replyStatus == MEGLER_PICK_OK && replyName == "selected.txt", "copy reply failed");
    QFile copy(folder + QLatin1Char('/') + QString::fromUtf8(replyName));
    check(copy.open(QIODevice::ReadOnly) && copy.readAll() == "picked", "copied content incorrect");
    copy.close();
    picker.canceled(token);
    check(replies == 1, "duplicate answer");

    check(host.pick(host.user, &request), "second request rejected");
    QCoreApplication::processEvents();
    picker.selected(token, wrongType);
    check(waitForReply(2) && replyStatus == MEGLER_PICK_FAILED, "invalid source did not fail");

    check(host.pick(host.user, &request), "cancel request rejected");
    QCoreApplication::processEvents();
    picker.canceled(token);
    check(replies == 3 && replyStatus == MEGLER_PICK_CANCELED, "cancel failed");

    check(host.pick(host.user, &request), "queued request rejected");
    picker.stop();
    check(replies == 4 && replyStatus == MEGLER_PICK_CANCELED, "stop did not cancel");
    picker.setUiReady(true);
    check(host.pick(host.user, &request), "request after stop rejected");
    const QString staleToken = token;
    const int oldShown = shown;
    QCoreApplication::processEvents();
    check(shown == oldShown + 1, "stale queued dispatch opened another picker");
    picker.canceled(staleToken);
    check(replies == 4, "stale cancellation affected new request");
    picker.selected(token, source);
    check(waitForReply(5) && replyStatus == MEGLER_PICK_OK && replyName == "1_selected.txt",
          "unique destination failed");

    request.types = "application/x-megler-unknown-type\n";
    check(host.pick(host.user, &request), "unknown type request rejected prematurely");
    QCoreApplication::processEvents();
    check(replies == 6 && replyStatus == MEGLER_PICK_FAILED, "unknown MIME type was not rejected");

    request.types = "*/*\n";
    check(host.pick(host.user, &request), "wildcard request rejected");
    QCoreApplication::processEvents();
    check(filters == QStringList(QStringLiteral("*")), "wildcard filter incorrect");
    picker.showFailed(token);
    check(replies == 7 && replyStatus == MEGLER_PICK_FAILED, "page creation failure not reported");

    check(host.pick(host.user, &request), "request before stop rejected");
    QCoreApplication::processEvents();
    picker.selected(token, source);
    picker.stop();
    QCoreApplication::processEvents();
    check(replies == 8 && replyStatus == MEGLER_PICK_CANCELED, "stop during copy answered twice");
    check(!QFileInfo::exists(folder + QStringLiteral("/2_selected.txt")), "canceled copy remained");
    std::puts("Picker host OK");
    return 0;
}
