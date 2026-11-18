#ifndef MEGLER_AURORA_RPM_SHORTCUT_H
#define MEGLER_AURORA_RPM_SHORTCUT_H

#include <QString>

namespace RpmShortcut {

// Builds and signs (Developer Mode test key) an RPM that adds an app grid icon
// launching the given installed APK. Returns the .rpm path or an empty string.
QString create(const QString &package, const QString &label, const QString &iconDataUri, QString *error);

}

#endif
