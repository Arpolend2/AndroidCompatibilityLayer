#include "../preview_gpu_order.h"

#include <cstdio>

int main()
{
    const PreviewGpuOrderLayer layers[] = {
        {true, MEGLER_GL_ABOVE | MEGLER_GL_TRANSLUCENT},
        {true, MEGLER_GL_OVERLAY},
        {true, 0},
        {false, MEGLER_GL_ABOVE},
        {true, MEGLER_GL_ABOVE | MEGLER_GL_OVERLAY},
        {true, MEGLER_GL_OVERLAY},
        {true, 0},
    };
    const int expected[] = {2, 6, 1, 5, -1, 0, 4};
    int order[8] = {};
    const int count = previewGpuOrder(layers, 7, order);
    if (count != 7) return 1;
    for (int i = 0; i < count; ++i) if (order[i] != expected[i]) return 1;
    std::puts("GPU layer ordering passed");
    return 0;
}
