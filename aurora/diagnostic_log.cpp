#include "diagnostic_log.h"
#include "error_log.h"

#include <QByteArray>
#include <QMap>
#include <QProcess>
#include <QTextCodec>
#include <QTimer>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <thread>
#include <unistd.h>

namespace {
const int MaxText = 131072;
const size_t MaxForward = 4 * 1024 * 1024;
const char Truncated[] = "[Earlier log output was truncated or lost.]\n";
const char ForwardLost[] = "[Some output could not be forwarded to the IDE.]\n";
std::mutex messageMutex;

struct Decoder {
    QTextCodec::ConverterState state;
    bool lineStart = true;
};

struct Forward {
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<QByteArray> queue;
    size_t bytes = 0;
    bool stopping = false;
    int fd = -1;
};
}

struct DiagnosticLog::State {
    std::mutex mutex;
    QMap<QString, std::shared_ptr<Decoder>> decoders;
    QString buffer;
    QString lastSource;
    bool lastLineStart = true;
    bool truncated = false;
    bool forwardLost = false;
    quint64 revision = 0;
    int readFd[2] = {-1, -1};
    int savedFd[2] = {-1, -1};
    std::shared_ptr<Forward> forward[2];
    std::thread reader;
    bool stop = false;
    bool optIn = false;

    void addText(const QString &source, const QString &value, Decoder &decoder)
    {
        for (const QChar ch : value) {
            if (source != lastSource && !lastLineStart) {
                buffer += QLatin1Char('\n');
                lastLineStart = true;
            }
            if (decoder.lineStart || source != lastSource) {
                buffer += QLatin1Char('[') + source + QStringLiteral("] ");
                decoder.lineStart = false;
            }
            buffer += ch;
            lastSource = source;
            lastLineStart = ch == QLatin1Char('\n');
            if (lastLineStart) decoder.lineStart = true;
        }
        if (buffer.size() > MaxText) {
            int cut = buffer.size() - MaxText;
            if (cut < buffer.size() && buffer.at(cut).isLowSurrogate()) ++cut;
            buffer.remove(0, cut);
            truncated = true;
        }
        ++revision;
    }

    void ingest(const QString &source, const QByteArray &bytes, int output, bool persist = false)
    {
        if (bytes.isEmpty()) return;
        QString text;
        if (!optIn || ErrorLog::detailedCaptureEnabled()) {
            std::lock_guard<std::mutex> lock(mutex);
            auto &decoder = decoders[source];
            if (!decoder) decoder = std::make_shared<Decoder>();
            text = QTextCodec::codecForName("UTF-8")->toUnicode(
                bytes.constData(), bytes.size(), &decoder->state);
            if (!text.isEmpty()) addText(source, text, *decoder);
        }
        if (persist && !text.isEmpty()) ErrorLog::record(source, text, false);
        const auto writer = forward[output];
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(writer->mutex);
            if (size_t(bytes.size()) > MaxForward - writer->bytes) {
                lost = true;
            } else {
                writer->queue.push_back(bytes);
                writer->bytes += size_t(bytes.size());
            }
        }
        if (lost) {
            std::lock_guard<std::mutex> lock(mutex);
            forwardLost = true;
            ++revision;
        } else {
            writer->ready.notify_one();
        }
    }

    void finish(const QString &source)
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto decoder = decoders.take(source);
        if (decoder && decoder->state.remainingChars > 0)
            addText(source, QString(QChar::ReplacementCharacter), *decoder);
    }
};

std::shared_ptr<DiagnosticLog::State> DiagnosticLog::s_messageState;
QtMessageHandler DiagnosticLog::s_previousHandler = nullptr;

void DiagnosticLog::messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    std::shared_ptr<State> state;
    QtMessageHandler previous;
    {
        std::lock_guard<std::mutex> lock(messageMutex);
        state = s_messageState;
        previous = s_previousHandler;
    }
    if (previous != &DiagnosticLog::childMessageHandler)
        ErrorLog::record(QStringLiteral("Qt"), message, type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg);
    if (state) state->ingest(QStringLiteral("controller Qt"),
                             (qFormatLogMessage(type, context, message) + QLatin1Char('\n')).toUtf8(), 1);
    if (previous) previous(type, context, message);
}

void DiagnosticLog::childMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    ErrorLog::record(QStringLiteral("Qt"), message, type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg);
    const QByteArray data = (qFormatLogMessage(type, context, message) + QLatin1Char('\n')).toUtf8();
    int pos = 0;
    while (pos < data.size()) {
        const ssize_t count = ::write(STDERR_FILENO, data.constData() + pos, size_t(data.size() - pos));
        if (count > 0) pos += int(count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
    }
}

void DiagnosticLog::installChildQtMessageHandler()
{
    qInstallMessageHandler(&DiagnosticLog::childMessageHandler);
}

DiagnosticLog::DiagnosticLog(QObject *parent)
    : QObject(parent), m_state(std::make_shared<State>()), m_timer(new QTimer(this))
{
    m_timer->setInterval(100);
    auto revision = std::make_shared<quint64>(0);
    connect(m_timer, &QTimer::timeout, this, [this, revision]() {
        quint64 current;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            current = m_state->revision;
        }
        if (*revision != current) {
            *revision = current;
            emit changed();
        }
    });
}

bool DiagnosticLog::start(QString *error, bool persistStreams)
{
    if (error) error->clear();
    if (available()) {
        if (error) *error = QStringLiteral("Log capture is already running.");
        return false;
    }
    m_state->optIn = persistStreams;
    int pipes[2][2] = {{-1, -1}, {-1, -1}};
    for (int i = 0; i < 2; ++i) {
        m_state->savedFd[i] = ::fcntl(i + 1, F_DUPFD_CLOEXEC, 3);
        if (m_state->savedFd[i] < 0 || ::pipe2(pipes[i], O_CLOEXEC) < 0 ||
            ::fcntl(pipes[i][0], F_SETFL, O_NONBLOCK) < 0) {
            const QString why = QString::fromLocal8Bit(std::strerror(errno));
            for (int j = 0; j < 2; ++j) {
                if (pipes[j][0] >= 0) ::close(pipes[j][0]);
                if (pipes[j][1] >= 0) ::close(pipes[j][1]);
                if (m_state->savedFd[j] >= 0) ::close(m_state->savedFd[j]);
                m_state->savedFd[j] = -1;
            }
            if (error) *error = why;
            return false;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (::dup2(pipes[i][1], i + 1) < 0) {
            const QString why = QString::fromLocal8Bit(std::strerror(errno));
            for (int j = 0; j < 2; ++j) {
                if (j < i) ::dup2(m_state->savedFd[j], j + 1);
                ::close(pipes[j][0]);
                ::close(pipes[j][1]);
                ::close(m_state->savedFd[j]);
                m_state->savedFd[j] = -1;
            }
            if (error) *error = why;
            return false;
        }
    }
    for (int i = 0; i < 2; ++i) {
        ::close(pipes[i][1]);
        m_state->readFd[i] = pipes[i][0];
        m_state->forward[i] = std::make_shared<Forward>();
        m_state->forward[i]->fd = ::fcntl(m_state->savedFd[i], F_DUPFD_CLOEXEC, 3);
        const auto state = m_state;
        const auto writer = state->forward[i];
        std::thread([state, writer]() {
            sigset_t blockedSignals;
            sigemptyset(&blockedSignals);
            sigaddset(&blockedSignals, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &blockedSignals, nullptr);
            for (;;) {
                QByteArray data;
                {
                    std::unique_lock<std::mutex> lock(writer->mutex);
                    writer->ready.wait(lock, [&]() { return writer->stopping || !writer->queue.empty(); });
                    if (writer->queue.empty()) break;
                    data = writer->queue.front();
                    writer->queue.pop_front();
                }
                int pos = 0;
                while (pos < data.size()) {
                    const ssize_t count = ::write(writer->fd, data.constData() + pos, size_t(data.size() - pos));
                    if (count > 0) pos += int(count);
                    else if (count < 0 && errno == EINTR) continue;
                    else {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->forwardLost = true;
                        ++state->revision;
                        break;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(writer->mutex);
                    writer->bytes -= size_t(data.size());
                }
            }
            if (writer->fd >= 0) ::close(writer->fd);
        }).detach();
    }
    const auto state = m_state;
    state->reader = std::thread([state, persistStreams]() {
        pollfd descriptors[2] = {{state->readFd[0], POLLIN, 0}, {state->readFd[1], POLLIN, 0}};
        char data[4096];
        for (;;) {
            ::poll(descriptors, 2, 50);
            for (int i = 0; i < 2; ++i) {
                for (int n = 0; n < 128; ++n) {
                    const ssize_t count = ::read(descriptors[i].fd, data, sizeof(data));
                    if (count <= 0) break;
                    state->ingest(i == 0 ? QStringLiteral("controller stdout") : QStringLiteral("controller stderr"),
                                  QByteArray(data, int(count)), i, persistStreams);
                }
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->stop) break;
        }
        state->finish(QStringLiteral("controller stdout"));
        state->finish(QStringLiteral("controller stderr"));
    });
    {
        std::lock_guard<std::mutex> lock(messageMutex);
        s_messageState = state;
        s_previousHandler = qInstallMessageHandler(&DiagnosticLog::messageHandler);
    }
    return true;
}

void DiagnosticLog::startNotifications()
{
    m_timer->start();
}

QString DiagnosticLog::standaloneDiagnosticFileError() const
{
    return ErrorLog::standaloneDiagnosticFileError();
}

QString DiagnosticLog::overlayText() const
{
    return ErrorLog::overlayText();
}

DiagnosticLog::~DiagnosticLog()
{
    if (!available()) return;
    {
        std::lock_guard<std::mutex> lock(messageMutex);
        qInstallMessageHandler(s_previousHandler);
        s_previousHandler = nullptr;
        s_messageState.reset();
    }
    for (int i = 0; i < 2; ++i) {
        ::dup2(m_state->savedFd[i], i + 1);
        ::close(m_state->savedFd[i]);
        m_state->savedFd[i] = -1;
    }
    {
        std::lock_guard<std::mutex> lock(m_state->mutex);
        m_state->stop = true;
    }
    m_state->reader.join();
    for (int i = 0; i < 2; ++i) {
        ::close(m_state->readFd[i]);
        const auto writer = m_state->forward[i];
        {
            std::lock_guard<std::mutex> lock(writer->mutex);
            writer->stopping = true;
        }
        writer->ready.notify_one();
    }
}

bool DiagnosticLog::available() const { return m_state->savedFd[0] >= 0; }

QString DiagnosticLog::text() const
{
    std::lock_guard<std::mutex> lock(m_state->mutex);
    return (m_state->truncated ? QString::fromLatin1(Truncated) : QString()) +
           (m_state->forwardLost ? QString::fromLatin1(ForwardLost) : QString()) + m_state->buffer;
}

void DiagnosticLog::append(const QString &source, const QByteArray &bytes, int output)
{
    m_state->ingest(source, bytes, output, true);
}

void DiagnosticLog::finish(const QString &source)
{
    m_state->finish(source);
}

void DiagnosticLog::watch(QProcess *process, const QString &source,
                          const std::function<void(bool, const QByteArray &)> &observer)
{
    Q_ASSERT(available());
    const QString out = source + QStringLiteral(" stdout");
    const QString err = source + QStringLiteral(" stderr");
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process, out, observer]() {
        const QByteArray bytes = process->readAllStandardOutput();
        if (observer && !bytes.isEmpty()) observer(false, bytes);
        append(out, bytes, 0);
    });
    connect(process, &QProcess::readyReadStandardError, this,
            [this, process, err, observer]() {
        const QByteArray bytes = process->readAllStandardError();
        if (observer && !bytes.isEmpty()) observer(true, bytes);
        append(err, bytes, 1);
    });
    connect(process, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this, process, out, err, observer](int, QProcess::ExitStatus) {
        const QByteArray stdoutBytes = process->readAllStandardOutput();
        const QByteArray stderrBytes = process->readAllStandardError();
        if (observer && !stdoutBytes.isEmpty()) observer(false, stdoutBytes);
        if (observer && !stderrBytes.isEmpty()) observer(true, stderrBytes);
        append(out, stdoutBytes, 0);
        append(err, stderrBytes, 1);
        finish(out);
        finish(err);
    });
}
