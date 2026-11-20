#include "../../core/src/mg_core.h"
#include "../../core/src/bionic/mg_bionic.h"
#include "../../core/src/drivers/mg_unity.h"
#include "../../core/src/jni/mg_jni.h"
#include "../../core/src/jni/mg_jni_android.h"
#include "../../core/src/ld/mg_ld.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef void *(*static_id_fn)(void *, mg_jobj *, const char *, const char *);
typedef void (*static_void_fn)(void *, mg_jobj *, void *, const mg_jvalue *);
typedef unsigned char (*call_boolean_fn)(void *, mg_jobj *, void *, const mg_jvalue *);
typedef unsigned char (*exception_check_fn)(void *);

static void *method;
static mg_jobj *system_class;
static int errors;

static int call_load(const char *path)
{
    void *env = mg_jni_env();
    void **table = *(void ***)env;
    mg_jvalue arg;
    int exception;
    memset(&arg, 0, sizeof(arg));
    if (path) arg.l = mg_jni_new_string(path);
    ((static_void_fn)table[143])(env, system_class, method, &arg);
    exception = ((exception_check_fn)table[228])(env);
    mg_jni_exception_clear();
    mg_jni_unref(arg.l);
    return exception;
}

static int call_unity_load(const char *path)
{
    void *env = mg_jni_env();
    void **table = *(void ***)env;
    mg_jobj *cls = mg_jni_class_object("com/unity3d/player/UnityPlayer");
    mg_jobj *player = mg_jni_new_object("com/unity3d/player/UnityPlayer");
    mg_jvalue arg;
    memset(&arg, 0, sizeof(arg));
    void *method = ((static_id_fn)table[33])(env, cls, "loadLibrary", "(Ljava/lang/String;)Z");
    int result;
    if (!method || !player) return 0;
    if (path) arg.l = mg_jni_new_string(path);
    result = ((call_boolean_fn)table[39])(env, player, method, &arg);
    mg_jni_unref(arg.l);
    mg_jni_unref(player);
    return result;
}

typedef struct load_call {
    const char *path;
    int done, exception;
} load_call;

static void *load_thread(void *arg)
{
    load_call *call = (load_call *)arg;
    call->exception = call_load(call->path);
    __atomic_store_n(&call->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "failed: %s\n", #expr); errors++; } } while (0)

int main(int argc, char **argv)
{
    const char *path = "/data/app/test-1/lib/arm/libjni_load_fixture.so";
    const char *bad = "/data/app/test-1/lib/arm/libjni_load_failure.so";
    const char *unity_name = "FirebaseCppApp-13_6_0";
    const char *unity_file = "libFirebaseCppApp-13_6_0.so";
    void *env, **table;
    mg_lib *lib, *failed_lib, *unity_lib;
    int (*entered)(void), (*calls)(void), (*failed_calls)(void);
    int (*unity_calls)(void);
    void *(*vm)(void);
    void *(*unity_vm)(void);
    void (*release)(void);
    void *(*native_dlopen)(const char *, int);
    load_call first = { path, 0, 0 }, second = { path, 0, 0 };
    pthread_t a, b;
    int i;
    if (argc != 2 || !mg_core_init()) return 1;
    mg_ld_add_search_dir(argv[1]);
    mg_jni_android_setup("android/app/Activity", "test", "/nonexistent.apk", 480, 800, 160);
    mg_unity_register_jni();
    env = mg_jni_env();
    table = *(void ***)env;
    system_class = mg_jni_class_object("java/lang/System");
    method = ((static_id_fn)table[113])(env, system_class, "load", "(Ljava/lang/String;)V");
    lib = mg_ld_load("libjni_load_fixture.so");
    failed_lib = mg_ld_load("libjni_load_failure.so");
    if (!method || !lib || !failed_lib || !mg_ld_init(lib) || !mg_ld_init(failed_lib)) return 1;
    entered = (int (*)(void))mg_ld_sym(lib, "fixture_entered");
    calls = (int (*)(void))mg_ld_sym(lib, "fixture_calls");
    vm = (void *(*)(void))mg_ld_sym(lib, "fixture_vm");
    release = (void (*)(void))mg_ld_sym(lib, "fixture_release");
    failed_calls = (int (*)(void))mg_ld_sym(failed_lib, "fixture_calls");
    if (!entered || !calls || !vm || !release || !failed_calls) return 1;
    CHECK(call_load(NULL));
    CHECK(call_load("relative.so"));
    CHECK(call_load("/missing/libnothing.so"));
    if (pthread_create(&a, NULL, load_thread, &first) != 0) return 1;
    for (i = 0; i < 500 && !entered(); i++) usleep(10000);
    CHECK(entered());
    if (pthread_create(&b, NULL, load_thread, &second) != 0) {
        release();
        pthread_join(a, NULL);
        return 1;
    }
    usleep(100000);
    CHECK(!__atomic_load_n(&second.done, __ATOMIC_SEQ_CST));
    release();
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    CHECK(!first.exception && !second.exception);
    CHECK(calls() == 1 && vm() == mg_jni_vm());
    CHECK(!call_load(path) && calls() == 1);
    CHECK(call_load(bad) && call_load(bad) && failed_calls() == 1);
    CHECK(!call_unity_load(NULL));
    CHECK(!call_unity_load("missing_unity_fixture"));
    CHECK(call_unity_load(unity_name));
    unity_lib = mg_ld_find_lib(unity_file);
    if (!unity_lib) return 1;
    unity_calls = (int (*)(void))mg_ld_sym(unity_lib, "fixture_calls");
    unity_vm = (void *(*)(void))mg_ld_sym(unity_lib, "fixture_vm");
    native_dlopen = (void *(*)(const char *, int))mg_bionic_find("dlopen");
    if (!unity_calls || !unity_vm || !native_dlopen) return 1;
    CHECK(unity_calls() == 1 && unity_vm() == mg_jni_vm());
    CHECK(native_dlopen(unity_file, 2) == unity_lib);
    CHECK(unity_calls() == 1 && unity_vm() == mg_jni_vm());
    CHECK(call_unity_load(unity_name) && unity_calls() == 1);
    CHECK(call_unity_load("/data/app/test-1/lib/arm/libFirebaseCppApp-13_6_0.so") && unity_calls() == 1);
    CHECK(call_unity_load(unity_file) && unity_calls() == 1);
    return errors ? 1 : 0;
}
