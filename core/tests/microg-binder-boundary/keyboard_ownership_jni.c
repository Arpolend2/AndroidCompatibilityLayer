#include <jni.h>

static jlong channel, generation;
static jint hide_count, show_count;

JNIEXPORT void JNICALL Java_megler_SharedMicrogKeyboardOwnershipTest_configure(
    JNIEnv *env, jclass cls, jlong ch, jlong gen)
{
    (void)env; (void)cls;
    channel = ch;
    generation = gen;
}

JNIEXPORT jint JNICALL Java_megler_SharedMicrogKeyboardOwnershipTest_hides(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return hide_count;
}

JNIEXPORT jint JNICALL Java_megler_SharedMicrogKeyboardOwnershipTest_shows(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return show_count;
}

JNIEXPORT jlong JNICALL Java_megler_SharedMicrogWorker_currentUiChannel(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return channel;
}

JNIEXPORT jlong JNICALL Java_megler_SharedMicrogWorker_currentUiGeneration(
    JNIEnv *env, jclass cls, jlong ch)
{
    (void)env; (void)cls;
    return ch == channel ? generation : 0;
}

JNIEXPORT jboolean JNICALL Java_megler_SharedMicrogWorker_cancelPendingUi(
    JNIEnv *env, jclass cls, jlong ch, jlong gen)
{
    (void)env; (void)cls; (void)ch; (void)gen;
    return JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_megler_SharedMicrogWorker_finishUi(
    JNIEnv *env, jclass cls, jlong ch, jlong gen)
{
    (void)env; (void)cls;
    if (ch != channel || gen != generation) return JNI_FALSE;
    channel = generation = 0;
    return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_megler_SharedMicrogWorker_keyboardHeight(
    JNIEnv *env, jclass cls, jlong ch, jlong gen, jint height)
{
    (void)env; (void)cls; (void)ch; (void)gen; (void)height;
}

JNIEXPORT void JNICALL Java_megler_Native_nativeLooperPrepare(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
}

JNIEXPORT void JNICALL Java_megler_Native_log(
    JNIEnv *env, jclass cls, jint priority, jstring tag, jstring message)
{
    (void)env; (void)cls; (void)priority; (void)tag; (void)message;
}

JNIEXPORT void JNICALL Java_megler_Native_setOrientation(JNIEnv *env, jclass cls, jint orientation)
{
    (void)env; (void)cls; (void)orientation;
}

JNIEXPORT void JNICALL Java_megler_Native_showKeyboard(
    JNIEnv *env, jclass cls, jboolean show, jint type, jint ime)
{
    (void)env; (void)cls; (void)type; (void)ime;
    if (show) ++show_count;
    else ++hide_count;
}
