#ifndef MEGLER_AURORA_APP_CACHE_CLEAR_H
#define MEGLER_AURORA_APP_CACHE_CLEAR_H

#include <QString>

namespace AppCacheClear {

bool request(const QString &appDataDirectory, const QString &package, QString *error = nullptr);

}

#endif
