#include "rpm_shortcut.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QRegExp>
#include <QStandardPaths>

#include "rpm_signers.h"
#include "rpm_test_keys.h"
#include "rpm_writer.h"
#include "rpm_icon.h"

namespace {

const char kMainBinary[] = "/usr/bin/ru.androidcompatibilitylayer.AndroidCompatibilityLayer";
const char kMainIcon[] = "/usr/share/icons/hicolor/172x172/apps/ru.androidcompatibilitylayer.AndroidCompatibilityLayer.png";

QImage iconImage(const QString &dataUri)
{
    QImage image;
    const int comma = dataUri.indexOf(QLatin1Char(','));
    if (comma > 0)
        image.loadFromData(QByteArray::fromBase64(dataUri.mid(comma + 1).toLatin1()));
    if (image.isNull()) image.load(QString::fromLatin1(kMainIcon));
    return image;
}

std::string cleanLine(const QString &text)
{
    QString value = text;
    value.replace(QRegExp(QStringLiteral("[\\r\\n\\t]+")), QStringLiteral(" "));
    return value.trimmed().toUtf8().toStdString();
}

}

QString RpmShortcut::create(const QString &package, const QString &label, const QString &iconDataUri, QString *error)
{
    auto fail = [error](const QString &message) -> QString {
        if (error) *error = message;
        return QString();
    };
    if (!QRegExp(QStringLiteral("[A-Za-z0-9_.]+")).exactMatch(package))
        return fail(QCoreApplication::translate("RpmShortcut", "Unsupported package name."));

    const QByteArray hash = QCryptographicHash::hash(package.toUtf8(), QCryptographicHash::Md5).toHex().left(8);
    const std::string name = "ru.androidcompatibilitylayer.Shortcut" + std::string(hash.constData());
    const qint64 now = QDateTime::currentMSecsSinceEpoch() / 1000;

    RpmWriter::Package pkg;
    pkg.name = name;
    pkg.version = "1.0.0";
    pkg.release = std::to_string(now);
    pkg.summary = "AndroidCompatibilityLayer app shortcut";
    pkg.description = "App grid shortcut for " + package.toStdString();
    pkg.buildTime = uint32_t(now);

    const std::string title = cleanLine(label.isEmpty() ? package : label);
    RpmWriter::File desktop;
    desktop.path = "/usr/share/applications/" + name + ".desktop";
    desktop.content = "[Desktop Entry]\nType=Application\nX-Nemo-Application-Type=silica-qt5\nIcon=" + name
        + "\nExec=" + kMainBinary + " --run-package " + package.toStdString()
        + "\nName=" + title + "\n\n[X-Application]\nOrganizationName=ru.androidcompatibilitylayer\nApplicationName=AndroidCompatibilityLayer\n";
    pkg.files.push_back(desktop);

    const QImage image = iconImage(iconDataUri);
    const int iconSizes[] = {86, 108, 128, 172};
    for (int size : iconSizes) {
        const QByteArray icon = RpmIcon::png(image, size);
        if (icon.isEmpty())
            return fail(QCoreApplication::translate("RpmShortcut", "Could not create the app icon."));
        RpmWriter::File png;
        png.path = "/usr/share/icons/hicolor/" + std::to_string(size) + "x" + std::to_string(size)
            + "/apps/" + name + ".png";
        png.content.assign(icon.constData(), size_t(icon.size()));
        pkg.files.push_back(png);
    }

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/shortcut-rpm");
    if (!QDir().mkpath(dir)) return fail(QCoreApplication::translate("RpmShortcut", "Could not create the package folder."));
    const QString prefix = QString::fromStdString(name);
    const QStringList old = QDir(dir).entryList(QStringList() << prefix + QStringLiteral("-*.rpm"), QDir::Files);
    for (const QString &file : old) QFile::remove(QDir(dir).filePath(file));

    const std::string data = RpmWriter::build(pkg);
    const QString path = QDir(dir).filePath(prefix + QStringLiteral("-1.0.0-") + QString::number(now)
                                            + QStringLiteral(".noarch.rpm"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(data.data(), qint64(data.size())) != qint64(data.size())) {
        file.close();
        QFile::remove(path);
        return fail(QCoreApplication::translate("RpmShortcut", "Could not write the package."));
    }
    file.close();

    std::string log;
    if (!RpmSigners::signWithLibrary(path.toStdString(), kTestKeyPem, kTestCertPem, log)) {
        QFile::remove(path);
        return fail(QCoreApplication::translate("RpmShortcut", "Could not sign the package: %1")
                        .arg(QString::fromStdString(log).trimmed()));
    }
    return path;
}
