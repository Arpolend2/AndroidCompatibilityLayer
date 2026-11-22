#include "manager_update.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace {
const qint64 MaxMetadata = 1024 * 1024;
const qint64 MaxRpm = 512LL * 1024 * 1024;

bool parseVersion(const QString &text, QVector<int> *parts, bool requireRelease)
{
    static const QRegularExpression pattern(QStringLiteral("^v?([0-9]{1,6})\\.([0-9]{1,6})\\.([0-9]{1,6})(?:-([0-9]{1,6}))?$"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch() || (requireRelease && match.captured(4).isEmpty())) return false;
    parts->clear();
    for (int i = 1; i <= 3; ++i) parts->append(match.captured(i).toInt());
    parts->append(match.captured(4).isEmpty() ? 0 : match.captured(4).toInt());
    return true;
}

ManagerUpdateResult errorResult(QString *error, const QString &message)
{
    if (error) *error = message;
    return ManagerUpdateResult::Error;
}
}

bool managerUpdateUrlAllowed(const QUrl &url)
{
    const QString host = url.host().toLower();
    return url.isValid() && url.scheme() == QLatin1String("https") &&
        url.userInfo().isEmpty() && (url.port() == -1 || url.port() == 443) &&
        !url.hasFragment() &&
        (host == QLatin1String("github.com") || host == QLatin1String("api.github.com") ||
         host == QLatin1String("release-assets.githubusercontent.com") ||
         host == QLatin1String("objects.githubusercontent.com") ||
         host == QLatin1String("github-releases.githubusercontent.com"));
}

ManagerUpdateResult managerUpdateRelease(const QByteArray &json, const QString &installedVersion,
                                        const QString &architecture, ManagerUpdateRelease *release,
                                        QString *error)
{
    if (error) error->clear();
    QVector<int> installed;
    if (!release || !parseVersion(installedVersion, &installed, true) ||
        (architecture != QLatin1String("aarch64") && architecture != QLatin1String("armv7hl")))
        return errorResult(error, QObject::tr("The installed version or device architecture is unsupported."));
    QJsonParseError parse;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    if (json.size() > MaxMetadata || parse.error != QJsonParseError::NoError || !doc.isArray())
        return errorResult(error, QObject::tr("Cannot read the GitHub release list."));
    const QJsonArray releases = doc.array();
    if (releases.size() >= 100)
        return errorResult(error, QObject::tr("The GitHub release list is too large."));
    QJsonObject latest;
    QVector<int> latestVersion;
    for (const QJsonValue &value : releases) {
        if (!value.isObject()) return errorResult(error, QObject::tr("Invalid GitHub release metadata."));
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("draft")).toBool()) continue;
        const QString tag = object.value(QStringLiteral("tag_name")).toString();
        QVector<int> version;
        if (!parseVersion(tag, &version, false) || version[3] != 0) continue;
        if (latest.isEmpty() || version > latestVersion) {
            latest = object;
            latestVersion = version;
        } else if (version == latestVersion) {
            return errorResult(error, QObject::tr("GitHub contains ambiguous release versions."));
        }
    }
    if (latest.isEmpty()) return errorResult(error, QObject::tr("No published GitHub release was found."));
    if (latestVersion[0] < installed[0] ||
        (latestVersion[0] == installed[0] && latestVersion[1] < installed[1]) ||
        (latestVersion[0] == installed[0] && latestVersion[1] == installed[1] && latestVersion[2] < installed[2]))
        return ManagerUpdateResult::Current;
    if (!latest.value(QStringLiteral("assets")).isArray())
        return errorResult(error, QObject::tr("The latest release has no RPM for this device."));
    const QString tag = latest.value(QStringLiteral("tag_name")).toString();
    const QString version = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
    const QRegularExpression namePattern(QStringLiteral("^ru\\.androidcompatibilitylayer\\.appmanager-") +
        QRegularExpression::escape(version) + QStringLiteral("-([0-9]{1,6})\\.") +
        QRegularExpression::escape(architecture) + QStringLiteral("\\.rpm$"));
    static const QRegularExpression digestPattern(QStringLiteral("^sha256:([a-f0-9]{64})$"));
    ManagerUpdateRelease candidate;
    QVector<int> candidateVersion;
    int found = 0;
    for (const QJsonValue &value : latest.value(QStringLiteral("assets")).toArray()) {
        if (!value.isObject()) return errorResult(error, QObject::tr("Invalid GitHub release asset."));
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QRegularExpressionMatch nameMatch = namePattern.match(name);
        if (!nameMatch.hasMatch()) continue;
        ++found;
        const QRegularExpressionMatch digest = digestPattern.match(asset.value(QStringLiteral("digest")).toString());
        const double size = asset.value(QStringLiteral("size")).toDouble(-1);
        const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString(), QUrl::StrictMode);
        const QString expected = QStringLiteral("/Arpolend2/AndroidCompatibilityLayer/releases/download/") + tag + QLatin1Char('/') + name;
        if (!digest.hasMatch() || size < 1 || size > MaxRpm || size != static_cast<qint64>(size) ||
            !managerUpdateUrlAllowed(url) || url.host() != QLatin1String("github.com") ||
            url.path() != expected || url.hasQuery())
            return errorResult(error, QObject::tr("The release RPM has invalid metadata or checksum."));
        candidate.url = url;
        candidate.sha256 = QByteArray::fromHex(digest.captured(1).toLatin1());
        candidate.size = static_cast<qint64>(size);
        candidate.version = version + QLatin1Char('-') + nameMatch.captured(1);
        if (!parseVersion(candidate.version, &candidateVersion, true))
            return errorResult(error, QObject::tr("The release RPM version is invalid."));
    }
    if (found != 1)
        return errorResult(error, QObject::tr("The latest release does not contain exactly one RPM for this device."));
    if (candidateVersion <= installed) return ManagerUpdateResult::Current;
    *release = candidate;
    return ManagerUpdateResult::Update;
}

ManagerUpdate::ManagerUpdate(QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_networkManager(network ? network : &m_network), m_hash(QCryptographicHash::Sha256)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this]() { fail(tr("The update download timed out.")); });
}

bool ManagerUpdate::start(const QString &directory, const QString &version, const QString &architecture, QString *error,
                          bool checkOnly)
{
    if (error) error->clear();
    if (m_busy || (!checkOnly && (directory.isEmpty() || !QDir().mkpath(directory)))) {
        if (error) *error = tr("The update check is unavailable or already running.");
        return false;
    }
    m_file.reset();
    m_metadata.clear();
    m_directory = directory;
    m_version = version;
    m_architecture = architecture;
    m_downloading = false;
    m_checkOnly = checkOnly;
    m_busy = true;
    m_redirects = 0;
    request(QUrl(QStringLiteral("https://api.github.com/repos/Arpolend2/AndroidCompatibilityLayer/releases?per_page=100")));
    return true;
}

void ManagerUpdate::request(const QUrl &url)
{
    if (!managerUpdateUrlAllowed(url) || ++m_redirects > 6 ||
        (!m_downloading && (url.host() != QLatin1String("api.github.com") ||
                            url.path() != QLatin1String("/repos/Arpolend2/AndroidCompatibilityLayer/releases")))) {
        fail(tr("The update server redirected to an untrusted address."));
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("Accept", m_downloading ? "application/octet-stream" : "application/vnd.github+json");
    request.setRawHeader("User-Agent", "AndroidCompatibilityLayer-Aurora");
    m_reply = m_networkManager->get(request);
    m_reply->setReadBufferSize(65536);
    connect(m_reply, &QNetworkReply::readyRead, this, &ManagerUpdate::readReply);
    connect(m_reply, &QNetworkReply::finished, this, &ManagerUpdate::replyFinished);
    m_timeout.start(60000);
}

void ManagerUpdate::readReply()
{
    if (!m_reply || !m_busy) return;
    m_timeout.start(60000);
    const int status = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        m_reply->readAll();
        return;
    }
    while (m_reply && m_reply->bytesAvailable() > 0) {
        const QByteArray bytes = m_reply->read(65536);
        if (bytes.isEmpty()) break;
        if (!m_downloading) {
            if (bytes.size() > MaxMetadata - m_metadata.size()) {
                fail(tr("The GitHub release list is too large."));
                return;
            }
            m_metadata += bytes;
        } else {
            if (bytes.size() > m_release.size - m_received || !m_file ||
                m_file->write(bytes) != bytes.size()) {
                fail(tr("Cannot save the update RPM or its size exceeds the release metadata."));
                return;
            }
            m_hash.addData(bytes);
            m_received += bytes.size();
            const int percent = int((m_received * 100) / m_release.size);
            if (percent != m_lastProgress) {
                m_lastProgress = percent;
                emit progress(percent);
            }
        }
    }
}

void ManagerUpdate::stopReply()
{
    m_timeout.stop();
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void ManagerUpdate::replyFinished()
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
        fail(tr("Cannot download the update: %1 (HTTP %2).").arg(detail).arg(status));
        return;
    }
    if (!m_downloading) {
        QString error;
        const ManagerUpdateResult result = managerUpdateRelease(m_metadata, m_version, m_architecture, &m_release, &error);
        if (result == ManagerUpdateResult::Error) { fail(error); return; }
        if (result == ManagerUpdateResult::Current) {
            m_busy = false;
            emit current();
            return;
        }
        if (m_checkOnly) {
            m_busy = false;
            emit available(m_release.version);
            return;
        }
        m_file.reset(new QTemporaryFile(QDir(m_directory).filePath(QStringLiteral("manager-update-XXXXXX.rpm"))));
        if (!m_file->open()) { fail(tr("Cannot create the temporary update RPM.")); return; }
        m_downloading = true;
        m_received = 0;
        m_hash.reset();
        m_lastProgress = -1;
        m_redirects = 0;
        emit progress(0);
        request(m_release.url);
        return;
    }
    if (!m_file || m_received != m_release.size || m_hash.result() != m_release.sha256 || !m_file->flush()) {
        fail(tr("The update RPM does not match the GitHub release checksum or size."));
        return;
    }
    m_file->setAutoRemove(false);
    m_file->close();
    m_busy = false;
    emit ready(m_file->fileName(), m_release.version);
}

void ManagerUpdate::fail(const QString &message)
{
    stopReply();
    m_file.reset();
    m_busy = false;
    emit failed(message);
}
