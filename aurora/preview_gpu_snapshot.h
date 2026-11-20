#ifndef ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_SNAPSHOT_H
#define ANDROID_COMPATIBILITY_LAYER_AURORA_PREVIEW_GPU_SNAPSHOT_H

#include <QOpenGLFunctions>

// Called with the consumer context current while the producer is waiting.
static inline GLuint previewGpuSnapshot(QOpenGLFunctions *gl, GLuint source, int width, int height,
                                        GLuint reusable = 0)
{
    if (!gl || !source || source == reusable || width <= 0 || height <= 0)
        return 0;
    for (int i = 0; gl->glGetError() != GL_NO_ERROR; ++i) {
        if (i == 15)
            return 0;
    }
    GLint previousFramebuffer = 0, previousTexture = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    GLuint framebuffer = 0, snapshot = reusable;
    gl->glGenFramebuffers(1, &framebuffer);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
    bool ok = framebuffer && gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        if (!snapshot)
            gl->glGenTextures(1, &snapshot);
        gl->glBindTexture(GL_TEXTURE_2D, snapshot);
        if (reusable) {
            gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
        } else {
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            gl->glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, width, height, 0);
        }
        // The source may be modified immediately after the caller unlocks.
        gl->glFinish();
        ok = snapshot && gl->glGetError() == GL_NO_ERROR;
    }
    gl->glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    gl->glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    if (framebuffer)
        gl->glDeleteFramebuffers(1, &framebuffer);
    if (!ok && snapshot && !reusable)
        gl->glDeleteTextures(1, &snapshot);
    return ok ? snapshot : 0;
}

#endif
