#ifndef MEGLER_AURORA_APK_STORE_H
#define MEGLER_AURORA_APK_STORE_H

#include <QString>
#include <QVariantList>

class ApkStore
{
public:
    explicit ApkStore(const QString &root);
    bool install(const QString &source, QString *message);
    bool remove(const QString &package, QString *message);
    bool recordLaunch(const QString &package, QString *message);
    bool permissions(const QString &package, QVariantList *names, QVariantList *maxSdk, QString *error) const;
    QVariantList apps() const;
    QString apkPath(const QString &package) const;
    QString dataPath(const QString &package) const;
    QString lockPath(const QString &package) const;

private:
    QString m_root;
};

#endif
