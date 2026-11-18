#include "microg_install.h"

#include "../core/src/apk/mg_apk.h"
#include "external_apk_path.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>

namespace {
const qint64 MaxMetadata = 1024 * 1024;
const qint64 MaxApk = 512LL * 1024 * 1024;

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
}

bool microgDownloadUrlAllowed(const QUrl &url)
{
    const QString host = url.host().toLower();
    return url.isValid() && url.scheme() == QLatin1String("https") &&
        url.userInfo().isEmpty() && (url.port() == -1 || url.port() == 443) &&
        url.fragment().isEmpty() &&
        (host == QLatin1String("github.com") || host == QLatin1String("api.github.com") ||
         host == QLatin1String("release-assets.githubusercontent.com") ||
         host == QLatin1String("objects.githubusercontent.com") ||
         host == QLatin1String("github-releases.githubusercontent.com"));
}

bool microgRelease(const QByteArray &json, MicrogRelease *release, QString *error)
{
    if (error) error->clear();
    if (!release || json.size() > MaxMetadata)
        return fail(error, QObject::tr("Invalid microG release metadata."));
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
        return fail(error, QObject::tr("Cannot read microG release metadata."));
    const QJsonObject root = doc.object();
    const QString tag = root.value(QStringLiteral("tag_name")).toString();
    static const QRegularExpression tagPattern(QStringLiteral("^[A-Za-z0-9._-]{1,100}$"));
    static const QRegularExpression assetPattern(QStringLiteral("^com\\.google\\.android\\.gms-[0-9]+\\.apk$"));
    static const QRegularExpression digestPattern(QStringLiteral("^sha256:([a-f0-9]{64})$"));
    if (!tagPattern.match(tag).hasMatch() || root.value(QStringLiteral("draft")).toBool() ||
        root.value(QStringLiteral("prerelease")).toBool() || !root.value(QStringLiteral("assets")).isArray())
        return fail(error, QObject::tr("The latest microG release is invalid."));
    int found = 0;
    MicrogRelease candidate;
    for (const QJsonValue &value : root.value(QStringLiteral("assets")).toArray()) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        if (!assetPattern.match(name).hasMatch()) continue;
        ++found;
        const QRegularExpressionMatch digest = digestPattern.match(asset.value(QStringLiteral("digest")).toString());
        const double size = asset.value(QStringLiteral("size")).toDouble(-1);
        const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString(), QUrl::StrictMode);
        const QString expected = QStringLiteral("/microg/GmsCore/releases/download/") + tag + QLatin1Char('/') + name;
        if (!digest.hasMatch() || size < 1 || size > MaxApk || size != static_cast<qint64>(size) ||
            !microgDownloadUrlAllowed(url) || url.host() != QLatin1String("github.com") ||
            url.path() != expected || url.hasQuery())
            return fail(error, QObject::tr("The microG release asset has invalid provenance or checksum."));
        candidate.url = url;
        candidate.sha256 = QByteArray::fromHex(digest.captured(1).toLatin1());
        candidate.size = static_cast<qint64>(size);
    }
    if (found != 1)
        return fail(error, QObject::tr("The release does not contain exactly one supported microG APK."));
    *release = candidate;
    return true;
}

MicrogInstall::MicrogInstall(const QString &destination, QObject *parent)
    : QObject(parent), m_destination(destination), m_hash(QCryptographicHash::Sha256)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this]() {
        finish(false, tr("The microG download timed out."));
    });
}

bool MicrogInstall::start(const QString &localPath, const std::function<bool()> &canPublish, QString *error)
{
    if (error) error->clear();
    if (m_busy || m_destination.isEmpty() || !canPublish || !canPublish())
        return fail(error, tr("Close all Android apps before installing or updating microG."));
    m_localPath.clear();
    if (!localPath.isEmpty() && !externalApkPath(localPath, &m_localPath, error)) return false;
    if (!QDir().mkpath(QFileInfo(m_destination).absolutePath()))
        return fail(error, tr("Cannot create microG storage."));
    m_lock.reset(new QLockFile(m_destination + QStringLiteral(".lock")));
    m_lock->setStaleLockTime(0);
    if (!m_lock->tryLock(0)) {
        m_lock.reset();
        return fail(error, tr("Another microG update is in progress."));
    }
    m_busy = true;
    m_canPublish = canPublish;
    m_stage = Metadata;
    m_redirects = 0;
    m_metadata.clear();
    if (m_localPath.isEmpty())
        request(QUrl(QStringLiteral("https://api.github.com/repos/microg/GmsCore/releases/latest")));
    else
        QTimer::singleShot(0, this, &MicrogInstall::beginPayload);
    return true;
}

void MicrogInstall::request(const QUrl &url)
{
    if (!microgDownloadUrlAllowed(url) || ++m_redirects > 6 ||
        (m_stage == Metadata && url.host() != QLatin1String("api.github.com"))) {
        finish(false, tr("The microG download redirected to an untrusted address."));
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("Accept", m_stage == Metadata ? "application/vnd.github+json" : "application/octet-stream");
    request.setRawHeader("User-Agent", "Megler-Aurora");
    m_reply = m_network.get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, &MicrogInstall::readReply);
    connect(m_reply, &QNetworkReply::finished, this, &MicrogInstall::replyFinished);
    m_timeout.start(60000);
}

void MicrogInstall::readReply()
{
    if (!m_reply || !m_busy) return;
    m_timeout.start(60000);
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        m_reply->readAll();
        return;
    }
    const QByteArray bytes = m_reply->readAll();
    if (m_stage == Metadata) {
        if (bytes.size() > MaxMetadata - m_metadata.size()) {
            finish(false, tr("microG release metadata is too large."));
            return;
        }
        m_metadata += bytes;
    } else if (!append(bytes)) {
        finish(false, tr("Cannot save the microG APK or its size exceeds the release metadata."));
    } else if (m_localPath.isEmpty()) {
        const int percent = int((m_received * 100) / m_release.size);
        if (percent != m_lastProgress) {
            m_lastProgress = percent;
            emit downloadProgress(percent);
        }
    }
}

void MicrogInstall::replyFinished()
{
    if (!m_reply || !m_busy) return;
    readReply();
    if (!m_reply || !m_busy) return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    m_timeout.stop();
    const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString detail = reply->errorString();
    const QUrl resolved = reply->url().resolved(redirect);
    reply->deleteLater();
    if (!redirect.isEmpty() && networkError == QNetworkReply::NoError && status >= 300 && status < 400) {
        request(resolved);
        return;
    }
    if (networkError != QNetworkReply::NoError || status != 200) {
        finish(false, tr("Cannot download microG: %1 (HTTP %2).").arg(detail).arg(status));
        return;
    }
    if (m_stage == Metadata) {
        QString error;
        if (!microgRelease(m_metadata, &m_release, &error)) {
            finish(false, error);
            return;
        }
        beginPayload();
    } else {
        publish();
    }
}

void MicrogInstall::beginPayload()
{
    const QFileInfo target(m_destination);
    if (!QDir().mkpath(target.absolutePath())) {
        finish(false, tr("Cannot create microG storage."));
        return;
    }
    m_output.reset(new QSaveFile(m_destination));
    m_output->setDirectWriteFallback(false);
    if (!m_output->open(QIODevice::WriteOnly)) {
        finish(false, tr("Cannot stage the microG APK: %1").arg(m_output->errorString()));
        return;
    }
    m_hash.reset();
    m_received = 0;
    m_stage = Download;
    if (m_localPath.isEmpty()) {
        m_redirects = 0;
        m_lastProgress = 0;
        emit downloadProgress(0);
        request(m_release.url);
        return;
    }
    m_input.setFileName(m_localPath);
    if (!m_input.open(QIODevice::ReadOnly)) {
        finish(false, tr("Cannot open the selected APK: %1").arg(m_input.errorString()));
        return;
    }
    QTimer::singleShot(0, this, &MicrogInstall::copyLocal);
}

void MicrogInstall::copyLocal()
{
    if (!m_busy) return;
    const QByteArray bytes = m_input.read(65536);
    if (bytes.isEmpty()) {
        if (m_input.error() != QFile::NoError)
            finish(false, tr("Cannot read the selected APK: %1").arg(m_input.errorString()));
        else
            publish();
        return;
    }
    if (!append(bytes)) {
        finish(false, tr("Cannot save the selected APK or its size exceeds the limit."));
        return;
    }
    QTimer::singleShot(0, this, &MicrogInstall::copyLocal);
}

bool MicrogInstall::append(const QByteArray &bytes)
{
    const qint64 limit = m_localPath.isEmpty() ? m_release.size : MaxApk;
    if (!m_output || bytes.size() > limit - m_received) return false;
    if (m_output->write(bytes) != bytes.size()) return false;
    m_hash.addData(bytes);
    m_received += bytes.size();
    return true;
}

void MicrogInstall::publish()
{
    if (m_localPath.isEmpty()) emit validatingDownload();
    const QByteArray digest = m_hash.result();
    if (m_received == 0 || (m_localPath.isEmpty() &&
        (m_received != m_release.size || digest != m_release.sha256))) {
        finish(false, tr("The APK does not match the latest official microG release."));
        return;
    }
    if (!m_output->flush()) {
        finish(false, tr("Cannot save the microG APK."));
        return;
    }
    const QByteArray staged = QByteArray("/proc/self/fd/") + QByteArray::number(m_output->handle());
    mg_apk_info info = {};
    if (!mg_apk_read_info(staged.constData(), &info) ||
        QByteArray(info.package) != "com.google.android.gms" ||
        info.split[0] || info.config_for_split[0] || info.feature_split || info.split_required ||
        info.required_split_types[0] || !mg_apk_is_microg(staged.constData())) {
        finish(false, tr("The release APK is not a supported standalone microG package."));
        return;
    }
    if (!m_canPublish()) {
        finish(false, tr("Close all Android apps before updating microG."));
        return;
    }
    if (QFileInfo::exists(m_destination)) {
        QFile old(m_destination);
        const QByteArray expected = QSettings().value(QStringLiteral("googleServices/managedSha256")).toByteArray();
        if (QFileInfo(m_destination).isSymLink() || expected.size() != 64 || !old.open(QIODevice::ReadOnly)) {
            finish(false, tr("The installed microG APK has no trusted update record; it was not replaced."));
            return;
        }
        QCryptographicHash previous(QCryptographicHash::Sha256);
        if (!previous.addData(&old) || previous.result().toHex() != expected) {
            finish(false, tr("The installed microG APK differs from its trusted update record; it was not replaced."));
            return;
        }
        mg_apk_info oldInfo = {};
        const QByteArray oldPath = m_destination.toUtf8();
        if (!mg_apk_read_info(oldPath.constData(), &oldInfo) || oldInfo.version_code > info.version_code) {
            finish(false, tr("The installed microG APK cannot be safely updated or the release is older."));
            return;
        }
    }
    if (!m_output->commit()) {
        finish(false, tr("Cannot install the microG APK: %1").arg(m_output->errorString()));
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("googleServices/managedSha256"), digest.toHex());
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        finish(false, tr("microG was installed, but its update record could not be saved."));
        return;
    }
    finish(true, tr("microG installed. Compatibility is not guaranteed."));
}

void MicrogInstall::finish(bool success, const QString &message)
{
    if (!m_busy) return;
    m_busy = false;
    m_timeout.stop();
    if (m_reply) {
        disconnect(m_reply, nullptr, this, nullptr);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_input.close();
    m_output.reset();
    m_lock.reset();
    m_canPublish = {};
    emit finished(success, message);
}
