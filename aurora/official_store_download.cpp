#include "official_store_download.h"
#include "official_store_release.h"

#include <QDir>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace {
const qint64 MaxMetadataBytes = 256 * 1024;
const qint64 MaxApkBytes = 512ll * 1024 * 1024;
const int TimeoutMs = 120000;

bool validMetadataUrl(const QUrl &url)
{
    static const QRegularExpression path(
        QStringLiteral("^/api/v4/projects/6922885/releases/(?:permalink/latest|[0-9]+(?:\\.[0-9]+){1,3})$"));
    return url.isValid() && url.scheme() == QLatin1String("https") &&
        url.host() == QLatin1String("gitlab.com") && (url.port() == -1 || url.port() == 443) &&
        url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() &&
        path.match(url.path()).hasMatch();
}
}

OfficialStoreDownload::OfficialStoreDownload(QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), m_networkManager(network ? network : &m_network), m_reply(nullptr),
      m_received(0), m_redirects(0), m_phase(Metadata)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this]() { fail(tr("Aurora Store download timed out.")); });
}

bool OfficialStoreDownload::start(const QString &cacheDirectory, QString *error)
{
    if (error) error->clear();
    if (busy() || cacheDirectory.isEmpty() || !QDir().mkpath(cacheDirectory)) {
        if (error) *error = tr("Aurora Store download is unavailable or busy.");
        return false;
    }
    clear();
    m_cacheDirectory = cacheDirectory;
    m_phase = Metadata;
    m_redirects = 0;
    request(QUrl(QStringLiteral("https://gitlab.com/api/v4/projects/6922885/releases/permalink/latest")));
    return true;
}

void OfficialStoreDownload::clear()
{
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
    m_timeout.stop();
    m_file.reset();
    m_metadata.clear();
    m_received = 0;
}

void OfficialStoreDownload::request(const QUrl &url)
{
    m_received = 0;
    QNetworkRequest request(url);
    request.setRawHeader("Accept", m_phase == Metadata ? "application/json" : "application/octet-stream");
    m_reply = m_networkManager->get(request);
    m_reply->setReadBufferSize(65536);
    connect(m_reply, &QNetworkReply::readyRead, this, &OfficialStoreDownload::readAvailable);
    connect(m_reply, &QNetworkReply::finished, this, &OfficialStoreDownload::finished);
    m_timeout.start(TimeoutMs);
}

void OfficialStoreDownload::readAvailable()
{
    if (!m_reply) return;
    const qint64 limit = m_phase == Metadata ? MaxMetadataBytes : MaxApkBytes;
    while (m_reply && m_reply->bytesAvailable() > 0) {
        const QByteArray bytes = m_reply->read(65536);
        if (bytes.isEmpty()) break;
        if (bytes.size() > limit - m_received) {
            fail(tr("Aurora Store response exceeds the size limit."));
            return;
        }
        m_received += bytes.size();
        if (m_phase == Metadata) m_metadata.append(bytes);
        else if (!m_file || m_file->write(bytes) != bytes.size()) {
            fail(tr("Could not save the Aurora Store APK."));
            return;
        }
        if (m_phase == Apk) {
            bool known = false;
            const qint64 total = m_reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&known);
            const int percent = known && total > 0
                    ? int(qMin(qint64(100), (m_received * 100) / total)) : -1;
            if (percent != m_lastProgress) {
                m_lastProgress = percent;
                emit downloadProgress(percent);
            }
        }
        m_timeout.start(TimeoutMs);
    }
}

void OfficialStoreDownload::finished()
{
    if (!m_reply) return;
    readAvailable();
    if (!m_reply) return;

    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    m_timeout.stop();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QUrl original = reply->url();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString networkMessage = reply->errorString();
    bool hasLength = false;
    const qint64 contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&hasLength);
    reply->deleteLater();

    if (!redirect.isEmpty()) {
        const QUrl next = original.resolved(redirect);
        if (m_phase != Metadata || ++m_redirects > 3 || !validMetadataUrl(next)) {
            fail(tr("The Aurora Store server returned an unsafe redirect."));
            return;
        }
        m_metadata.clear();
        request(next);
        return;
    }
    if (networkError != QNetworkReply::NoError || status != 200) {
        fail(networkError != QNetworkReply::NoError
             ? tr("Could not download Aurora Store: %1").arg(networkMessage)
             : tr("Could not download Aurora Store (HTTP %1).").arg(status));
        return;
    }
    if (hasLength && (contentLength < 0 || contentLength != m_received)) {
        fail(tr("The Aurora Store response has an incorrect length."));
        return;
    }
    if (m_phase == Metadata) {
        OfficialStoreRelease release;
        QString error;
        if (!parseOfficialStoreRelease(m_metadata, &release, &error)) {
            fail(error);
            return;
        }
        m_file.reset(new QTemporaryFile(QDir(m_cacheDirectory).filePath(QStringLiteral("aurora-store-XXXXXX.apk"))));
        if (!m_file->open()) {
            fail(tr("Could not create a temporary Aurora Store APK."));
            return;
        }
        m_apkUrl = release.apkUrl;
        m_phase = Apk;
        m_lastProgress = -1;
        emit downloadProgress(-1);
        request(m_apkUrl);
        return;
    }
    if (!m_file || !m_received || !m_file->flush()) {
        fail(tr("Could not save the Aurora Store APK."));
        return;
    }
    m_file->close();
    emit ready(m_file->fileName());
}

void OfficialStoreDownload::fail(const QString &message)
{
    clear();
    emit failed(message);
}
