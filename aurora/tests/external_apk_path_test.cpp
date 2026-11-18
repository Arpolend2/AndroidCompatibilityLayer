#include "../external_apk_path.h"

#include <QCoreApplication>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    struct Case { const char *input; const char *expected; };
    const Case cases[] = {
        {"/home/user/Downloads/My app.apk", "/home/user/Downloads/My app.apk"},
        {"file:///home/user/Downloads/My%20app.APK", "/home/user/Downloads/My app.APK"},
        {"file:///home/user/Downloads/%D1%84%D0%B0%D0%B9%D0%BB.apk", "/home/user/Downloads/\xd1\x84\xd0\xb0\xd0\xb9\xd0\xbb.apk"},
        {"https://example.com/app.apk", nullptr},
        {"file://otherhost/home/user/app.apk", nullptr},
        {"file:///home/user/app.apk?query", nullptr},
        {"file:///home/user/app.apk#fragment", nullptr},
        {"file:///home/user/%00evil.apk", nullptr},
        {"file:///home/user/%2e%2e/app.apk", nullptr},
        {"file:///home/user/%GG.apk", nullptr},
        {"/home/user/app.xapk", nullptr},
        {"relative.apk", nullptr},
        {"//otherhost/app.apk", nullptr},
        {"/app.apk", nullptr},
    };
    for (const Case &test : cases) {
        QString path, error;
        const bool ok = externalApkPath(QString::fromUtf8(test.input), &path, &error);
        if (ok != (test.expected != nullptr) ||
            (ok && path != QString::fromUtf8(test.expected)) || (!ok && error.isEmpty())) {
            std::fprintf(stderr, "Unexpected result for %s\n", test.input);
            return 1;
        }
        for (const char *name : {"file:///home/user/My%20app.XAPK", "/home/user/app.apks"}) {
            QString path, error;
            if (!externalApkPath(QString::fromUtf8(name), &path, &error, true) || path.isEmpty()) return 1;
        }
        for (const char *name : {"file://otherhost/home/app.xapk", "/home/user/../app.apks",
                                 "/home/user/app.apkm", "https://example.com/app.xapk"}) {
            QString path, error;
            if (externalApkPath(QString::fromUtf8(name), &path, &error, true) || error.isEmpty()) return 1;
        }
    }
    std::puts("External APK path validation OK");
    return 0;
}
