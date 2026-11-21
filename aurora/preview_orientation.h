#pragma once

#include <Qt>

inline Qt::ScreenOrientation previewNativeOrientation(Qt::ScreenOrientation native, bool wayland)
{
    return native == Qt::PrimaryOrientation && wayland ? Qt::PortraitOrientation : native;
}

inline int previewDisplayRotation(Qt::ScreenOrientation native, Qt::ScreenOrientation actual)
{
    const Qt::ScreenOrientation order[] = { Qt::PortraitOrientation, Qt::LandscapeOrientation,
        Qt::InvertedPortraitOrientation, Qt::InvertedLandscapeOrientation };
    int n = -1, a = -1;
    for (int i = 0; i != 4; ++i) {
        if (native == order[i]) n = i;
        if (actual == order[i]) a = i;
    }
    return n < 0 || a < 0 ? -1 : (a - n + 4) % 4;
}
