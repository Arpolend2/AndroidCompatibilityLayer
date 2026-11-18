#include "../camera_frame.h"

#include <cassert>
#include <vector>

int main()
{
    const uint8_t y[] = {1, 2, 99, 99, 3, 4, 99, 99};
    const uint8_t uv[] = {5, 6, 99, 99};
    std::vector<uint8_t> result;
    assert(aurora_camera::copyFrame(2, 2, y, sizeof(y), 4, uv, sizeof(uv), 4, false, &result));
    assert((result == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    assert(aurora_camera::copyFrame(2, 2, y, sizeof(y), 4, uv, sizeof(uv), 4, true, &result));
    assert((result == std::vector<uint8_t>{1, 2, 3, 4, 6, 5}));
    assert(!aurora_camera::copyFrame(2, 2, y, sizeof(y) - 1, 4, uv, sizeof(uv), 4, false, &result));
    assert(!aurora_camera::copyFrame(2, 2, y, sizeof(y), 4, uv, sizeof(uv) - 1, 4, false, &result));
    assert(!aurora_camera::copyFrame(3, 2, y, sizeof(y), 4, uv, sizeof(uv), 4, false, &result));
    assert(!aurora_camera::copyFrame(2, 2, y, sizeof(y), 1, uv, sizeof(uv), 4, false, &result));
    assert(!aurora_camera::copyFrame(2, 2, nullptr, sizeof(y), 4, uv, sizeof(uv), 4, false, &result));
    const uint8_t u[] = {5, 99};
    const uint8_t v[] = {6, 99};
    assert(aurora_camera::copyPlanarFrame(2, 2, y, sizeof(y), 4, u, sizeof(u), 2, v, sizeof(v), 2, &result));
    assert((result == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    assert(!aurora_camera::copyPlanarFrame(2, 2, y, sizeof(y), 4, u, 1, 2, v, sizeof(v), 2, &result));
    assert(!aurora_camera::copyPlanarFrame(2, 2, y, sizeof(y), 4, u, sizeof(u), 0, v, sizeof(v), 2, &result));
    // 4x4 NV12 decimated by 2 keeps samples (0,0),(0,2),(2,0),(2,2) and chroma pair (0,0).
    uint8_t y4[16], uv4[8];
    for (int i = 0; i < 16; ++i) y4[i] = static_cast<uint8_t>(i);
    for (int i = 0; i < 8; ++i) uv4[i] = static_cast<uint8_t>(100 + i);
    assert(aurora_camera::copyFrame(4, 4, y4, sizeof(y4), 4, uv4, sizeof(uv4), 4, false, &result, 2));
    assert((result == std::vector<uint8_t>{0, 2, 8, 10, 100, 101}));
    assert(aurora_camera::sensorOrientation(270, false) == 90 && aurora_camera::sensorOrientation(270, true) == 0);
    assert(aurora_camera::sensorOrientation(90, true) == 180 && aurora_camera::sensorOrientation(0, false) == 0);
    assert(aurora_camera::sensorOrientation(45, false) == -1 && aurora_camera::sensorOrientation(-90, true) == -1);
    assert(!aurora_camera::copyFrame(6, 4, y4, sizeof(y4), 6, uv4, sizeof(uv4), 6, false, &result, 2));
}
