#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_TEXT_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_TEXT_H

#include "../core/include/megler.h"

megler_gfx_host previewGraphics();
struct SharedMicrogGpuCanvas;
SharedMicrogGpuCanvas *createSharedMicrogGpuCanvas(
    megler_gfx_host *graphics,
    bool (MEGLER_CB *present)(void *, const void *, int, int, int),
    bool (MEGLER_CB *presentTexture)(void *, uint32_t, int, int), void *user);
void destroySharedMicrogGpuCanvas(SharedMicrogGpuCanvas *canvas);
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
class PreviewGpuCanvasHost;
megler_gfx_host previewGraphics(PreviewGpuCanvasHost *gpuCanvas);
#endif

#endif
