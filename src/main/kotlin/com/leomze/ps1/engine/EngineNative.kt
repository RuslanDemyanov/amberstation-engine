// SPDX-License-Identifier: GPL-2.0-or-later
package com.leomze.ps1.engine

import android.util.Log
import com.leomze.ps1.engine.api.IPsxEngineCallback

/** JNI-контракт к libpsxengine.so. Только для [EngineService]. */
internal object EngineNative {
    init {
        System.loadLibrary("psxengine")
    }

    /** Колбэк приложения текущей сессии. */
    @Volatile
    var callback: IPsxEngineCallback? = null

    external fun nativeCreate(workDir: String): Long
    external fun nativeDestroy(handle: Long)
    external fun nativeSharedMemoryFd(handle: Long): Int
    external fun nativeFrameEventFd(handle: Long): Int
    external fun nativeRegisterFile(fd: Int): Int
    external fun nativeBoot(handle: Long, disc: String?): Boolean
    external fun nativeStart(handle: Long)
    external fun nativePause(handle: Long)
    external fun nativeResume(handle: Long)
    external fun nativeSetSpeed(handle: Long, percent: Int)
    external fun nativeSaveState(handle: Long, path: String): Boolean
    external fun nativeLoadState(handle: Long, path: String): Boolean
    external fun nativeSwapDisc(handle: Long, path: String, lid: Boolean): Boolean

    /** Из native при загрузке диска: путь к карте памяти игры. */
    @JvmStatic
    fun onGameId(discId: String?, label: String?): String? =
        runCatching { callback?.onGameIdentified(discId, label) }
            .onFailure { Log.w(TAG, "onGameIdentified", it) }
            .getOrNull()

    /** Из эму-потока: вибрация от игры. */
    @JvmStatic
    fun onVibrate(pad: Int, low: Int, high: Int) {
        runCatching { callback?.onVibrate(pad, low, high) }
    }

    private const val TAG = "psxengine"
}
