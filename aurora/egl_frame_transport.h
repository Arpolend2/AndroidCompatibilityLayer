#ifndef MEGLER_AURORA_EGL_FRAME_TRANSPORT_H
#define MEGLER_AURORA_EGL_FRAME_TRANSPORT_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    EGLDisplay display;
    EGLImageKHR image;
    EGLSyncKHR fence;
    int width;
    int height;
} AuroraEglFrame;

typedef enum {
    AURORA_EGL_FRAME_REJECTED,
    AURORA_EGL_FRAME_READY,
    AURORA_EGL_FRAME_CONTEXT_LOST
} AuroraEglFrameResult;

/* A non-empty failure buffer receives the first rejected capture step. */
/* The caller owns a READY frame and must release it, including on failure.
   opaque makes alpha 1 without changing RGB; otherwise alpha is preserved. */
AuroraEglFrameResult aurora_egl_frame_capture(EGLDisplay display, EGLSurface surface,
                                               int width, int height, int opaque, AuroraEglFrame *frame,
                                               char *failure, size_t failure_size);
/* Called with a consumer GLES context current on the frame's display.
   The caller owns the returned texture in that context. */
GLuint aurora_egl_frame_import(const AuroraEglFrame *frame);
void aurora_egl_frame_release(AuroraEglFrame *frame);

#ifdef __cplusplus
}
#endif

#endif
