// SPDX-License-Identifier: MIT
// Сессия ретро-движка (Денди и др.) на libretro-ядре: тот же протокол, что у
// PS1 (общая память psx_engine_shm.h + eventfd кадра). Всё общение с ядром —
// в одном «потоке ядра»; команды Binder-потоков (загрузка, сейвы) ставятся в
// очередь и выполняются между кадрами. Ядро выбирается по расширению игры.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "LibretroHost.h"

extern "C" {
#include "psx_engine_shm.h"
}

class RetroEngineHost {
public:
    static std::unique_ptr<RetroEngineHost> Create(const std::string &workDir);
    ~RetroEngineHost();

    int SharedMemoryFd() const { return shm_fd_; }
    int FrameEventFd() const { return event_fd_; }

    // файл из SAF: копия дескриптора, путь "saf:<n>.<ext>"
    int RegisterFile(int fd);
    bool Boot(const std::string &game);
    void Start();
    void Pause() { paused_ = true; }
    void Resume() { if (loaded_) paused_ = false; }
    void SetSpeedPercent(int pct);
    bool SaveState(const std::string &path);
    bool LoadState(const std::string &path);

private:
    RetroEngineHost() = default;
    bool CreateSharedMemory();
    bool ReadGame(const std::string &game, std::vector<uint8_t> *data);
    void ThreadMain();
    // выполнить на потоке ядра и дождаться результата
    bool Call(std::function<bool()> task);
    void Frame();
    void PublishFrame();
    void WriteAudio(const int16_t *frames, size_t count);
    void WriteRing(const int16_t *frames, size_t count);

    // ресемплинг звука ядра к PSX_AUDIO_RATE
    double resample_pos_ = 0;
    int64_t resample_acc_[2] = {0, 0};
    int resample_cnt_ = 0;
    int16_t resample_prev_[2] = {0, 0};
    std::vector<int16_t> resample_out_;

    std::string work_dir_;
    LibretroHost core_;
    std::vector<int> files_;

    int shm_fd_ = -1;
    int event_fd_ = -1;
    psx_shm *shm_ = nullptr;
    size_t shm_size_ = 0;
    uint32_t video_back_ = 0;
    uint64_t sram_frames_ = 0;

    std::thread thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool quit_ = false;

    std::atomic<bool> loaded_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{true};
    std::atomic<int> speed_pct_{100};
};
