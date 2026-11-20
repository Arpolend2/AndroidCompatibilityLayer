#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_LIBRARY_APP_INDEX_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_LIBRARY_APP_INDEX_H

#include <QThread>
#include <QString>

class LibraryAppIndex : public QThread
{
public:
    explicit LibraryAppIndex(const QString &root) : m_root(root) {}
    QString contents;
    QString error;

protected:
    void run() override;

private:
    QString m_root;
};

#endif
