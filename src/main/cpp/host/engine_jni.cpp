// SPDX-License-Identifier: GPL-2.0-or-later
// JNI-мост движка: EngineService (Kotlin) <-> EngineHost.
#include <jni.h>
#include <android/log.h>

#include <cstring>
#include <memory>
#include <string>

#include "EngineHost.h"

#define LOG_TAG "psxengine.jni"
#define log_err(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM *g_vm = nullptr;
static jclass g_nativeClass = nullptr;   // com.leomze.ps1.engine.EngineNative
static jmethodID g_onVibrate = nullptr;  // static void onVibrate(int,int,int)
static jmethodID g_onGameId = nullptr;   // static String onGameId(String,String)

jint JNI_OnLoad(JavaVM *vm, void * /*reserved*/)
{
    g_vm = vm;
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;
    jclass cls = env->FindClass("com/leomze/ps1/engine/EngineNative");
    if (cls == nullptr) {
        log_err("FindClass(EngineNative) failed");
        env->ExceptionClear();
        return JNI_ERR;
    }
    g_nativeClass = static_cast<jclass>(env->NewGlobalRef(cls));
    g_onVibrate = env->GetStaticMethodID(cls, "onVibrate", "(III)V");
    g_onGameId = env->GetStaticMethodID(cls, "onGameId",
            "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    env->DeleteLocalRef(cls);
    if (g_onVibrate == nullptr || g_onGameId == nullptr) {
        log_err("EngineNative callbacks not found");
        env->ExceptionClear();
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}

static JNIEnv *AttachJniThread(bool *attached)
{
    JNIEnv *env = nullptr;
    *attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return nullptr;
        *attached = true;
    }
    return env;
}

// из эму-потока ядра (plat_trigger_vibrate)
void psxengine_jni_vibrate(int pad, int low, int high)
{
    bool attached;
    JNIEnv *env = AttachJniThread(&attached);
    if (env == nullptr)
        return;
    env->CallStaticVoidMethod(g_nativeClass, g_onVibrate, pad, low, high);
    if (env->ExceptionCheck())
        env->ExceptionClear();
    if (attached)
        g_vm->DetachCurrentThread();
}

// из emu_boot (после CheckCdrom): путь карты памяти этой игры
char *psxengine_jni_memcard_path(const char *id, const char *label)
{
    bool attached;
    JNIEnv *env = AttachJniThread(&attached);
    if (env == nullptr)
        return nullptr;
    jstring jid = id != nullptr ? env->NewStringUTF(id) : nullptr;
    jstring jlabel = label != nullptr ? env->NewStringUTF(label) : nullptr;
    auto jpath = static_cast<jstring>(
        env->CallStaticObjectMethod(g_nativeClass, g_onGameId, jid, jlabel));
    if (env->ExceptionCheck())
        env->ExceptionClear();
    char *result = nullptr;
    if (jpath != nullptr) {
        const char *path = env->GetStringUTFChars(jpath, nullptr);
        if (path != nullptr) {
            result = strdup(path);
            env->ReleaseStringUTFChars(jpath, path);
        }
        env->DeleteLocalRef(jpath);
    }
    if (jid != nullptr) env->DeleteLocalRef(jid);
    if (jlabel != nullptr) env->DeleteLocalRef(jlabel);
    if (attached)
        g_vm->DetachCurrentThread();
    return result;
}

extern "C" int emu_saf_register_fd(int fd);

static EngineHost *Host(jlong handle)
{
    return reinterpret_cast<EngineHost *>(handle);
}

static std::string ToString(JNIEnv *env, jstring s)
{
    if (s == nullptr)
        return {};
    const char *c = env->GetStringUTFChars(s, nullptr);
    std::string out(c != nullptr ? c : "");
    if (c != nullptr)
        env->ReleaseStringUTFChars(s, c);
    return out;
}

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_leomze_ps1_engine_EngineNative_##name

JNI_FN(jlong, nativeCreate)(JNIEnv *env, jobject, jstring workDir)
{
    auto host = EngineHost::Create(ToString(env, workDir));
    return host != nullptr ? reinterpret_cast<jlong>(host.release()) : 0;
}

JNI_FN(void, nativeDestroy)(JNIEnv *, jobject, jlong handle)
{
    delete Host(handle);
}

JNI_FN(jint, nativeSharedMemoryFd)(JNIEnv *, jobject, jlong handle)
{
    return Host(handle)->SharedMemoryFd();
}

JNI_FN(jint, nativeFrameEventFd)(JNIEnv *, jobject, jlong handle)
{
    return Host(handle)->FrameEventFd();
}

// fd файла из SAF для моста fopen (см. emu/emu_saf.c); native делает dup
JNI_FN(jint, nativeRegisterFile)(JNIEnv *, jobject, jint fd)
{
    return emu_saf_register_fd(fd);
}

JNI_FN(jboolean, nativeBoot)(JNIEnv *env, jobject, jlong handle, jstring disc)
{
    return Host(handle)->Boot(ToString(env, disc)) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(void, nativeStart)(JNIEnv *, jobject, jlong handle) { Host(handle)->Start(); }
JNI_FN(void, nativePause)(JNIEnv *, jobject, jlong handle) { Host(handle)->Pause(); }
JNI_FN(void, nativeResume)(JNIEnv *, jobject, jlong handle) { Host(handle)->Resume(); }

JNI_FN(void, nativeSetSpeed)(JNIEnv *, jobject, jlong handle, jint pct)
{
    Host(handle)->SetSpeedPercent(pct);
}

JNI_FN(jboolean, nativeSaveState)(JNIEnv *env, jobject, jlong handle, jstring path)
{
    return Host(handle)->SaveState(ToString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jboolean, nativeLoadState)(JNIEnv *env, jobject, jlong handle, jstring path)
{
    return Host(handle)->LoadState(ToString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jboolean, nativeSwapDisc)(JNIEnv *env, jobject, jlong handle, jstring path, jboolean lid)
{
    return Host(handle)->SwapDisc(ToString(env, path), lid == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}
