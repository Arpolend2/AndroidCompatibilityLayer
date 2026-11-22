#ifndef ANDROID_COMPATIBILITY_LAYER_PACKAGE_ARCHITECTURE_H
#define ANDROID_COMPATIBILITY_LAYER_PACKAGE_ARCHITECTURE_H

#include <QString>

inline QString packageBuildArchitecture()
{
#if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("aarch64");
#elif defined(__arm__) && defined(__ARM_PCS_VFP)
    return QStringLiteral("armv7hl");
#elif defined(Q_PROCESSOR_X86_64)
    return QStringLiteral("x86_64");
#else
    return QString();
#endif
}

#endif
