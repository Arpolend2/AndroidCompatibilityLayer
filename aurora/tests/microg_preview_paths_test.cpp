#include "../microg_preview_paths.h"

#include <cassert>

int main()
{
    const QString base = QStringLiteral("/home/user/app");
    const QString data = base + QStringLiteral("/services/data");
    const QString managed = base + QStringLiteral("/services/GmsCore.apk");
    const QString bundled = QStringLiteral("/runtime/services/GmsCore.apk");
    const QString chosen = QStringLiteral("/home/user/chosen.apk");
    assert(microgPreviewPaths(base, QStringLiteral("/runtime"), QString(), managed, data));
    assert(microgPreviewPaths(base, QStringLiteral("/runtime"), QString(), bundled, data));
    assert(microgPreviewPaths(base, QStringLiteral("/runtime"), chosen, chosen, data));
    assert(!microgPreviewPaths(base, QStringLiteral("/runtime"), QString(), chosen, data));
    assert(!microgPreviewPaths(base, QStringLiteral("/runtime"), chosen, chosen, base));
    assert(!microgPreviewPaths(QString(), QStringLiteral("/runtime"), chosen, chosen, data));
}
