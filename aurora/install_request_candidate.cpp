#include "install_request_candidate.h"

#include "../core/src/apk/mg_apk.h"

#include <QObject>
#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>

namespace {
bool validPackage(const QString &package)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+$"));
    return package.size() <= 255 && pattern.match(package).hasMatch();
}

QString safeLabel(const char *label, const QString &fallback)
{
    const QString text = QString::fromUtf8(label).trimmed();
    for (QChar c : text) {
        const QChar::Category category = c.category();
        if (category == QChar::Other_Control || category == QChar::Other_Format ||
            category == QChar::Other_Surrogate || category == QChar::Other_NotAssigned)
            return fallback;
    }
    return text.isEmpty() ? fallback : text;
}
}

void InstallRequestCandidate::clear()
{
    m_snapshot.clear();
    m_package.clear();
    m_label.clear();
    m_version = 0;
    m_fingerprint.clear();
}

bool InstallRequestCandidate::prepare(const QString &senderRoot, const QString &source,
                                      const QString &name, quint64 maxBytes, QString *error, bool fingerprint)
{
    clear();
    if (error) error->clear();
    std::string captureError;
    if (!m_snapshot.capture(senderRoot.toUtf8().toStdString(), source.toUtf8().toStdString(),
                            name.toUtf8().toStdString(), maxBytes, &captureError)) {
        if (error) *error = QString::fromStdString(captureError);
        return false;
    }

    mg_apk_info info;
    const std::string snapshotPath = m_snapshot.path();
    if (fingerprint) {
        QFile snapshot(QString::fromStdString(snapshotPath));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!snapshot.open(QIODevice::ReadOnly) || !hash.addData(&snapshot)) {
            if (error) *error = QObject::tr("Cannot verify the APK snapshot.");
            clear();
            return false;
        }
        m_fingerprint = hash.result().toHex();
    }
    if (!mg_apk_read_info(snapshotPath.c_str(), &info)) {
        if (error) *error = QObject::tr("Cannot inspect the requested APK.");
        clear();
        return false;
    }
    if (info.split[0] || info.config_for_split[0] || info.feature_split || info.split_required ||
        info.required_split_types[0]) {
        if (error) *error = QObject::tr("Split APKs are not supported.");
        clear();
        return false;
    }
    const QString package = QString::fromUtf8(info.package);
    if (!validPackage(package) || !info.runnable) {
        if (error) *error = QObject::tr("The requested APK is invalid or cannot be started.");
        clear();
        return false;
    }
    if (name.startsWith(QLatin1String("request_")) && name.size() > 44) {
        const int separator = name.indexOf(QLatin1Char('_'), 41);
        bool validVersion = false;
        const quint32 expectedVersion = name.mid(41, separator - 41).toUInt(&validVersion);
        const QString expected = name.mid(separator + 1, name.size() - separator - 5);
        if (separator < 0 || !validVersion || package != expected ||
            static_cast<quint32>(info.version_code) != expectedVersion) {
            if (error) *error = QObject::tr("The APK package or version does not match the installation session.");
            clear();
            return false;
        }
    }
    m_package = package;
    m_label = safeLabel(info.label, package);
    m_version = static_cast<quint32>(info.version_code);
    return true;
}

QString InstallRequestCandidate::path() const
{
    return QString::fromStdString(m_snapshot.path());
}
