// SPDX-License-Identifier: MIT
// JNI-мост ретро-движка: RetroEngineService (Kotlin) <-> RetroEngineHost.
#include <jni.h>

#include <string>

#include "RetroEngineHost.h"

static RetroEngineHost *Host(jlong handle)
{
    return reinterpret_cast<RetroEngineHost *>(handle);
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

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_com_leomze_ps1_engine_RetroEngineNative_##name

JNI_FN(jlong, nativeCreate)(JNIEnv *env, jobject, jstring workDir)
{
    auto host = RetroEngineHost::Create(ToString(env, workDir));
    return host != nullptr ? reinterpret_cast<jlong>(host.release()) : 0;
}

JNI_FN(void, nativeDestroy)(JNIEnv *, jobject, jlong handle) { delete Host(handle); }
JNI_FN(jint, nativeSharedMemoryFd)(JNIEnv *, jobject, jlong handle) { return Host(handle)->SharedMemoryFd(); }
JNI_FN(jint, nativeFrameEventFd)(JNIEnv *, jobject, jlong handle) { return Host(handle)->FrameEventFd(); }
JNI_FN(jint, nativeRegisterFile)(JNIEnv *, jobject, jlong handle, jint fd) { return Host(handle)->RegisterFile(fd); }

JNI_FN(jboolean, nativeBoot)(JNIEnv *env, jobject, jlong handle, jstring game)
{
    return Host(handle)->Boot(ToString(env, game)) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(void, nativeStart)(JNIEnv *, jobject, jlong handle) { Host(handle)->Start(); }
JNI_FN(void, nativePause)(JNIEnv *, jobject, jlong handle) { Host(handle)->Pause(); }
JNI_FN(void, nativeResume)(JNIEnv *, jobject, jlong handle) { Host(handle)->Resume(); }
JNI_FN(void, nativeSetSpeed)(JNIEnv *, jobject, jlong handle, jint pct) { Host(handle)->SetSpeedPercent(pct); }

JNI_FN(jboolean, nativeSaveState)(JNIEnv *env, jobject, jlong handle, jstring path)
{
    return Host(handle)->SaveState(ToString(env, path)) ? JNI_TRUE : JNI_FALSE;
}

JNI_FN(jboolean, nativeLoadState)(JNIEnv *env, jobject, jlong handle, jstring path)
{
    return Host(handle)->LoadState(ToString(env, path)) ? JNI_TRUE : JNI_FALSE;
}
