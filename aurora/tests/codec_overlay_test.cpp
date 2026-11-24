#include "../error_log.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <cstdio>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir root;
    QString error;
    CHECK(root.isValid());
    CHECK(ErrorLog::initialize(QStringLiteral("Android App Manager"), &error));
    CHECK(ErrorLog::startPersistence(root.path(), &error));
    CHECK(ErrorLog::setDetailedEnabled(true, &error));
    const QString generic = QStringLiteral("codec: error, timeout or fallback");
    ErrorLog::record(QStringLiteral("controller stderr"),
                     QStringLiteral("media codec: an H.264 decoder closed after 60 samples, 60 pictures, "
                                    "0 blocks of sound, 0 failures\n"), false);
    CHECK(!ErrorLog::overlayText().contains(generic));
    ErrorLog::record(QStringLiteral("controller stderr"),
                     QStringLiteral("media codec: an H.264 decoder closed after 60 samples, 58 pictures, "
                                    "0 blocks of sound, 2 failures\n"), false);
    CHECK(ErrorLog::overlayText().contains(generic));
    const char *events[][2] = {
        {"ACL codec: rockchip mpp decoder initialized", "Codec HW: rockchip mpp decoder initialized"},
        {"ACL codec: rockchip mpp first picture", "Codec HW: rockchip mpp first picture"},
        {"ACL codec: rockchip mpp capability rejected", "Codec HW: rockchip mpp capability rejected"},
        {"ACL codec: rockchip mpp buffer synchronization failed", "Codec HW: rockchip mpp buffer synchronization failed"},
        {"ACL codec: rockchip mpp output frame error", "Codec HW: rockchip mpp output frame error"},
        {"ACL codec: rockchip mpp drain produced no pictures", "Codec HW: rockchip mpp drain produced no pictures"},
        {"ACL codec: hardware replay limit reached", "Codec HW: replay limit reached"},
        {"ACL codec: droidmedia hardware fallback active", "Codec HW: droidmedia fallback active"},
        {"ACL codec: droidmedia decoder initialization failed", "Codec HW: droidmedia initialization failed"},
    };
    for (const auto &event : events) {
        ErrorLog::record(QStringLiteral("controller stderr"),
                         QString::fromLatin1(event[0]) + QStringLiteral(" PRIVATE-DATA\n"), false);
        CHECK(ErrorLog::overlayText().contains(QString::fromLatin1(event[1])));
        CHECK(!ErrorLog::overlayText().contains(QStringLiteral("PRIVATE-DATA")));
    }
    std::puts("codec overlay: OK");
    return 0;
}
