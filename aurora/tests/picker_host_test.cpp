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
QList<QByteArray> replyNames;

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
    replyNames.clear();
    for (int i = 0; i < count; ++i) replyNames.append(QByteArray(names[i]));
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
                     [&](const QString &t, const QStringList &f, bool) { token = t; filters = f; ++shown; });
    check(!host.pick(host.user, &request), "accepted without UI");
    picker.setUiReady(true);
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
    picker.setUiReady(true);
    request.multiple = true;
    request.max = 2;
    check(host.pick(host.user, &request), "multiple request rejected");
    QCoreApplication::processEvents();
    picker.selectedMany(token, QStringList() << source << source);
    check(waitForReply(9) && replyStatus == MEGLER_PICK_OK && replyNames.size() == 2 &&
          replyNames[0] == "2_selected.txt" && replyNames[1] == "3_selected.txt",
          "multiple selection or unique names failed");
    check(host.pick(host.user, &request), "limited request rejected");
    QCoreApplication::processEvents();
    picker.selectedMany(token, QStringList() << source << source << source);
    check(replies == 10 && replyStatus == MEGLER_PICK_FAILED, "selection limit ignored");
    request.types = "text/plain\n";
    check(host.pick(host.user, &request), "partial failure request rejected");
    QCoreApplication::processEvents();
    picker.selectedMany(token, QStringList() << source << wrongType);
    check(waitForReply(11) && replyStatus == MEGLER_PICK_FAILED &&
          !QFileInfo::exists(folder + QStringLiteral("/4_selected.txt")), "partial copy not rolled back");

    request.kind = MEGLER_PICK_VIDEO;
    check(!host.pick(host.user, &request), "video capture unexpectedly accepted");
    picker.setDeniedPermissions(QStringList());
    request.kind = MEGLER_PICK_PHOTO;
    QString photoPath;
    QObject::connect(&picker, &AuroraPickerHost::showPhoto,
                     [&](const QString &t, const QString &path) { token = t; photoPath = path; });
    bool validPhoto = false;
    int validations = 0;
    QObject::connect(&picker, &AuroraPickerHost::photoValidated,
                     [&](const QString &, bool valid) { validPhoto = valid; ++validations; });
    check(host.pick(host.user, &request), "photo request rejected");
    QCoreApplication::processEvents();
    check(!photoPath.isEmpty() && !QFileInfo::exists(photoPath), "private photo path missing");
    picker.confirmPhoto(token);
    check(replies == 11, "unconfirmed photo was returned");
    QFile photo(photoPath);
    check(photo.open(QIODevice::WriteOnly), "photo file open failed");
    check(photo.write(QByteArray::fromHex("ffd8ff0102")) == 5, "photo write failed");
    photo.close();
    picker.validatePhoto(token, source);
    picker.validatePhoto(QStringLiteral("stale"), photoPath);
    check(validations == 1 && !validPhoto, "untrusted photo path or stale token accepted");
    picker.validatePhoto(token, photoPath);
    check(validations == 2 && validPhoto, "saved photo rejected");
    check(!QFileInfo::exists(folder + QStringLiteral("/photo.jpg")), "photo copied before confirmation");
    const QString canceledPhoto = photoPath;
    const QString canceledToken = token;
    picker.canceled(token);
    check(replies == 12 && replyStatus == MEGLER_PICK_CANCELED && !QFileInfo::exists(canceledPhoto),
          "photo cancellation failed");
    picker.validatePhoto(canceledToken, canceledPhoto);
    check(validations == 2, "late photo callback accepted");

    check(host.pick(host.user, &request), "second photo request rejected");
    QCoreApplication::processEvents();
    picker.validatePhoto(canceledToken, photoPath);
    check(token != canceledToken && validations == 2, "stale photo affected new request");
    QFile invalidPhoto(photoPath);
    check(invalidPhoto.open(QIODevice::WriteOnly), "invalid photo open failed");
    invalidPhoto.write("not jpeg");
    invalidPhoto.close();
    picker.validatePhoto(token, photoPath);
    check(validations == 3 && !validPhoto, "invalid photo accepted");
    check(invalidPhoto.open(QIODevice::WriteOnly | QIODevice::Truncate), "photo rewrite failed");
    invalidPhoto.write(QByteArray::fromHex("ffd8ff0304"));
    invalidPhoto.close();
    picker.validatePhoto(token, photoPath);
    check(validations == 4 && validPhoto, "valid photo not accepted");
    picker.confirmPhoto(token);
    check(waitForReply(13) && replyStatus == MEGLER_PICK_OK && replyName == "photo.jpg",
          "confirmed photo not returned");
    QFile result(folder + QStringLiteral("/photo.jpg"));
    check(result.open(QIODevice::ReadOnly) && result.readAll() == QByteArray::fromHex("ffd8ff0304"),
          "confirmed photo contents incorrect");
    result.close();
    check(!QFileInfo::exists(photoPath), "private photo was not removed");
    check(host.pick(host.user, &request), "photo before shutdown rejected");
    QCoreApplication::processEvents();
    int closed = 0;
    QObject::connect(&picker, &AuroraPickerHost::closePhoto, [&](const QString &t) {
        if (t == token) ++closed;
    });
    picker.stop();
    check(closed == 1 && replies == 14 && replyStatus == MEGLER_PICK_CANCELED,
          "shutdown did not close and cancel camera");
    picker.confirmPhoto(token);
    check(replies == 14, "late confirmation produced a reply");
    QObject::disconnect(&picker, nullptr, nullptr, nullptr);
    picker.setUiReady(true);
    check(host.pick(host.user, &request), "unconnected photo request rejected prematurely");
    QCoreApplication::processEvents();
    check(replies == 15 && replyStatus == MEGLER_PICK_FAILED,
          "unconnected photo UI did not fail explicitly");
    picker.setPickerCapabilities(false, false);
    check(!host.pick(host.user, &request), "legacy remote accepted unsupported photo");
    request.kind = MEGLER_PICK_FILES;
    request.multiple = true;
    check(!host.pick(host.user, &request), "legacy remote accepted unsupported multiple selection");
    std::puts("Picker host OK");
    return 0;
}
