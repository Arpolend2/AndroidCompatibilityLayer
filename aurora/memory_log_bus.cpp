#include "memory_log_bus.h"
#include "manager_identity.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>
#include <QVariant>

#include <atomic>
#include <cstdio>
#include <deque>
#include <limits>

namespace {
const char Service[] = "ru.androidcompatibilitylayer.appmanager";
const char Path[] = "/ru/androidcompatibilitylayer/appmanager/log";
const char Interface[] = "ru.androidcompatibilitylayer.appmanager.log";
const int MaxMessage = 4096;
const int MaxSource = 128;
const int MaxPending = 16;
const int MaxClients = 64;
const int MaxIdentityExecutable = 1024;
const int MaxIdentityKey = 256;
const int MaxQueueBytes = 256 * 1024;
const int MaxQueueEvents = 1024;
const int MaxEventsPerSecond = 100;
const int CallTimeout = 5000;
std::atomic<bool> clientDetailedState(false);
const char *const DiagnosticStages[] = {
    "probe", "manager discovery", "manager activation", "manager identification",
    "state request", "event delivery", "connection", "manager well-known comparison",
    "identity detail delivery"
};
const char *const DiagnosticCategories[] = {
    "started", "access denied", "service unavailable", "timeout",
    "D-Bus error", "invalid response", "identity rejected: executable mismatch, key present",
    "identity rejected: executable mismatch, key empty",
    "identity rejected: executable mismatch, key oversized",
    "identity rejected: key empty", "identity rejected: key oversized", "state received",
    "expected=0; executable_equal=0; key_equal=0", "expected=0; executable_equal=0; key_equal=1",
    "expected=0; executable_equal=1; key_equal=0", "expected=0; executable_equal=1; key_equal=1",
    "expected=1; executable_equal=0; key_equal=0", "expected=1; executable_equal=0; key_equal=1",
    "expected=1; executable_equal=1; key_equal=0", "expected=1; executable_equal=1; key_equal=1"
};
const int StageCount = int(sizeof(DiagnosticStages) / sizeof(DiagnosticStages[0]));
const int CategoryCount = int(sizeof(DiagnosticCategories) / sizeof(DiagnosticCategories[0]));

int diagnosticIndex(const QString &value, const char *const *values, int count)
{
    for (int i = 0; i < count; ++i)
        if (value == QLatin1String(values[i])) return i;
    return -1;
}

struct Event {
    QString source;
    QString message;
    bool error;
    int bytes() const { return (source.size() + message.size()) * int(sizeof(QChar)); }
};

class Server : public QDBusVirtualObject
{
public:
    Server(const QDBusConnection &bus, const MemoryLogBus::Receiver &receiver,
           const MemoryLogBus::DetailedState &state)
        : QDBusVirtualObject(QCoreApplication::instance()), m_bus(bus), m_receiver(receiver),
          m_state(state), m_watcher(this)
    {
        m_watcher.setConnection(bus);
        m_watcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
        connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this,
                [this](const QString &name) { m_clients.remove(name); m_watcher.removeWatchedService(name); });
        m_clock.start();
        m_lossTimer.setInterval(1000);
        connect(&m_lossTimer, &QTimer::timeout, this, [this]() {
            for (auto it = m_rates.begin(); it != m_rates.end(); ++it) {
                if (!it.value().dropped) continue;
                m_receiver(it.key(), QStringLiteral("IPC"), QString(), false, it.value().dropped);
                it.value().dropped = 0;
            }
        });
        m_lossTimer.start();
    }

    QString introspect(const QString &path) const override
    {
        if (path != QLatin1String(Path)) return QString();
        return QStringLiteral("<interface name=\"ru.androidcompatibilitylayer.appmanager.log\">"
                              "<method name=\"Diagnostic\"><arg type=\"s\" direction=\"in\"/>"
                              "<arg type=\"s\" direction=\"in\"/></method>"
                              "<method name=\"DiagnosticIdentity\"><arg type=\"s\" direction=\"in\"/>"
                              "<arg type=\"s\" direction=\"in\"/></method>"
                              "<method name=\"State\"><arg type=\"b\" direction=\"out\"/></method>"
                              "<method name=\"Append\"><arg type=\"s\" direction=\"in\"/>"
                              "<arg type=\"s\" direction=\"in\"/><arg type=\"b\" direction=\"in\"/>"
                              "<arg type=\"t\" direction=\"in\"/></method></interface>");
    }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
    {
        if (message.type() != QDBusMessage::MethodCallMessage || message.path() != QLatin1String(Path) ||
            message.interface() != QLatin1String(Interface)) return false;
        if (message.member() == QLatin1String("Diagnostic")) {
            const QList<QVariant> args = message.arguments();
            const int stage = args.size() == 2 && args.at(0).type() == QVariant::String
                ? diagnosticIndex(args.at(0).toString(), DiagnosticStages, StageCount) : -1;
            const int category = args.size() == 2 && args.at(1).type() == QVariant::String
                ? diagnosticIndex(args.at(1).toString(), DiagnosticCategories, CategoryCount) : -1;
            if (stage < 0 || category < 0) {
                ++m_invalid;
                connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid diagnostic.")));
            } else {
                ++m_diagnostics[stage][category];
                connection.send(message.createReply());
            }
            return true;
        }
        if (message.member() == QLatin1String("DiagnosticIdentity")) {
            const QList<QVariant> args = message.arguments();
            if (args.size() != 2 || args.at(0).type() != QVariant::String ||
                args.at(1).type() != QVariant::String ||
                args.at(0).toString().size() > MaxIdentityExecutable ||
                args.at(1).toString().size() > MaxIdentityKey ||
                !message.service().startsWith(QLatin1Char(':'))) {
                ++m_invalid;
                connection.send(message.createErrorReply(QDBusError::InvalidArgs,
                                                          QStringLiteral("Invalid identity diagnostic.")));
            } else {
                if (m_state()) {
                    QJsonObject detail;
                    detail.insert(QStringLiteral("sender"), message.service());
                    detail.insert(QStringLiteral("executable"), args.at(0).toString());
                    detail.insert(QStringLiteral("keyid"), args.at(1).toString());
                    m_receiver(QStringLiteral("untrusted-dbus-identity"),
                               QStringLiteral("D-Bus identity"),
                               QString::fromUtf8(QJsonDocument(detail).toJson(QJsonDocument::Compact)),
                               false, 0);
                }
                connection.send(message.createReply());
            }
            return true;
        }
        if (message.member() == QLatin1String("State")) {
            ++m_states;
            if (!message.arguments().isEmpty()) {
                connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid state request.")));
            } else {
                connection.send(message.createReply(QVariantList() << m_state()));
            }
            return true;
        }
        if (message.member() != QLatin1String("Append")) return false;
        ++m_appends;
        const QList<QVariant> args = message.arguments();
        if (args.size() != 4 || args.at(0).type() != QVariant::String ||
            args.at(1).type() != QVariant::String || args.at(2).type() != QVariant::Bool ||
            args.at(3).type() != QVariant::ULongLong ||
            args.at(0).toString().size() > MaxSource || args.at(1).toString().size() > MaxMessage ||
            !message.service().startsWith(QLatin1Char(':'))) {
            ++m_invalid;
            connection.send(message.createErrorReply(QDBusError::InvalidArgs, QStringLiteral("Invalid log event.")));
            return true;
        }
        const QString sender = message.service();
        if (m_clients.contains(sender)) {
            receive(connection, message, m_clients.value(sender));
            return true;
        }
        if (m_pending >= MaxPending || m_clients.size() >= MaxClients) {
            ++m_limits;
            connection.send(message.createErrorReply(QDBusError::LimitsExceeded, QStringLiteral("Log identity limit reached.")));
            return true;
        }
        QDBusMessage identify = QDBusMessage::createMethodCall(
            QStringLiteral("ru.omp.am"), QStringLiteral("/ru/omp/am"),
            QStringLiteral("ru.omp.am.Identify"), QStringLiteral("IdentifyByBusName"));
        identify << sender;
        ++m_pending;
        message.setDelayedReply(true);
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(connection.asyncCall(identify, CallTimeout), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, watcher, connection, message, sender]() {
            --m_pending;
            const QDBusMessage reply = watcher->reply();
            const QList<QVariant> values = reply.arguments();
            if (reply.type() != QDBusMessage::ReplyMessage || values.size() != 2 ||
                values.at(0).type() != QVariant::String || values.at(1).type() != QVariant::String ||
                !isTrustedLogSenderExecutable(values.at(0).toString()) ||
                values.at(1).toString().isEmpty() ||
                values.at(1).toString().size() > MaxAppManagerIdentityKeyLength ||
                m_clients.size() >= MaxClients) {
                if (reply.type() != QDBusMessage::ReplyMessage || values.size() != 2 ||
                    values.at(0).type() != QVariant::String || values.at(1).type() != QVariant::String)
                    ++m_identityErrors;
                else
                    ++m_identityRejected;
                connection.send(message.createErrorReply(QDBusError::AccessDenied,
                                                          QStringLiteral("Log sender identification failed.")));
            } else {
                const QString identity = values.at(0).toString();
                m_clients.insert(sender, identity);
                m_watcher.addWatchedService(sender);
                receive(connection, message, identity);
            }
            watcher->deleteLater();
        });
        return true;
    }

    QString statistics() const
    {
        QString result = QStringLiteral("Log transport (manager process only): state requests=%1, append requests=%2, "
                                        "accepted=%3, identification errors=%4, identity rejected=%5, "
                                        "invalid requests=%6, limited=%7.\n")
            .arg(m_states.load()).arg(m_appends.load()).arg(m_accepted.load())
            .arg(m_identityErrors.load()).arg(m_identityRejected.load()).arg(m_invalid.load()).arg(m_limits.load());
        bool seenDiagnostic = false;
        for (int stage = 0; stage < StageCount; ++stage) {
            for (int category = 0; category < CategoryCount; ++category) {
                const quint64 count = m_diagnostics[stage][category].load();
                if (!count) continue;
                if (!seenDiagnostic)
                    result += QStringLiteral("Client diagnostics (untrusted, no log data):\n");
                seenDiagnostic = true;
                result += QStringLiteral("  %1 / %2 = %3\n")
                    .arg(QLatin1String(DiagnosticStages[stage]), QLatin1String(DiagnosticCategories[category]))
                    .arg(count);
            }
        }
        if (!seenDiagnostic && !m_states.load() && !m_appends.load())
            result += QStringLiteral("No requests: standalone may not have started, or connection failed before reaching the manager.\n");
        return result;
    }

private:
    void receive(const QDBusConnection &connection, const QDBusMessage &message, const QString &identity)
    {
        const QList<QVariant> args = message.arguments();
        if (!m_rates.contains(identity) && m_rates.size() >= MaxClients) {
            ++m_limits;
            const quint64 reported = args.at(3).toULongLong();
            m_receiver(identity, QStringLiteral("IPC"), QString(), false,
                       reported == std::numeric_limits<quint64>::max() ? reported : reported + 1);
            connection.send(message.createErrorReply(QStringLiteral("ru.androidcompatibilitylayer.appmanager.log.Error.Counted"),
                                                      QStringLiteral("Log source limit reached.")));
            return;
        }
        Rate &rate = m_rates[identity];
        if (m_clock.elapsed() - rate.window >= 1000) {
            rate.window = m_clock.elapsed();
            rate.count = 0;
        }
        if (++rate.count > MaxEventsPerSecond) {
            ++m_limits;
            const quint64 reported = args.at(3).toULongLong();
            const quint64 available = std::numeric_limits<quint64>::max() - rate.dropped;
            rate.dropped += qMin(available, reported == std::numeric_limits<quint64>::max() ? reported : reported + 1);
            connection.send(message.createErrorReply(QStringLiteral("ru.androidcompatibilitylayer.appmanager.log.Error.Counted"),
                                                      QStringLiteral("Log rate limit reached.")));
            return;
        }
        m_receiver(identity, args.at(0).toString(), args.at(1).toString(), args.at(2).toBool(), args.at(3).toULongLong());
        ++m_accepted;
        connection.send(message.createReply());
    }
    struct Rate { qint64 window = 0; int count = 0; quint64 dropped = 0; };
    QDBusConnection m_bus;
    MemoryLogBus::Receiver m_receiver;
    MemoryLogBus::DetailedState m_state;
    QDBusServiceWatcher m_watcher;
    QMap<QString, QString> m_clients;
    QMap<QString, Rate> m_rates;
    QElapsedTimer m_clock;
    QTimer m_lossTimer;
    int m_pending = 0;
    std::atomic<quint64> m_states{0}, m_appends{0}, m_accepted{0}, m_identityErrors{0};
    std::atomic<quint64> m_identityRejected{0}, m_invalid{0}, m_limits{0};
    std::atomic<quint64> m_diagnostics[StageCount][CategoryCount]{};
};

class Client : public QObject
{
public:
    explicit Client(const QDBusConnection &bus)
        : QObject(QCoreApplication::instance()), m_bus(bus)
    {
        m_clock.start();
        m_timer.setInterval(50);
        connect(&m_timer, &QTimer::timeout, this, [this]() { pump(); });
        m_timer.start();
    }
    ~Client() override
    {
        clientDetailedState.store(false, std::memory_order_relaxed);
        QMutexLocker lock(&m_mutex);
        if (!m_queue.empty() || m_dropped || m_beforeState)
            std::fprintf(stderr, "Log IPC shutdown: queued=%llu, dropped=%llu, state-unknown=%llu\n",
                         static_cast<unsigned long long>(m_queue.size()),
                         static_cast<unsigned long long>(m_dropped),
                         static_cast<unsigned long long>(m_beforeState));
    }
    void enqueue(const QString &source, const QString &message, bool error)
    {
        QMutexLocker lock(&m_mutex);
        Event event{source.left(MaxSource), message.left(MaxMessage), error};
        if ((message.size() > MaxMessage || source.size() > MaxSource) &&
            m_dropped != std::numeric_limits<quint64>::max()) ++m_dropped;
        if (m_queue.size() >= MaxQueueEvents || event.bytes() > MaxQueueBytes - m_bytes) {
            if (m_dropped != std::numeric_limits<quint64>::max()) ++m_dropped;
            return;
        }
        m_queue.push_back(event);
        m_bytes += event.bytes();
    }
    void record(const QString &source, const QString &message, const QString &safe, bool error)
    {
        int state;
        {
            QMutexLocker lock(&m_mutex);
            state = m_detailState.load(std::memory_order_relaxed);
            if (state == 0 && m_beforeState != std::numeric_limits<quint64>::max()) ++m_beforeState;
        }
        if (state == 2) enqueue(source, message, error);
        else if (!safe.isEmpty()) enqueue(source, safe, error);
    }

private:
    static QString failureCategory(const QDBusMessage &reply)
    {
        if (reply.type() != QDBusMessage::ErrorMessage) return QStringLiteral("invalid response");
        const QString name = reply.errorName();
        if (name == QLatin1String("org.freedesktop.DBus.Error.AccessDenied") ||
            name == QLatin1String("org.freedesktop.DBus.Error.AuthFailed"))
            return QStringLiteral("access denied");
        if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown") ||
            name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner"))
            return QStringLiteral("service unavailable");
        if (name == QLatin1String("org.freedesktop.DBus.Error.NoReply") ||
            name == QLatin1String("org.freedesktop.DBus.Error.Timeout") ||
            name == QLatin1String("org.freedesktop.DBus.Error.TimedOut"))
            return QStringLiteral("timeout");
        return QStringLiteral("D-Bus error");
    }
    void diagnostic(const QString &stage, const QString &category)
    {
        const QString current = stage + QStringLiteral(": ") + category;
        if (current == m_diagnostic) return;
        m_diagnostic = current;
        m_statusStage = stage;
        m_statusCategory = category;
        sendStatus();
        const QByteArray output = QStringLiteral("Log IPC %1\n").arg(current).toUtf8();
        std::fwrite(output.constData(), 1, size_t(output.size()), stderr);
    }

    void sendStatus()
    {
        if (m_statusBusy || diagnosticIndex(m_statusStage, DiagnosticStages, StageCount) < 0 ||
            diagnosticIndex(m_statusCategory, DiagnosticCategories, CategoryCount) < 0) return;
        m_statusBusy = true;
        m_lastStatus = m_clock.elapsed();
        QDBusMessage request = QDBusMessage::createMethodCall(
            QString::fromLatin1(Service), QString::fromLatin1(Path), QString::fromLatin1(Interface),
            QStringLiteral("Diagnostic"));
        request.setAutoStartService(false);
        request << m_statusStage << m_statusCategory;
        const QString sentStage = m_statusStage;
        const QString sentCategory = m_statusCategory;
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(request, CallTimeout), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, sentStage, sentCategory]() {
            m_statusBusy = false;
            watcher->deleteLater();
            if (sentStage != m_statusStage || sentCategory != m_statusCategory) sendStatus();
        });
    }

    void sendIdentityDetails(const QString &executable, const QString &key)
    {
        if (m_identityDetailsSent) return;
        m_identityDetailsSent = true;
        QDBusMessage request = QDBusMessage::createMethodCall(
            QString::fromLatin1(Service), QString::fromLatin1(Path), QString::fromLatin1(Interface),
            QStringLiteral("DiagnosticIdentity"));
        request.setAutoStartService(false);
        request << executable << key;
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(request, CallTimeout), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
            const QDBusMessage reply = watcher->reply();
            if (reply.type() != QDBusMessage::ReplyMessage || !reply.arguments().isEmpty())
                diagnostic(QStringLiteral("identity detail delivery"), failureCategory(reply));
            watcher->deleteLater();
        });
    }

    void compareWellKnownIdentity(const QString &executable, const QString &key)
    {
        if (m_wellKnownCompared) return;
        m_wellKnownCompared = true;
        QDBusMessage identify = QDBusMessage::createMethodCall(
            QStringLiteral("ru.omp.am"), QStringLiteral("/ru/omp/am"),
            QStringLiteral("ru.omp.am.Identify"), QStringLiteral("IdentifyByBusName"));
        identify << QString::fromLatin1(Service);
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(identify, CallTimeout), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, executable, key]() {
            const QDBusMessage reply = watcher->reply();
            const QList<QVariant> fields = reply.arguments();
            if (reply.type() == QDBusMessage::ErrorMessage) {
                diagnostic(QStringLiteral("manager well-known comparison"), failureCategory(reply));
            } else if (reply.type() != QDBusMessage::ReplyMessage || fields.size() != 2 ||
                       fields.at(0).type() != QVariant::String || fields.at(1).type() != QVariant::String) {
                diagnostic(QStringLiteral("manager well-known comparison"), QStringLiteral("invalid response"));
            } else {
                const QString otherExecutable = fields.at(0).toString();
                const QString otherKey = fields.at(1).toString();
                const QString result = QStringLiteral("expected=%1; executable_equal=%2; key_equal=%3")
                    .arg(isTrustedAppManagerExecutable(otherExecutable) ? 1 : 0)
                    .arg(otherExecutable == executable ? 1 : 0).arg(otherKey == key ? 1 : 0);
                diagnostic(QStringLiteral("manager well-known comparison"), result);
            }
            watcher->deleteLater();
        });
    }

    void identifyManager()
    {
        m_busy = true;
        QDBusMessage owner = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
            QStringLiteral("org.freedesktop.DBus"), QStringLiteral("GetNameOwner"));
        owner << QString::fromLatin1(Service);
        QDBusPendingCallWatcher *ownerWatcher = new QDBusPendingCallWatcher(m_bus.asyncCall(owner, CallTimeout), this);
        connect(ownerWatcher, &QDBusPendingCallWatcher::finished, this, [this, ownerWatcher]() {
            const QDBusMessage ownerReply = ownerWatcher->reply();
            const QList<QVariant> values = ownerReply.arguments();
            ownerWatcher->deleteLater();
            if (ownerReply.type() == QDBusMessage::ErrorMessage &&
                ownerReply.errorName() != QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner") &&
                ownerReply.errorName() != QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")) {
                diagnostic(QStringLiteral("manager discovery"), failureCategory(ownerReply));
                m_busy = false;
                return;
            }
            if (ownerReply.type() != QDBusMessage::ReplyMessage || values.size() != 1 ||
                values.at(0).type() != QVariant::String ||
                !values.at(0).toString().startsWith(QLatin1Char(':'))) {
                if (ownerReply.type() != QDBusMessage::ErrorMessage) {
                    diagnostic(QStringLiteral("manager discovery"), QStringLiteral("invalid response"));
                    m_busy = false;
                    return;
                }
                QDBusMessage activate = QDBusMessage::createMethodCall(
                    QStringLiteral("org.freedesktop.DBus"), QStringLiteral("/org/freedesktop/DBus"),
                    QStringLiteral("org.freedesktop.DBus"), QStringLiteral("StartServiceByName"));
                activate << QString::fromLatin1(Service) << uint(0);
                QDBusPendingCallWatcher *activation = new QDBusPendingCallWatcher(m_bus.asyncCall(activate, CallTimeout), this);
                connect(activation, &QDBusPendingCallWatcher::finished, this, [this, activation]() {
                const QDBusMessage reply = activation->reply();
                if (reply.type() == QDBusMessage::ErrorMessage)
                    diagnostic(QStringLiteral("manager activation"), failureCategory(reply));
                    m_busy = false;
                    activation->deleteLater();
                });
                return;
            }
            const QString unique = values.at(0).toString();
            QDBusMessage identify = QDBusMessage::createMethodCall(
                QStringLiteral("ru.omp.am"), QStringLiteral("/ru/omp/am"),
                QStringLiteral("ru.omp.am.Identify"), QStringLiteral("IdentifyByBusName"));
            identify << unique;
            QDBusPendingCallWatcher *identity = new QDBusPendingCallWatcher(m_bus.asyncCall(identify, CallTimeout), this);
            connect(identity, &QDBusPendingCallWatcher::finished, this, [this, identity, unique]() {
                const QDBusMessage reply = identity->reply();
                const QList<QVariant> fields = reply.arguments();
                if (reply.type() == QDBusMessage::ReplyMessage && fields.size() == 2 &&
                    fields.at(0).type() == QVariant::String &&
                    isTrustedAppManagerExecutable(fields.at(0).toString()) &&
                    fields.at(1).type() == QVariant::String && !fields.at(1).toString().isEmpty() &&
                    fields.at(1).toString().size() <= MaxAppManagerIdentityKeyLength) {
                    m_owner = unique;
                } else if (reply.type() == QDBusMessage::ErrorMessage) {
                    diagnostic(QStringLiteral("manager identification"), failureCategory(reply));
                } else if (reply.type() != QDBusMessage::ReplyMessage || fields.size() != 2 ||
                           fields.at(0).type() != QVariant::String ||
                           fields.at(1).type() != QVariant::String) {
                    diagnostic(QStringLiteral("manager identification"), QStringLiteral("invalid response"));
                } else {
                    const QString key = fields.at(1).toString();
                    QString category;
                    if (!isTrustedAppManagerExecutable(fields.at(0).toString())) {
                        category = key.isEmpty()
                            ? QStringLiteral("identity rejected: executable mismatch, key empty")
                            : key.size() > MaxAppManagerIdentityKeyLength
                                ? QStringLiteral("identity rejected: executable mismatch, key oversized")
                                : QStringLiteral("identity rejected: executable mismatch, key present");
                    } else {
                        category = key.isEmpty() ? QStringLiteral("identity rejected: key empty")
                                                 : QStringLiteral("identity rejected: key oversized");
                    }
                    sendIdentityDetails(fields.at(0).toString(), key);
                    diagnostic(QStringLiteral("manager identification"), category);
                    compareWellKnownIdentity(fields.at(0).toString(), key);
                }
                m_busy = false;
                identity->deleteLater();
            });
        });
    }

    void pump()
    {
        if (m_lastStatus < 0 || m_clock.elapsed() - m_lastStatus >= 5000) sendStatus();
        if (m_busy) return;
        if (m_owner.isEmpty()) {
            if (m_lastIdentity < 0 || m_clock.elapsed() - m_lastIdentity >= 1000) {
                m_lastIdentity = m_clock.elapsed();
                identifyManager();
            }
            return;
        }
        if (m_lastState < 0 || m_clock.elapsed() - m_lastState >= 1000) {
            m_busy = true;
            m_lastState = m_clock.elapsed();
            QDBusMessage request = QDBusMessage::createMethodCall(
                m_owner, QString::fromLatin1(Path), QString::fromLatin1(Interface), QStringLiteral("State"));
            QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(request, CallTimeout), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
                const QDBusMessage reply = watcher->reply();
                const QList<QVariant> args = reply.arguments();
                if (reply.type() != QDBusMessage::ReplyMessage || args.size() != 1 ||
                    args.at(0).type() != QVariant::Bool) {
                    diagnostic(QStringLiteral("state request"), failureCategory(reply));
                    m_owner.clear();
                    m_detailState.store(0, std::memory_order_relaxed);
                    clientDetailedState.store(false, std::memory_order_relaxed);
                } else {
                    const bool enabled = args.at(0).toBool();
                    quint64 beforeState = 0;
                    {
                        QMutexLocker lock(&m_mutex);
                        m_detailState.store(enabled ? 2 : 1, std::memory_order_relaxed);
                        clientDetailedState.store(enabled, std::memory_order_relaxed);
                        beforeState = m_beforeState;
                        m_beforeState = 0;
                    }
                    if (enabled && beforeState)
                        enqueue(QStringLiteral("IPC"),
                                QStringLiteral("Log transport: %1 early events unavailable while detailed state was unknown")
                                    .arg(beforeState), false);
                    if (!m_diagnostic.isEmpty()) {
                        diagnostic(QStringLiteral("connection"), QStringLiteral("state received"));
                        m_diagnostic.clear();
                    }
                }
                m_busy = false;
                watcher->deleteLater();
            });
            return;
        }
        Event event;
        quint64 dropped;
        bool hadEvent;
        {
            QMutexLocker lock(&m_mutex);
            if (m_queue.empty() && !m_dropped) return;
            hadEvent = !m_queue.empty();
            event = !hadEvent ? Event{QStringLiteral("IPC"), QString(), false} : m_queue.front();
            if (!m_queue.empty()) {
                m_queue.pop_front();
                m_bytes -= event.bytes();
            }
            dropped = m_dropped;
            m_dropped = 0;
        }
        m_busy = true;
        QDBusMessage request = QDBusMessage::createMethodCall(
            m_owner, QString::fromLatin1(Path), QString::fromLatin1(Interface), QStringLiteral("Append"));
        request << event.source << event.message << event.error << QVariant::fromValue<qulonglong>(dropped);
        QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(request, CallTimeout), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, dropped, hadEvent]() {
            const QDBusMessage reply = watcher->reply();
            if (reply.type() != QDBusMessage::ReplyMessage || !reply.arguments().isEmpty()) {
                const bool countedByServer = reply.errorName() == QLatin1String("ru.androidcompatibilitylayer.appmanager.log.Error.Counted");
                {
                    QMutexLocker lock(&m_mutex);
                    const quint64 available = std::numeric_limits<quint64>::max() - m_dropped;
                    const quint64 lost = countedByServer ? 0 : dropped == std::numeric_limits<quint64>::max()
                        ? dropped : dropped + (hadEvent ? 1 : 0);
                    m_dropped += qMin(available, lost);
                }
                if (!countedByServer) {
                    diagnostic(QStringLiteral("event delivery"), failureCategory(reply));
                    m_owner.clear();
                    m_detailState.store(0, std::memory_order_relaxed);
                    clientDetailedState.store(false, std::memory_order_relaxed);
                }
            }
            m_busy = false;
            watcher->deleteLater();
        });
    }
    QDBusConnection m_bus;
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_lastState = -1;
    qint64 m_lastIdentity = -1;
    qint64 m_lastStatus = -1;
    QString m_owner;
    QMutex m_mutex;
    std::deque<Event> m_queue;
    int m_bytes = 0;
    quint64 m_dropped = 0;
    quint64 m_beforeState = 0;
    bool m_busy = false;
    bool m_statusBusy = false;
    bool m_wellKnownCompared = false;
    bool m_identityDetailsSent = false;
    std::atomic<int> m_detailState{0};
    QString m_diagnostic;
    QString m_statusStage = QStringLiteral("probe");
    QString m_statusCategory = QStringLiteral("started");
};

Client *client = nullptr;
Server *server = nullptr;
}

bool MemoryLogBus::startServer(const Receiver &receiver, const DetailedState &state, QString *error)
{
    if (error) error->clear();
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (server || !receiver || !state || !bus.isConnected() || !bus.interface()) {
        if (error) *error = QStringLiteral("Cannot initialize the log service.");
        return false;
    }
    const QDBusReply<QString> owner = bus.interface()->serviceOwner(QString::fromLatin1(Service));
    if (!owner.isValid() || owner.value() != bus.baseService()) {
        if (error) *error = QStringLiteral("The manager does not own its D-Bus service.");
        return false;
    }
    Server *candidate = new Server(bus, receiver, state);
    if (!bus.registerVirtualObject(QString::fromLatin1(Path), candidate, QDBusConnection::SingleNode)) {
        if (error) *error = bus.lastError().message();
        delete candidate;
        return false;
    }
    server = candidate;
    QObject::connect(server, &QObject::destroyed, []() { server = nullptr; });
    return true;
}

bool MemoryLogBus::startClient(QString *error)
{
    if (error) error->clear();
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (client || !QCoreApplication::instance() || !bus.isConnected()) {
        if (error) *error = QStringLiteral("Cannot initialize the log client.");
        return false;
    }
    client = new Client(bus);
    QObject::connect(client, &QObject::destroyed, []() { client = nullptr; });
    return true;
}

bool MemoryLogBus::clientActive() { return client != nullptr; }
bool MemoryLogBus::clientDetailedEnabled() { return clientDetailedState.load(std::memory_order_relaxed); }
void MemoryLogBus::enqueue(const QString &source, const QString &message, bool error)
{
    if (client) client->enqueue(source, message, error);
}
void MemoryLogBus::record(const QString &source, const QString &message, const QString &safe, bool error)
{
    if (client) client->record(source, message, safe, error);
}
QString MemoryLogBus::statistics()
{
    return server ? server->statistics() : QStringLiteral("Log transport unavailable in this process.\n");
}
