// SPDX-License-Identifier: MIT
package com.leomze.ps1.engine

/** JNI-контракт к libretroengine.so (наш libretro-хост). Только для [RetroEngineService]. */
internal object RetroEngineNative {
    init {
        System.loadLibrary("retroengine")
    }

    external fun nativeCreate(workDir: String): Long
    external fun nativeDestroy(handle: Long)
    external fun nativeSharedMemoryFd(handle: Long): Int
    external fun nativeFrameEventFd(handle: Long): Int
    external fun nativeRegisterFile(handle: Long, fd: Int): Int
    external fun nativeBoot(handle: Long, game: String?): Boolean
    external fun nativeStart(handle: Long)
    external fun nativePause(handle: Long)
    external fun nativeResume(handle: Long)
    external fun nativeSetSpeed(handle: Long, percent: Int)
    external fun nativeSaveState(handle: Long, path: String): Boolean
    external fun nativeLoadState(handle: Long, path: String): Boolean
}
