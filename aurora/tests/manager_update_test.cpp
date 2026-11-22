#include "../manager_update.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QTimer>
#include <cstring>

struct Response {
    int status;
    QByteArray data;
    QUrl redirect;
};

class Reply : public QNetworkReply {
public:
    Reply(const QNetworkRequest &request, const Response &response, QObject *parent)
        : QNetworkReply(parent), m_data(response.data)
    {
        setRequest(request);
        setUrl(request.url());
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response.status);
        if (!response.redirect.isEmpty())
            setAttribute(QNetworkRequest::RedirectionTargetAttribute, response.redirect);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this]() { emit readyRead(); setFinished(true); emit finished(); });
    }
    void abort() override {}
    qint64 bytesAvailable() const override { return m_data.size() - m_pos + QNetworkReply::bytesAvailable(); }
protected:
    qint64 readData(char *data, qint64 size) override
    {
        const qint64 count = qMin(size, qint64(m_data.size()) - m_pos);
        if (count <= 0) return -1;
        std::memcpy(data, m_data.constData() + m_pos, size_t(count));
        m_pos += count;
        return count;
    }
private:
    QByteArray m_data;
    qint64 m_pos = 0;
};

class Network : public QNetworkAccessManager {
public:
    QList<Response> responses;
    int requests = 0;
protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        if (requests >= responses.size()) qFatal("Unexpected network request");
        return new Reply(request, responses.at(requests++), this);
    }
};

static QByteArray release(const QByteArray &version, const QByteArray &arch, const QByteArray &payload)
{
    const QByteArray name = "ru.androidcompatibilitylayer.appmanager-" + version + "-1." + arch + ".rpm";
    return "{\"draft\":false,\"prerelease\":true,\"tag_name\":\"" + version + "\",\"assets\":[{"
        "\"name\":\"" + name + "\",\"size\":" + QByteArray::number(payload.size()) +
        ",\"digest\":\"sha256:" + QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex() +
        "\",\"browser_download_url\":\"https://github.com/Arpolend2/AndroidCompatibilityLayer/releases/download/" +
        version + "/" + name + "\"}]}";
}

static bool download(const QList<Response> &responses, bool good, bool current, int requests)
{
    QTemporaryDir directory;
    Network network;
    network.responses = responses;
    ManagerUpdate update(nullptr, &network);
    QEventLoop loop;
    bool ready = false, failed = false, isCurrent = false;
    QString path;
    QList<int> progress;
    QObject::connect(&update, &ManagerUpdate::progress, &loop, [&](int value) { progress.append(value); });
    QObject::connect(&update, &ManagerUpdate::ready, &loop, [&](const QString &file, const QString &version) {
        QFile rpm(file);
        ready = version == QLatin1String("0.3.2-1") && rpm.open(QIODevice::ReadOnly) &&
            rpm.readAll() == QByteArray("RPM fixture");
        path = file;
        loop.quit();
    });
    QObject::connect(&update, &ManagerUpdate::failed, &loop, [&](const QString &message) {
        failed = !message.isEmpty();
        loop.quit();
    });
    QObject::connect(&update, &ManagerUpdate::current, &loop, [&]() { isCurrent = true; loop.quit(); });
    QString error;
    if (!update.start(directory.path(), QStringLiteral("0.3.1-1"), QStringLiteral("aarch64"), &error) ||
        update.start(directory.path(), QStringLiteral("0.3.1-1"), QStringLiteral("aarch64"), &error)) return false;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    if (update.busy() || network.requests != requests || ready != good || isCurrent != current ||
        failed != (!good && !current)) return false;
    if (good && (progress.isEmpty() || progress.first() != 0 || progress.last() != 100 || !QFile::exists(path)))
        return false;
    return good || QDir(directory.path()).entryList(QDir::Files).isEmpty();
}

static bool checkOnly(const QByteArray &metadata)
{
    Network network;
    network.responses = {{200, metadata, {}}};
    ManagerUpdate update(nullptr, &network);
    QEventLoop loop;
    bool available = false;
    QObject::connect(&update, &ManagerUpdate::available, &loop, [&](const QString &version) {
        available = version == QLatin1String("0.3.2-1");
        loop.quit();
    });
    QObject::connect(&update, &ManagerUpdate::failed, &loop, [&](const QString &) { loop.quit(); });
    QString error;
    if (!update.start(QString(), QStringLiteral("0.3.1-1"), QStringLiteral("aarch64"), &error, true)) return false;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    return available && !update.busy() && network.requests == 1;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QByteArray rpm("RPM fixture");
    const QByteArray newer = "[" + release("0.3.2", "aarch64", rpm) + "]";
    const QByteArray older = "[" + release("0.3.0", "aarch64", rpm) + "]";
    ManagerUpdateRelease result;
    QString error;
    if (managerUpdateRelease(newer, QStringLiteral("0.3.1-1"), QStringLiteral("aarch64"), &result, &error) != ManagerUpdateResult::Update ||
        managerUpdateRelease(newer, QStringLiteral("0.3.2-1"), QStringLiteral("aarch64"), &result, &error) != ManagerUpdateResult::Current ||
        managerUpdateRelease(older, QStringLiteral("0.3.2-1"), QStringLiteral("aarch64"), &result, &error) != ManagerUpdateResult::Current ||
        managerUpdateRelease(newer, QStringLiteral("0.3.1-1"), QStringLiteral("armv7hl"), &result, &error) != ManagerUpdateResult::Error ||
        managerUpdateRelease(newer, QStringLiteral("bad"), QStringLiteral("aarch64"), &result, &error) != ManagerUpdateResult::Error ||
        managerUpdateRelease("[]", QStringLiteral("0.3.1-1"), QStringLiteral("aarch64"), &result, &error) != ManagerUpdateResult::Error ||
        managerUpdateUrlAllowed(QUrl(QStringLiteral("https://example.com/file")))) return 1;
    QByteArray badDigest = newer;
    badDigest.replace("sha256:", "missing:");
    if (managerUpdateRelease(badDigest, QStringLiteral("0.3.1-1"), QStringLiteral("aarch64"), &result, &error) != ManagerUpdateResult::Error ||
        !checkOnly(newer) ||
        !download({{200, newer, {}}, {200, rpm, {}}}, true, false, 2) ||
        !download({{200, older, {}}}, false, true, 1) ||
        !download({{200, newer, {}}, {200, "bad fixture", {}}}, false, false, 2) ||
        !download({{200, newer, {}}, {302, {}, QUrl(QStringLiteral("https://example.com/evil"))}}, false, false, 2))
        return 1;
    return 0;
}
