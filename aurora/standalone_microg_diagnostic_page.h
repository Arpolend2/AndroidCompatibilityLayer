#ifndef ACL_STANDALONE_MICROG_DIAGNOSTIC_PAGE_H
#define ACL_STANDALONE_MICROG_DIAGNOSTIC_PAGE_H

#include "standalone_microg_diagnostic.h"

#include <QQmlComponent>
#include <QQuickView>

namespace StandaloneMicrogDiagnostic {
inline QQmlComponent *createPage(QQuickView *view)
{
    auto *component = new QQmlComponent(view->engine(), view);
    component->setData(page, QUrl());
    return component;
}
}
#endif
