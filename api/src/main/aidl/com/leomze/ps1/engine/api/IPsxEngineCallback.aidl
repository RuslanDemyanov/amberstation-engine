// SPDX-License-Identifier: MIT
package com.leomze.ps1.engine.api;

/** Обратные вызовы движка в приложение. */
interface IPsxEngineCallback {
    /**
     * Диск опознан (при загрузке, до сброса консоли). Возвращает путь к файлу
     * карты памяти для этой игры или null — карта по умолчанию.
     */
    String onGameIdentified(String discId, String label);

    /** Вибрация от игры: сила моторов 0..255. */
    oneway void onVibrate(int pad, int low, int high);
}
