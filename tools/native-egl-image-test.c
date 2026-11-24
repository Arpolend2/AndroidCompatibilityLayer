/* Pixel readback is used only to verify the result. */
#include "../aurora/egl_frame_transport.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int has_extension(const char *extensions, const char *name)
{
    size_t length = strlen(name);
    const char *p;
    if (!extensions) return 0;
    p = extensions;
    while ((p = strstr(p, name)) != NULL) {
        if ((p == extensions || p[-1] == ' ') && (p[length] == ' ' || p[length] == '\0')) return 1;
        p += length;
    }
    return 0;
}

#define REQUIRE(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s (EGL 0x%x, GL 0x%x)\n", message, eglGetError(), glGetError()); \
        return 1; \
    } \
} while (0)

int main(void)
{
    static const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    static const EGLint surface_attrs[] = { EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE };
    static const EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig config;
    EGLint count;
    EGLContext producer, consumer;
    EGLSurface producer_surface, consumer_surface;
    int frame;

    REQUIRE(display != EGL_NO_DISPLAY && eglInitialize(display, NULL, NULL), "EGL display unavailable");
    const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
    REQUIRE(has_extension(extensions, "EGL_KHR_image_base") &&
            has_extension(extensions, "EGL_KHR_gl_texture_2D_image") &&
            has_extension(extensions, "EGL_KHR_fence_sync") &&
            has_extension(extensions, "EGL_KHR_wait_sync"), "Required EGL extensions unavailable");
    REQUIRE(eglBindAPI(EGL_OPENGL_ES_API) &&
            eglChooseConfig(display, config_attrs, &config, 1, &count) && count == 1, "EGL configuration unavailable");
    producer = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    consumer = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    producer_surface = eglCreatePbufferSurface(display, config, surface_attrs);
    consumer_surface = eglCreatePbufferSurface(display, config, surface_attrs);
    REQUIRE(producer != EGL_NO_CONTEXT && consumer != EGL_NO_CONTEXT &&
            producer_surface != EGL_NO_SURFACE && consumer_surface != EGL_NO_SURFACE, "EGL contexts or surfaces unavailable");

    if (has_extension(extensions, "EGL_KHR_no_config_context")) {
        EGLContext no_config = eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attrs);
        AuroraEglFrame image;
        char failure[160];
        REQUIRE(no_config != EGL_NO_CONTEXT, "No-config EGL context unavailable");
        REQUIRE(eglMakeCurrent(display, producer_surface, producer_surface, no_config),
                "No-config context could not use the producer surface");
        REQUIRE(aurora_egl_frame_capture(display, producer_surface, 4, 4, 0, &image,
                                         failure, sizeof(failure)) == AURORA_EGL_FRAME_READY,
                failure);
        aurora_egl_frame_release(&image);
        REQUIRE(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT),
                "Could not release the no-config context");
        REQUIRE(eglDestroyContext(display, no_config), "Could not destroy the no-config context");
    }

    for (frame = 0; frame < 16; ++frame) {
        GLuint target = 0, fbo = 0, app_texture = 0, app_fbo = 0;
        GLint binding = 0;
        AuroraEglFrame image;
        char failure[160];
        const int green = frame & 1;
        const int opaque = frame & 2;
        GLubyte pixel[4] = {0, 0, 0, 0}, top[4] = {0, 0, 0, 0}, clear[4] = {0, 0, 0, 0};
        GLubyte source_rgba[4 * 4 * 4] = {0};
        REQUIRE(eglMakeCurrent(display, producer_surface, producer_surface, producer), "Producer context unavailable");
        if (!frame) {
            REQUIRE(eglMakeCurrent(display, producer_surface, consumer_surface, producer) &&
                    aurora_egl_frame_capture(display, producer_surface, 4, 4, opaque, &image,
                                             failure, sizeof(failure)) == AURORA_EGL_FRAME_REJECTED &&
                    strstr(failure, "not current for both drawing and reading") &&
                    eglMakeCurrent(display, producer_surface, producer_surface, producer),
                    "Capture accepted different draw and read surfaces");
        }
        glDisable(GL_SCISSOR_TEST);
        glClearColor(green ? 0.0f : 1.0f, green ? 1.0f : 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 2, 4, 2);
        glClearColor(0.0f, 0.0f, 1.0f, 0.5f);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(2, 2, 2, 2);
        glClearColor(1.0f, 0.0f, 1.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glGenTextures(1, &app_texture);
        glBindTexture(GL_TEXTURE_2D, app_texture);
        glGenFramebuffers(1, &app_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, app_fbo);
        REQUIRE(aurora_egl_frame_capture(display, producer_surface, 3, 4, opaque, &image,
                                         failure, sizeof(failure)) == AURORA_EGL_FRAME_REJECTED &&
                strstr(failure, "EGL surface is 4x4; frame is 3x4"),
                "Capture accepted an incorrect surface size");
        REQUIRE(aurora_egl_frame_capture(EGL_NO_DISPLAY, producer_surface, 4, 4, opaque, &image,
                                         failure, sizeof(failure)) == AURORA_EGL_FRAME_REJECTED &&
                strstr(failure, "no EGL display"),
                "Capture accepted an incorrect display");
        glEnable(0xffffffffu);
        REQUIRE(aurora_egl_frame_capture_with_readback(display, producer_surface, 4, 4, opaque, &image,
                                                        source_rgba, sizeof(source_rgba),
                                                        failure, sizeof(failure)) == AURORA_EGL_FRAME_READY &&
                !failure[0],
                "EGL image export failed");
        REQUIRE(source_rgba[(2 * 4 + 0) * 4 + 2] == 255 &&
                source_rgba[(2 * 4 + 0) * 4 + 3] >= 127 &&
                source_rgba[(2 * 4 + 0) * 4 + 3] <= 128 &&
                source_rgba[(2 * 4 + 3) * 4 + 0] == 255 &&
                source_rgba[(2 * 4 + 3) * 4 + 2] == 255 &&
                source_rgba[(2 * 4 + 3) * 4 + 3] == 0,
                "Pre-transfer readback changed source colors or alpha");
        REQUIRE(eglGetCurrentContext() == producer && eglGetCurrentSurface(EGL_DRAW) == producer_surface &&
                eglGetCurrentSurface(EGL_READ) == producer_surface && glGetError() == GL_INVALID_ENUM,
                "Producer context or error state changed");
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        REQUIRE((GLuint)binding == app_texture, "Producer texture binding changed");
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &binding);
        REQUIRE((GLuint)binding == app_fbo, "Producer framebuffer binding changed");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &app_fbo);
        glDeleteTextures(1, &app_texture);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        REQUIRE(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) &&
                !aurora_egl_frame_import(&image), "Import accepted an absent consumer context");
        REQUIRE(eglMakeCurrent(display, consumer_surface, consumer_surface, consumer), "Consumer context unavailable");
        REQUIRE(has_extension((const char *)glGetString(GL_EXTENSIONS), "GL_OES_EGL_image"),
                "GL_OES_EGL_image unavailable");
        target = aurora_egl_frame_import(&image);
        REQUIRE(target != 0, "EGL image import failed");
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        REQUIRE(glGetError() == GL_NO_ERROR && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                "EGL image import failed");
        /* Verification only: the image transfer itself does not read pixels back. */
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        glReadPixels(0, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);
        glReadPixels(3, 3, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, clear);
        REQUIRE(glGetError() == GL_NO_ERROR && pixel[0] == (green ? 0 : 255) &&
                pixel[1] == (green ? 255 : 0) && pixel[2] == 0 && pixel[3] == 255 &&
                top[0] == 0 && top[1] == 0 && top[2] == 255 &&
                (opaque ? top[3] == 255 : (top[3] >= 127 && top[3] <= 128)) &&
                clear[0] == 255 && clear[1] == 0 && clear[2] == 255 &&
                clear[3] == (opaque ? 255 : 0),
                "Imported frame has incorrect pixels");
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &target);
        aurora_egl_frame_release(&image);
        REQUIRE(!aurora_egl_frame_import(&image), "Import accepted a released image");
    }
    REQUIRE(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) &&
            eglDestroySurface(display, producer_surface) && eglDestroySurface(display, consumer_surface) &&
            eglDestroyContext(display, producer) && eglDestroyContext(display, consumer) && eglTerminate(display),
            "EGL cleanup failed");
    puts("EGL image transfer between isolated contexts passed");
    return 0;
}
