#include "../core/src/drivers/mg_java_gl.h"
#include "../core/src/mg_core.h"
#include "../core/src/ndk/mg_egl.h"
#include "../core/src/ndk/mg_ndk.h"
#include "../core/src/run/mg_run.h"

#include <stdio.h>
#include <string.h>

static unsigned char frame[64];
static int frames;
static int egl_frames, egl_result;

static int MEGLER_CB present_egl(void *user, void *display, void *surface, int width, int height)
{
    (void)user;
    if (!display || !surface || width != 4 || height != 4) return -1;
    egl_frames++;
    return egl_result;
}

static void MEGLER_CB present(void *user, const void *pixels, int width, int height, int stride)
{
    (void)user;
    if (width != 4 || height != 4 || stride != 16) return;
    memcpy(frame, pixels, sizeof(frame));
    frames++;
}

int main(void)
{
    mg_core_init();
    void *(*get_display)(void *) = (void *(*)(void *))mg_egl_host("eglGetDisplay");
    unsigned (*choose)(void *, const int *, void **, int, int *) =
        (unsigned (*)(void *, const int *, void **, int, int *))mg_egl_host("eglChooseConfig");
    void *(*create_context)(void *, void *, void *, const int *) =
        (void *(*)(void *, void *, void *, const int *))mg_egl_host("eglCreateContext");
    void *(*create_surface)(void *, void *, const int *) =
        (void *(*)(void *, void *, const int *))mg_egl_host("eglCreatePbufferSurface");
    void *(*create_window)(void *, void *, mg_nwindow *, const int *) =
        (void *(*)(void *, void *, mg_nwindow *, const int *))mg_egl_host("eglCreateWindowSurface");
    unsigned (*swap)(void *, void *) = (unsigned (*)(void *, void *))mg_egl_host("eglSwapBuffers");
    int (*get_error)(void) = (int (*)(void))mg_egl_host("eglGetError");
    unsigned (*make_current)(void *, void *, void *, void *) =
        (unsigned (*)(void *, void *, void *, void *))mg_egl_host("eglMakeCurrent");
    unsigned (*destroy_context)(void *, void *) = (unsigned (*)(void *, void *))mg_egl_host("eglDestroyContext");
    unsigned (*destroy_surface)(void *, void *) = (unsigned (*)(void *, void *))mg_egl_host("eglDestroySurface");
    void (*clear_color)(float, float, float, float) = (void (*)(float, float, float, float))mg_egl_host("glClearColor");
    void (*clear)(unsigned) = (void (*)(unsigned))mg_egl_host("glClear");
    void (*scissor)(int, int, int, int) = (void (*)(int, int, int, int))mg_egl_host("glScissor");
    void (*enable)(unsigned) = (void (*)(unsigned))mg_egl_host("glEnable");
    static const int config_attrs[] = { 0x3033, 1, 0x3040, 4, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038 };
    static const int multisample_attrs[] = { 0x3033, 1, 0x3040, 4, 0x3032, 1, 0x3031, 4, 0x3038 };
    static const int surface_attrs[] = { 0x3057, 4, 0x3056, 4, 0x3038 };
    static const int context_attrs[] = { 0x3098, 2, 0x3038 };
    void *display, *config = NULL, *context, *surface, *other, *multisample = NULL;
    mg_nwindow *window;
    /* The test owns the otherwise unconfigured run state for its entire process. */
    megler_run_config *run = (megler_run_config *)mg_run_config();
    int count = 0;
    if (!get_display || !choose || !create_context || !create_surface || !create_window || !swap || !get_error || !make_current || !destroy_context || !destroy_surface ||
        !clear_color || !clear || !scissor || !enable) {
        fprintf(stderr, "EGL/GLES entry point unavailable\n");
        return 1;
    }
    display = get_display(NULL);
    if (!display || !choose(display, config_attrs, &config, 1, &count) || !count) {
        fprintf(stderr, "System EGL display or pbuffer configuration unavailable\n");
        return 1;
    }
    context = create_context(display, config, NULL, context_attrs);
    surface = create_surface(display, config, surface_attrs);
    if (!context || !surface || !make_current(display, surface, surface, context)) {
        fprintf(stderr, "System EGL context or pbuffer unavailable\n");
        return 1;
    }
    clear_color(1, 0, 0, 1);
    clear(0x4000);
    enable(0x0C11);
    scissor(0, 2, 4, 2);
    clear_color(0, 0, 1, 1);
    clear(0x4000);
    if (!mg_java_gl_present_native(display, surface, 4, 4, present, NULL) || frames != 1 ||
        frame[0] != 255 || frame[1] != 0 || frame[2] != 0 ||
        frame[48] != 0 || frame[49] != 0 || frame[50] != 255) {
        fprintf(stderr, "System EGL frame readback or presentation failed\n");
        return 1;
    }
    other = create_surface(display, config, surface_attrs);
    if (!other || !make_current(display, surface, other, context) ||
        mg_java_gl_present_native(display, surface, 4, 4, present, NULL) || frames != 1) {
        fprintf(stderr, "System EGL accepted a frame from a different read surface\n");
        return 1;
    }
    make_current(display, NULL, NULL, NULL);
    destroy_surface(display, other);
    destroy_surface(display, surface);
    window = mg_nwindow_create(4, 4);
    run->window.present_pixels = present;
    if (choose(display, multisample_attrs, &multisample, 1, &count) && count > 0) {
        if (create_window(display, multisample, window, NULL) || get_error() != 0x3009) {
            fprintf(stderr, "System EGL accepted a multisampled native window\n");
            return 1;
        }
    }
    surface = create_window(display, config, window, NULL);
    if (!surface || !make_current(display, surface, surface, context)) {
        fprintf(stderr, "System EGL native window surface unavailable\n");
        return 1;
    }
    clear_color(0, 1, 0, 1);
    clear(0x4000);
    if (!swap(display, surface) || frames != 2 || frame[1] != 255 || frame[0] != 0 || frame[2] != 0) {
        fprintf(stderr, "System EGL native window swap did not present a frame\n");
        return 1;
    }
    run->present_egl_window = present_egl;
    egl_result = 1;
    if (!swap(display, surface) || egl_frames != 1 || frames != 2) {
        fprintf(stderr, "Accepted GPU frame unexpectedly used CPU fallback\n");
        return 1;
    }
    egl_result = 0;
    if (!swap(display, surface) || egl_frames != 2 || frames != 3) {
        fprintf(stderr, "Rejected GPU frame did not use CPU fallback\n");
        return 1;
    }
    egl_result = -1;
    if (swap(display, surface) || egl_frames != 3 || frames != 3 || get_error() != 0x3006) {
        fprintf(stderr, "Lost GPU context did not fail the swap\n");
        return 1;
    }
    run->present_egl_window = NULL;
    make_current(display, NULL, NULL, NULL);
    destroy_surface(display, surface);
    mg_nwindow_release(window);
    destroy_context(display, context);
    puts("System EGL frame readback passed");
    return 0;
}
