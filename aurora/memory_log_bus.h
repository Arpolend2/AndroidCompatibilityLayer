#ifndef ANDROID_COMPATIBILITY_LAYER_MEMORY_LOG_BUS_H
#define ANDROID_COMPATIBILITY_LAYER_MEMORY_LOG_BUS_H

#include <QString>
#include <functional>

namespace MemoryLogBus {
using Receiver = std::function<void(const QString &, const QString &, const QString &, bool, quint64)>;
using DetailedState = std::function<bool()>;

bool startServer(const Receiver &receiver, const DetailedState &state, QString *error);
bool startClient(QString *error, bool sendAppend = true);
bool clientActive();
bool clientDetailedEnabled();
void enqueue(const QString &source, const QString &message, bool error);
void record(const QString &source, const QString &message, const QString &safe, bool error);
QString statistics();
}

#endif
