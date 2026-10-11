// SPDX-License-Identifier: GPL-2.0-or-later
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
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write

/**
 * Движок эмуляции в отдельном процессе (`:engine`). Приложение привязывается
 * к сервису и управляет сессией через [IPsxEngine]; картинка, звук и кнопки
 * идут через общую память.
 */
class EngineService : Service() {

    private var handle = 0L
    // команды идут параллельно (пауза во время загрузки и т.п.),
    // создание и закрытие сессии — исключительно
    private val lock = ReentrantReadWriteLock()

    private val binder = object : IPsxEngine.Stub() {
        override fun protocolVersion(): Int = PsxEngineProtocol.VERSION

        override fun create(workDir: String, callback: IPsxEngineCallback): Boolean = lock.write {
            if (handle != 0L) destroyLocked()
            // приложение умерло — сессия без него не нужна, процесс тоже
            callback.asBinder().linkToDeath({
                Log.w(TAG, "приложение завершилось — закрываем движок")
                Process.killProcess(Process.myPid())
            }, 0)
            EngineNative.callback = callback
            handle = EngineNative.nativeCreate(workDir)
            handle != 0L
        }

        override fun sharedMemory(): ParcelFileDescriptor? = session { h ->
            ParcelFileDescriptor.fromFd(EngineNative.nativeSharedMemoryFd(h))
        }

        override fun frameEvent(): ParcelFileDescriptor? = session { h ->
            ParcelFileDescriptor.fromFd(EngineNative.nativeFrameEventFd(h))
        }

        override fun registerFile(fd: ParcelFileDescriptor): Int =
            fd.use { EngineNative.nativeRegisterFile(it.fd) }

        override fun boot(discPath: String?): Boolean =
            session { EngineNative.nativeBoot(it, discPath) } ?: false

        override fun start() { session { EngineNative.nativeStart(it) } }
        override fun pause() { session { EngineNative.nativePause(it) } }
        override fun resume() { session { EngineNative.nativeResume(it) } }
        override fun setSpeed(percent: Int) { session { EngineNative.nativeSetSpeed(it, percent) } }

        override fun saveState(path: String): Boolean =
            session { EngineNative.nativeSaveState(it, path) } ?: false

        override fun loadState(path: String): Boolean =
            session { EngineNative.nativeLoadState(it, path) } ?: false

        override fun swapDisc(path: String, lid: Boolean): Boolean =
            session { EngineNative.nativeSwapDisc(it, path, lid) } ?: false

        override fun destroy() = lock.write { destroyLocked() }
    }

    private inline fun <T> session(block: (Long) -> T): T? = lock.read {
        val h = handle
        if (h == 0L) null else block(h)
    }

    private fun destroyLocked() {
        if (handle != 0L) {
            EngineNative.nativeDestroy(handle)
            handle = 0L
        }
        EngineNative.callback = null
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onUnbind(intent: Intent?): Boolean {
        // приложение отвязалось, не закрыв сессию, — закрываем сами
        lock.write { destroyLocked() }
        return false
    }

    private companion object {
        const val TAG = "psxengine"
    }
}
