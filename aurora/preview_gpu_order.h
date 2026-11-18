#ifndef MEGLER_AURORA_PREVIEW_GPU_ORDER_H
#define MEGLER_AURORA_PREVIEW_GPU_ORDER_H

#include "../core/include/megler.h"

typedef struct {
    bool shown;
    int flags;
} PreviewGpuOrderLayer;

/* -1 denotes the premultiplied Java view picture. */
static inline int previewGpuOrder(const PreviewGpuOrderLayer *layers, int count, int *out)
{
    int pass, i, n = 0;
    for (pass = 0; pass < 3; ++pass) {
        if (pass == 2) out[n++] = -1;
        for (i = 0; i < count; ++i) {
            const int which = (layers[i].flags & MEGLER_GL_ABOVE) ? 2 :
                              (layers[i].flags & MEGLER_GL_OVERLAY) ? 1 : 0;
            if (layers[i].shown && which == pass) out[n++] = i;
        }
    }
    return n;
}

#endif
