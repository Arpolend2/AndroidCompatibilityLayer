#include "../shared_microg_module.h"

#include <QCoreApplication>
#include <QTemporaryDir>

#include <cstdio>
#include <sys/stat.h>

static bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    const QDir root(directory.path());
    const QString source = root.filePath(QStringLiteral("installed.apk"));
    const QString shared = root.filePath(QStringLiteral("common"));
    const QString target = SharedMicrogModule::path(shared);
    QString error;
    QByteArray digest;
    if (!writeFile(source, "first") ||
        !SharedMicrogModule::publish(source, shared, &error, &digest) ||
        digest != QCryptographicHash::hash("first", QCryptographicHash::Sha256)) return 1;
    struct stat first = {}, unchanged = {}, replaced = {};
    if (::stat(QFile::encodeName(target).constData(), &first) ||
        !SharedMicrogModule::publish(source, shared, &error) ||
        ::stat(QFile::encodeName(target).constData(), &unchanged) ||
        first.st_ino != unchanged.st_ino) return 1;
    if (!writeFile(source, "other") || !SharedMicrogModule::publish(source, shared, &error) ||
        ::stat(QFile::encodeName(target).constData(), &replaced) ||
        replaced.st_ino == first.st_ino) return 1;
    QFile copied(target);
    if (!copied.open(QIODevice::ReadOnly) || copied.readAll() != "other") return 1;
    copied.close();
    if (SharedMicrogModule::publish(root.filePath(QStringLiteral("missing")), shared, &error) ||
        error.isEmpty()) return 1;
    if (!copied.open(QIODevice::ReadOnly) || copied.readAll() != "other") return 1;
    copied.close();
    if (!QFile::remove(target) || !QFile::link(source, target) ||
        SharedMicrogModule::publish(source, shared, &error) || error.isEmpty()) return 1;
    std::puts("Shared microG module publication OK");
    return 0;
}
