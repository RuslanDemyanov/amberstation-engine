// SPDX-License-Identifier: MIT
package com.leomze.ps1.engine

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.os.Process
import android.util.Log
import com.leomze.ps1.engine.api.IPsxEngine
import com.leomze.ps1.engine.api.IPsxEngineCallback
import com.leomze.ps1.engine.api.PsxEngineProtocol
import java.io.File
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

/**
 * Ретро-движок (Денди и др.) в своём процессе `:retro`: наш libretro-хост и
 * ядро, выбранное по расширению игры. Протокол тот же, что у PS1 ([IPsxEngine]
 * + общая память: картинка, звук, кнопки). Смены диска и опознания игры нет —
 * ключ сейвов задаёт приложение.
 */
class RetroEngineService : Service() {

    private var handle = 0L
    private val lock = ReentrantReadWriteLock()

    private val binder = object : IPsxEngine.Stub() {
        override fun protocolVersion(): Int = PsxEngineProtocol.VERSION

        override fun create(workDir: String, callback: IPsxEngineCallback): Boolean = lock.write {
            if (handle != 0L) destroyLocked()
            callback.asBinder().linkToDeath({
                Log.w(TAG, "приложение завершилось — закрываем движок")
                Process.killProcess(Process.myPid())
            }, 0)
            // данные ядер (батарейные сейвы картриджей) — в своём каталоге
            val dir = File(workDir, "retro").apply { mkdirs() }
            handle = RetroEngineNative.nativeCreate(dir.absolutePath)
            handle != 0L
        }

        override fun sharedMemory(): ParcelFileDescriptor? = session { h ->
            ParcelFileDescriptor.fromFd(RetroEngineNative.nativeSharedMemoryFd(h))
        }

        override fun frameEvent(): ParcelFileDescriptor? = session { h ->
            ParcelFileDescriptor.fromFd(RetroEngineNative.nativeFrameEventFd(h))
        }

        override fun registerFile(fd: ParcelFileDescriptor): Int =
            fd.use { pfd -> session { RetroEngineNative.nativeRegisterFile(it, pfd.fd) } ?: -1 }

        override fun boot(discPath: String?): Boolean =
            session { RetroEngineNative.nativeBoot(it, discPath) } ?: false

        override fun start() { session { RetroEngineNative.nativeStart(it) } }
        override fun pause() { session { RetroEngineNative.nativePause(it) } }
        override fun resume() { session { RetroEngineNative.nativeResume(it) } }
        override fun setSpeed(percent: Int) { session { RetroEngineNative.nativeSetSpeed(it, percent) } }

        override fun saveState(path: String): Boolean =
            session { RetroEngineNative.nativeSaveState(it, path) } ?: false

        override fun loadState(path: String): Boolean =
            session { RetroEngineNative.nativeLoadState(it, path) } ?: false

        override fun swapDisc(path: String, lid: Boolean): Boolean = false

        override fun destroy() = lock.write { destroyLocked() }
    }

    private inline fun <T> session(block: (Long) -> T): T? = lock.read {
        val h = handle
        if (h == 0L) null else block(h)
    }

    private fun destroyLocked() {
        if (handle != 0L) {
            RetroEngineNative.nativeDestroy(handle)
            handle = 0L
        }
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onUnbind(intent: Intent?): Boolean {
        lock.write { destroyLocked() }
        return false
    }

    private companion object {
        const val TAG = "retroengine"
    }
}
