#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_CANDIDATE_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_INSTALL_REQUEST_CANDIDATE_H

#include "install_request_snapshot.h"
#include <QString>

// Confine the candidate to one thread at a time. Prepare off the UI and guest
// callback threads; retain the object until the confirmed installation ends.
class InstallRequestCandidate
{
public:
    bool prepare(const QString &senderRoot, const QString &source,
                 const QString &name, quint64 maxBytes, QString *error);
    QString path() const;
    QString package() const { return m_package; }
    QString label() const { return m_label; }
    quint32 version() const { return m_version; }
    void clear();

private:
    InstallRequestSnapshot m_snapshot;
    QString m_package;
    QString m_label;
    quint32 m_version = 0;
};

#endif
