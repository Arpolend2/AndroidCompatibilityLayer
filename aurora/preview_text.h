#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_TEXT_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_TEXT_H

#include "../core/include/megler.h"

megler_gfx_host previewGraphics();
#ifdef MG_EXPERIMENTAL_GPU_CANVAS
class PreviewGpuCanvasHost;
megler_gfx_host previewGraphics(PreviewGpuCanvasHost *gpuCanvas);
#endif

#endif
