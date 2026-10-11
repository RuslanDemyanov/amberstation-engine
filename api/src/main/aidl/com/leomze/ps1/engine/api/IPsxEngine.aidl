// SPDX-License-Identifier: MIT
package com.leomze.ps1.engine.api;

import com.leomze.ps1.engine.api.IPsxEngineCallback;

/**
 * Команды движку эмуляции. Картинка, звук, кнопки и статус ходят не через
 * эти вызовы, а через общую память (раскладка — include/psx_engine_shm.h).
 */
interface IPsxEngine {
    /** Версия протокола движка, см. PsxEngineProtocol.VERSION. */
    int protocolVersion();

    /**
     * Новая сессия: workDir — каталог данных (bios/, memcards/).
     * Возвращает false, если ядро не запустилось.
     */
    boolean create(String workDir, IPsxEngineCallback callback);

    /** Общая память сессии (ASharedMemory), отображать на чтение и запись. */
    ParcelFileDescriptor sharedMemory();

    /** eventfd: движок пишет в него после каждого кадра. */
    ParcelFileDescriptor frameEvent();

    /**
     * Файл, недоступный по пути (SAF): движок оставляет себе копию
     * дескриптора. Возвращает номер n — путь "saf:<n>.<ext>" или
     * ".__saf<n>.<ext>" внутри cue; -1 — ошибка.
     */
    int registerFile(in ParcelFileDescriptor fd);

    /** Загрузка: путь к образу диска, null — консоль без диска. */
    boolean boot(String discPath);

    void start();
    void pause();
    void resume();

    /** Скорость эмуляции, 100..800 %. */
    void setSpeed(int percent);

    boolean saveState(String path);
    boolean loadState(String path);

    /** Сменить диск; lid — «открыть и закрыть крышку», чтобы игра заметила. */
    boolean swapDisc(String path, boolean lid);

    /** Закрыть сессию: остановить ядро, освободить память и файлы. */
    void destroy();
}
