#include "egl_frame_transport.h"

#include <GLES2/gl2ext.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int has_extension(const char *extensions, const char *name)
{
    const size_t length = strlen(name);
    const char *p = extensions;
    if (!p) return 0;
    while ((p = strstr(p, name)) != NULL) {
        if ((p == extensions || p[-1] == ' ') && (p[length] == ' ' || p[length] == '\0')) return 1;
        p += length;
    }
    return 0;
}

static int has_gl_extension(const char *name)
{
    const char *version = (const char *)glGetString(GL_VERSION);
    if (!version) return 0;
    if (strstr(version, "OpenGL ES 3")) {
        typedef const GLubyte *(GL_APIENTRYP AuroraGetStringi)(GLenum, GLuint);
        AuroraGetStringi get_string_i = (AuroraGetStringi)eglGetProcAddress("glGetStringi");
        GLint count = 0;
        GLint i;
        if (!get_string_i) return 0;
        glGetIntegerv(0x821D, &count); /* GL_NUM_EXTENSIONS */
        if (count < 0 || count > 65536) return 0;
        for (i = 0; i < count; ++i) {
            const char *extension = (const char *)get_string_i(GL_EXTENSIONS, (GLuint)i);
            if (extension && strcmp(extension, name) == 0) return 1;
        }
        return 0;
    }
    return has_extension((const char *)glGetString(GL_EXTENSIONS), name);
}

static void set_failure(char *failure, size_t failure_size, const char *message)
{
    if (failure && failure_size) snprintf(failure, failure_size, "%s", message);
}

static void set_egl_failure(char *failure, size_t failure_size, const char *operation)
{
    if (failure && failure_size)
        snprintf(failure, failure_size, "%s failed (EGL error 0x%04x)", operation, (unsigned)eglGetError());
}

static AuroraEglFrameResult reject(char *failure, size_t failure_size, const char *message)
{
    set_failure(failure, failure_size, message);
    return AURORA_EGL_FRAME_REJECTED;
}

static AuroraEglFrameResult reject_egl(char *failure, size_t failure_size, const char *operation)
{
    set_egl_failure(failure, failure_size, operation);
    return AURORA_EGL_FRAME_REJECTED;
}

void aurora_egl_frame_release(AuroraEglFrame *frame)
{
    if (!frame) return;
    if (frame->fence) {
        PFNEGLDESTROYSYNCKHRPROC destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
        if (!destroy_sync || !destroy_sync(frame->display, frame->fence))
            fprintf(stderr, "EGL frame fence destruction failed\n");
    }
    if (frame->image) {
        PFNEGLDESTROYIMAGEKHRPROC destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        if (!destroy_image || !destroy_image(frame->display, frame->image))
            fprintf(stderr, "EGL frame image destruction failed\n");
    }
    memset(frame, 0, sizeof(*frame));
}

AuroraEglFrameResult aurora_egl_frame_capture(EGLDisplay display, EGLSurface surface,
                                               int width, int height, int opaque, AuroraEglFrame *frame,
                                               char *failure, size_t failure_size)
{
    const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const EGLint image_attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLContext original = eglGetCurrentContext(), copy_context = EGL_NO_CONTEXT;
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    EGLConfig config = NULL;
    EGLint config_id = 0, count = 0, actual_width = 0, actual_height = 0;
    EGLSyncKHR producer_fence = EGL_NO_SYNC_KHR;
    GLuint source = 0, fbo = 0;
    int switched = 0, restored = 1, ok = 0;
    const char *extensions;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    if (failure && failure_size) failure[0] = '\0';
    if (!frame) return reject(failure, failure_size, "frame output is null");
    memset(frame, 0, sizeof(*frame));
    if (display == EGL_NO_DISPLAY) return reject(failure, failure_size, "no EGL display");
    if (eglGetCurrentDisplay() != display)
        return reject(failure, failure_size, "current GLES context uses a different EGL display");
    if (eglQueryAPI() != EGL_OPENGL_ES_API)
        return reject(failure, failure_size, "current EGL API is not OpenGL ES");
    if (original == EGL_NO_CONTEXT) return reject(failure, failure_size, "no current EGL context");
    if (surface == EGL_NO_SURFACE) return reject(failure, failure_size, "no EGL surface");
    if (draw != surface || read != surface)
        return reject(failure, failure_size, "EGL surface is not current for both drawing and reading");
    if (width <= 0 || height <= 0) return reject(failure, failure_size, "invalid EGL frame dimensions");
    extensions = eglQueryString(display, EGL_EXTENSIONS);
    if (!has_extension(extensions, "EGL_KHR_image_base"))
        return reject(failure, failure_size, "missing EGL_KHR_image_base");
    if (!has_extension(extensions, "EGL_KHR_gl_texture_2D_image"))
        return reject(failure, failure_size, "missing EGL_KHR_gl_texture_2D_image");
    if (!has_extension(extensions, "EGL_KHR_fence_sync"))
        return reject(failure, failure_size, "missing EGL_KHR_fence_sync");
    if (!has_extension(extensions, "EGL_KHR_wait_sync"))
        return reject(failure, failure_size, "missing EGL_KHR_wait_sync");
    create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    create_sync = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    destroy_sync = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    wait_sync = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    if (!create_image) return reject(failure, failure_size, "eglCreateImageKHR is unavailable");
    if (!create_sync) return reject(failure, failure_size, "eglCreateSyncKHR is unavailable");
    if (!destroy_sync) return reject(failure, failure_size, "eglDestroySyncKHR is unavailable");
    if (!wait_sync) return reject(failure, failure_size, "eglWaitSyncKHR is unavailable");
    if (!eglGetProcAddress("eglDestroyImageKHR"))
        return reject(failure, failure_size, "eglDestroyImageKHR is unavailable");
    if (!eglQueryContext(display, original, EGL_CONFIG_ID, &config_id))
        return reject_egl(failure, failure_size, "eglQueryContext(EGL_CONFIG_ID)");
    if (!eglQuerySurface(display, surface, EGL_WIDTH, &actual_width))
        return reject_egl(failure, failure_size, "eglQuerySurface(EGL_WIDTH)");
    if (!eglQuerySurface(display, surface, EGL_HEIGHT, &actual_height))
        return reject_egl(failure, failure_size, "eglQuerySurface(EGL_HEIGHT)");
    if (width != actual_width || height != actual_height) {
        if (failure && failure_size)
            snprintf(failure, failure_size, "EGL surface is %dx%d; frame is %dx%d",
                     actual_width, actual_height, width, height);
        return AURORA_EGL_FRAME_REJECTED;
    }
    if (config_id) {
        EGLConfig matching = NULL;
        EGLint attrs[] = { EGL_CONFIG_ID, config_id, EGL_NONE };
        if (!eglChooseConfig(display, attrs, &matching, 1, &count)) {
            if (!has_extension(extensions, "EGL_KHR_no_config_context"))
                return reject_egl(failure, failure_size, "eglChooseConfig");
            (void)eglGetError();
        } else if (count == 1) {
            config = matching;
        }
    }
    if (!config) {
        if (!has_extension(extensions, "EGL_KHR_no_config_context"))
            return reject(failure, failure_size, "no EGL config matches the guest context");
        /* Megler can create a guest context without a config; the copy context must match it. */
        config = EGL_NO_CONFIG_KHR;
    }
    copy_context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (copy_context == EGL_NO_CONTEXT) return reject_egl(failure, failure_size, "eglCreateContext for frame copy");
    producer_fence = create_sync(display, EGL_SYNC_FENCE_KHR, NULL);
    if (producer_fence == EGL_NO_SYNC_KHR) {
        set_egl_failure(failure, failure_size, "eglCreateSyncKHR producer fence");
        goto done;
    }
    glFlush();
    if (!eglMakeCurrent(display, surface, surface, copy_context)) {
        set_egl_failure(failure, failure_size, "eglMakeCurrent for frame copy");
        goto done;
    }
    switched = 1;
    if (!wait_sync(display, producer_fence, 0)) {
        set_egl_failure(failure, failure_size, "eglWaitSyncKHR producer fence");
        goto done;
    }
    glGenTextures(1, &source);
    glBindTexture(GL_TEXTURE_2D, source);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, width, height, 0);
    {
        const GLenum error = glGetError();
        if (!source || error != GL_NO_ERROR) {
            if (failure && failure_size)
                snprintf(failure, failure_size, "glCopyTexImage2D failed (texture %s; GL error 0x%04x)",
                         source ? "created" : "not created", error);
            goto done;
        }
    }
    if (opaque) {
        GLenum status;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
        status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (!fbo || status != GL_FRAMEBUFFER_COMPLETE) {
            if (failure && failure_size)
                snprintf(failure, failure_size, "frame-copy framebuffer is incomplete (status 0x%04x)", status);
            goto done;
        }
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        {
            const GLenum error = glGetError();
            if (error != GL_NO_ERROR) {
                if (failure && failure_size)
                    snprintf(failure, failure_size, "frame-copy alpha clear failed (GL error 0x%04x)", error);
                goto done;
            }
        }
    }
    frame->display = display;
    frame->image = create_image(display, copy_context, EGL_GL_TEXTURE_2D_KHR,
                                (EGLClientBuffer)(uintptr_t)source, image_attrs);
    if (frame->image == EGL_NO_IMAGE_KHR) {
        set_egl_failure(failure, failure_size, "eglCreateImageKHR");
        goto done;
    }
    frame->fence = create_sync(display, EGL_SYNC_FENCE_KHR, NULL);
    if (frame->fence == EGL_NO_SYNC_KHR) {
        set_egl_failure(failure, failure_size, "eglCreateSyncKHR consumer fence");
        goto done;
    }
    frame->width = width;
    frame->height = height;
    glFlush();
    ok = 1;
done:
    if (switched && !ok) glFinish();
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (source) glDeleteTextures(1, &source);
    if (switched) {
        restored = eglMakeCurrent(display, draw, read, original);
        /* Do not let the app overwrite the surface before the copy has finished. */
        if (!restored) set_egl_failure(failure, failure_size, "eglMakeCurrent restoring guest context");
        if (restored && ok && !wait_sync(display, frame->fence, 0)) {
            set_egl_failure(failure, failure_size, "eglWaitSyncKHR consumer fence");
            restored = 0;
        }
    }
    if (producer_fence != EGL_NO_SYNC_KHR && !destroy_sync(display, producer_fence))
        fprintf(stderr, "EGL producer fence destruction failed\n");
    if (copy_context != EGL_NO_CONTEXT && !eglDestroyContext(display, copy_context))
        fprintf(stderr, "EGL copy context destruction failed\n");
    if (!ok || !restored) aurora_egl_frame_release(frame);
    if (!restored) return AURORA_EGL_FRAME_CONTEXT_LOST;
    if (!ok && failure && failure_size && !failure[0])
        set_failure(failure, failure_size, "native EGL frame copy failed");
    return ok ? AURORA_EGL_FRAME_READY : AURORA_EGL_FRAME_REJECTED;
}

GLuint aurora_egl_frame_import(const AuroraEglFrame *frame)
{
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;
    GLint old_texture = 0;
    GLuint texture = 0;
    if (!frame || !frame->image || !frame->fence || eglGetCurrentContext() == EGL_NO_CONTEXT ||
        eglGetCurrentDisplay() != frame->display ||
        !has_gl_extension("GL_OES_EGL_image")) return 0;
    wait_sync = (PFNEGLWAITSYNCKHRPROC)eglGetProcAddress("eglWaitSyncKHR");
    image_target = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!wait_sync || !image_target || !wait_sync(frame->display, frame->fence, 0)) return 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    image_target(GL_TEXTURE_2D, frame->image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!texture || glGetError() != GL_NO_ERROR) {
        if (texture) glDeleteTextures(1, &texture);
        texture = 0;
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)old_texture);
    return texture;
}
