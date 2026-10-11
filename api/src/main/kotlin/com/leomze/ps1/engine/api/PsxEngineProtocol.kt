// SPDX-License-Identifier: MIT
package com.leomze.ps1.engine.api

/** Общие константы протокола приложения и движка. */
object PsxEngineProtocol {
    /** Меняется вместе с IPsxEngine.aidl и psx_engine_shm.h. */
    const val VERSION = 1

    /** Сервис движка; приложение привязывается к нему по имени класса. */
    const val SERVICE_CLASS = "com.leomze.ps1.engine.EngineService"

    /** Ретро-движок (Денди и др., ядра libretro) — свой процесс. */
    const val SERVICE_CLASS_RETRO = "com.leomze.ps1.engine.RetroEngineService"
}
