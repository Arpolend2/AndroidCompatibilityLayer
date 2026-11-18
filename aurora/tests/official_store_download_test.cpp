#include "../official_store_download.h"

#include <QCoreApplication>
#include <QFile>
#include <QList>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstring>

struct Response {
    int status;
    QByteArray body;
    QUrl redirect;
    qint64 length;
};

class FakeReply : public QNetworkReply
{
public:
    FakeReply(const QNetworkRequest &request, const Response &response, QObject *parent)
        : QNetworkReply(parent), m_body(response.body), m_position(0)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(QNetworkAccessManager::GetOperation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        if (!response.redirect.isEmpty())
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, response.redirect);
        if (response.length >= 0) setHeader(QNetworkRequest::ContentLengthHeader, response.length);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this]() {
            if (!m_body.isEmpty()) emit readyRead();
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 bytesAvailable() const override
    {
        return m_body.size() - m_position + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        const qint64 size = qMin(maxSize, qint64(m_body.size()) - m_position);
        if (size <= 0) return -1;
        std::memcpy(data, m_body.constData() + m_position, size_t(size));
        m_position += size;
        return size;
    }

private:
    QByteArray m_body;
    qint64 m_position;
};

class FakeNetwork : public QNetworkAccessManager
{
public:
    QList<Response> responses;
    int requests = 0;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *) override
    {
        if (op != GetOperation || requests >= responses.size()) return nullptr;
        return new FakeReply(request, responses.at(requests++), this);
    }
};

static bool runCase(const QList<Response> &responses, bool expectSuccess, int expectedRequests,
                    int expectedProgress = -2)
{
    QTemporaryDir directory;
    if (!directory.isValid()) return false;
    FakeNetwork network;
    network.responses = responses;
    OfficialStoreDownload download(nullptr, &network);
    QEventLoop loop;
    bool succeeded = false, failed = false;
    QList<int> progress;
    QString savedPath, error;
    QObject::connect(&download, &OfficialStoreDownload::downloadProgress, &loop,
                     [&](int percent) { progress.append(percent); });
    QObject::connect(&download, &OfficialStoreDownload::ready, &loop, [&](const QString &path) {
        savedPath = path;
        QFile file(path);
        succeeded = file.open(QIODevice::ReadOnly) && file.readAll() == QByteArray("APK fixture");
        loop.quit();
    });
    QObject::connect(&download, &OfficialStoreDownload::failed, &loop, [&](const QString &message) {
        failed = !message.isEmpty();
        loop.quit();
    });
    if (download.start(QString(), &error) || error.isEmpty()) return false;
    if (!download.start(directory.path(), &error) || download.start(directory.path(), &error)) return false;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    download.clear();
    return succeeded == expectSuccess && failed != expectSuccess &&
        network.requests == expectedRequests && (savedPath.isEmpty() || !QFile::exists(savedPath)) &&
        (expectedProgress == -2 || (!progress.isEmpty() && progress.first() == -1 &&
                                    progress.last() == expectedProgress));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray metadata = "{\"tag_name\":\"4.8.4\",\"upcoming_release\":false,"
        "\"description\":\"[AuroraStore-4.8.4.apk](/uploads/245eec8867a47057cbc54535f73b32ee/AuroraStore-4.8.4.apk)\"}";
    const Response good = {200, metadata, QUrl(), metadata.size()};
    const Response apk = {200, "APK fixture", QUrl(), 11};
    const Response safe = {302, QByteArray(), QUrl(QStringLiteral("/api/v4/projects/6922885/releases/4.8.4")), 0};
    const Response unsafe = {302, QByteArray(), QUrl(QStringLiteral("https://example.com/evil")), 0};
    const Response shortApk = {200, "APK fixture", QUrl(), 12};
    const Response unknownLength = {200, "APK fixture", QUrl(), -1};
    const Response oversized = {200, QByteArray(256 * 1024 + 1, 'a'), QUrl(), -1};
    if (!runCase({safe, good, apk}, true, 3, 100) ||
        !runCase({good, unknownLength}, true, 2, -1) ||
        !runCase({unsafe}, false, 1) ||
        !runCase({good, shortApk}, false, 2) ||
        !runCase({oversized}, false, 1)) return 1;
    std::puts("Official store download OK");
    return 0;
}
