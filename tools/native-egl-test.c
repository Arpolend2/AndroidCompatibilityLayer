/* Isolated native backend test; compile with -ffunction-sections and --gc-sections. */
#include "../core/src/ndk/mg_egl.c"

#include <assert.h>

static int egl_lib, gles2_lib, gles1_lib, angle_egl, angle_gles, display_token, config_token, context_token;
static int opened, initialized, context_calls, extension_calls;
static bool fail_display, fail_initialize, missing_egl, missing_gles1;

static EGLDisplay fake_get_display(void *native)
{
    assert(!native);
    return fail_display ? NULL : &display_token;
}

static EGLBoolean fake_initialize(EGLDisplay d, EGLint *major, EGLint *minor)
{
    assert(d == &display_token);
    initialized++;
    if (fail_initialize) return 0;
    *major = 1;
    *minor = 5;
    return 1;
}

static EGLContext fake_create_context(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *attrs)
{
    assert(d == &display_token && c == &config_token && !share);
    assert(attrs[0] == EGL_CONTEXT_CLIENT_VERSION && attrs[1] == 3 && attrs[2] == EGL_NONE);
    context_calls++;
    return &context_token;
}

static void fake_gles2(void) {}
static void fake_gles1(void) {}
static void fake_extension(void) {}
static void fake_sync(void) {}

static EGLDisplay fake_platform_display(EGLenum platform, void *native, const EGLint *attrs)
{
    assert(platform == EGL_PLATFORM_ANGLE_ANGLE && !native);
    assert(attrs[0] == EGL_PLATFORM_ANGLE_TYPE_ANGLE && attrs[1] == EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE);
    return &display_token;
}

static void *fake_provider(const char *name)
{
    if (!strcmp(name, "eglGetPlatformDisplayEXT")) return (void *)fake_platform_display;
    if (!strcmp(name, "eglInitialize")) return (void *)fake_initialize;
    return NULL;
}

static void *fake_get_proc(const char *name)
{
    extension_calls++;
    if (!strcmp(name, "glExtensionExample")) return (void *)fake_extension;
    if (!strcmp(name, "eglCreateSyncKHR")) return (void *)fake_sync;
    return NULL;
}

void *mg_dl_open(const char *name)
{
    opened++;
    if (!strcmp(name, "libEGL.so.1")) return missing_egl ? NULL : &egl_lib;
    if (!strcmp(name, "libGLESv2.so.2")) return &gles2_lib;
    if (!strcmp(name, "libGLESv1_CM.so.1")) return missing_gles1 ? NULL : &gles1_lib;
    if (!strcmp(name, "libEGL.dll")) return &angle_egl;
    if (!strcmp(name, "libGLESv2.dll")) return &angle_gles;
    assert(!"unexpected library");
    return NULL;
}

void *mg_dl_sym(void *lib, const char *name)
{
    if (lib == &egl_lib || lib == &angle_egl) {
        if (!strcmp(name, "eglGetDisplay")) return (void *)fake_get_display;
        if (!strcmp(name, "eglInitialize")) return (void *)fake_initialize;
        if (!strcmp(name, "eglCreateContext")) return (void *)fake_create_context;
        if (!strcmp(name, "eglGetProcAddress")) return (void *)fake_get_proc;
    }
    if ((lib == &gles2_lib || lib == &angle_gles) && !strcmp(name, "glGles2Example")) return (void *)fake_gles2;
    if (lib == &gles1_lib && !strcmp(name, "glGles1Example")) return (void *)fake_gles1;
    return NULL;
}

void mg_lock_acquire(mg_lock *l) { pthread_mutex_lock(&l->m); }
void mg_lock_release(mg_lock *l) { pthread_mutex_unlock(&l->m); }
void *mg_alloc(size_t n) { void *p = calloc(1, n); assert(p); return p; }
bool mg_flag(const char *name) { (void)name; return false; }
int mg_host_d3d11_hardware_level(void) { return 0; }
void mg_log(int level, const char *fmt, ...) { (void)level; (void)fmt; }
size_t mg_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) {
        size_t k = n < cap - 1 ? n : cap - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return n;
}

static void reset_backend(void)
{
    g_lib_egl = g_lib_gles = g_lib_gles1 = NULL;
    g_angle_tried = false;
    g_provider = NULL;
    opened = initialized = context_calls = extension_calls = 0;
    fail_display = fail_initialize = missing_egl = missing_gles1 = false;
}

int main(void)
{
    made_display m;
    EGLint attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLConfig candidates[8];
    fn_create_context create;

    reset_backend();
#if defined(MG_EXPERIMENTAL_NATIVE_EGL) && !MG_HOST_WINDOWS
    assert(native_egl());
    assert(angle("eglGetDisplay") == (void *)fake_get_display);
    assert(angle("glGles2Example") == (void *)fake_gles2);
    assert(angle("glGles1Example") == (void *)fake_gles1);
    assert(angle("glExtensionExample") == (void *)fake_extension);
    assert(angle("glNotAvailable") == NULL);
    assert(opened == 3 && extension_calls == 2);
    memset(&m, 0, sizeof(m));
    make_display(&m);
    assert(m.d == &display_token && m.major == 1 && m.minor == 5 && initialized == 1);
    assert(!strcmp(m.how, "system EGL"));
    assert(surface_configs(m.d, &config_token, candidates, 8) == 1 && candidates[0] == &config_token);
    assert(!has_ext(layer_extensions("EGL_KHR_image_base"), "EGL_KHR_fence_sync"));
    assert(native_sync_name("eglCreateSyncKHR"));
    assert(native_sync_name("eglDupNativeFenceFDANDROID"));
    assert(!native_sync_name("eglCreateContext"));
    assert(angle("eglCreateSyncKHR") == (void *)fake_sync);
    create = A(fn_create_context, "eglCreateContext");
    assert(create(m.d, context_config(m.d, &config_token, attrs, true), NULL, attrs) == &context_token);
    assert(context_calls == 1);
    memset(&m, 0, sizeof(m));
    m.whose = "the WebView";
    make_display(&m);
    assert(!m.d && initialized == 1);

    reset_backend();
    missing_egl = true;
    memset(&m, 0, sizeof(m));
    make_display(&m);
    assert(!m.d && initialized == 0);

    reset_backend();
    missing_gles1 = true;
    assert(angle("glGles1Example") == NULL);
    assert(angle("glGles2Example") == (void *)fake_gles2);

    reset_backend();
    fail_display = true;
    memset(&m, 0, sizeof(m));
    make_display(&m);
    assert(!m.d && initialized == 0);

    reset_backend();
    fail_initialize = true;
    memset(&m, 0, sizeof(m));
    make_display(&m);
    assert(!m.d && initialized == 1);

    reset_backend();
#else
    assert(!native_egl());
    assert(angle("eglGetDisplay") == (void *)fake_get_display);
    assert(angle("glGles2Example") == (void *)fake_gles2);
    assert(angle("glExtensionExample") == NULL);
    assert(opened == 2 && extension_calls == 0);
    (void)attrs;
    (void)candidates;
    (void)create;
    reset_backend();
#endif
    mg_egl_set_provider(fake_provider);
    assert(!native_egl());
    assert(has_ext(layer_extensions("EGL_EXT_platform_base"), "EGL_KHR_fence_sync"));
    memset(&m, 0, sizeof(m));
    make_display(&m);
    assert(m.d == &display_token && !strcmp(m.how, "Direct3D 11") && opened == 0);
    return 0;
}
