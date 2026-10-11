// SPDX-License-Identifier: GPL-2.0-or-later
// Хост сессии движка: ядро крутится в своём потоке, а картинка, звук, кнопки
// и статус ходят через общую память (engine-api/include/psx_engine_shm.h).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

extern "C" {
#include "emu_frontend.h"
#include "psx_engine_shm.h"
}

class EngineHost {
public:
    static std::unique_ptr<EngineHost> Create(const std::string &workDir);
    ~EngineHost();

    int SharedMemoryFd() const { return shm_fd_; }
    int FrameEventFd() const { return event_fd_; }

    // disc пустой -> консоль без диска
    bool Boot(const std::string &disc);
    void Start();
    void Pause();
    void Resume();
    void SetSpeedPercent(int pct);
    bool SaveState(const std::string &path);
    bool LoadState(const std::string &path);
    // многодисковые игры: вставить другой образ (lid — с открытием крышки)
    bool SwapDisc(const std::string &path, bool lid);

private:
    EngineHost() = default;
    bool CreateSharedMemory();
    void EmuLoop();
    void ApplyInput();
    void PublishFrame();
    void PublishStatus(std::chrono::nanoseconds work);
    void WriteAudio(const int16_t *frames, int count);
    // безопасный доступ к ядру: эму-поток паркуется между кадрами
    void SuspendEmu();
    void ResumeEmu();

    static void OnAudioWrite(const int16_t *frames, int count);
    static void OnVibrate(int pad, int low, int high);
    static char *OnGameMemcardPath(const char *id, const char *label);

    static EngineHost *s_active; // колбэки из C без контекста

    std::string work_dir_;
    std::string disc_; // ядро держит указатель на строку пути

    // общая память и событие кадра
    int shm_fd_ = -1;
    int event_fd_ = -1;
    psx_shm *shm_ = nullptr;
    size_t shm_size_ = 0;
    uint32_t video_back_ = 0;              // буфер кадра, которым владеет движок
    uint32_t applied_buttons_[PSX_PADS] = {};
    uint32_t applied_axes_[PSX_PADS] = {};

    std::thread emu_thread_;
    std::atomic<bool> emu_quit_{false};
    std::atomic<bool> emu_thread_running_{false};
    std::atomic<bool> paused_{true};
    std::atomic<bool> suspend_requested_{false};
    std::atomic<bool> suspended_{false};
    std::mutex suspend_mtx_;
    std::condition_variable suspend_cv_;
    std::atomic<int> speed_pct_{100};
    bool booted_ = false;
};
