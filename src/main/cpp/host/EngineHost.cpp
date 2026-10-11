// SPDX-License-Identifier: GPL-2.0-or-later
#include "EngineHost.h"

#include <android/log.h>
#include <android/sharedmem.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define LOG_TAG "psxengine"
#define log_info(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define log_err(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

EngineHost *EngineHost::s_active = nullptr;

extern "C" double emu_get_frame_time(void);

// из engine_jni.cpp
extern char *psxengine_jni_memcard_path(const char *id, const char *label);
extern void psxengine_jni_vibrate(int pad, int low, int high);

std::unique_ptr<EngineHost> EngineHost::Create(const std::string &workDir)
{
    if (s_active != nullptr) {
        log_err("session already exists");
        return nullptr;
    }
    auto host = std::unique_ptr<EngineHost>(new EngineHost());
    host->work_dir_ = workDir;
    if (!host->CreateSharedMemory())
        return nullptr;

    struct emu_paths paths = {};
    std::string bios_dir = workDir + "/bios";
    std::string mcd1 = workDir + "/memcards/card1.mcd";
    paths.bios_dir = bios_dir.c_str();
    paths.mcd1 = mcd1.c_str();
    paths.mcd2 = nullptr;

    // кадр забираем сами после каждого кадра ядра (как video_cb в libretro)
    struct emu_callbacks cbs = {
        .video_frame = nullptr,
        .audio_write = EngineHost::OnAudioWrite,
        .vibrate = EngineHost::OnVibrate,
        .game_memcard_path = EngineHost::OnGameMemcardPath,
    };

    if (emu_init(&paths, &cbs) != 0) {
        log_err("emu_init failed");
        return nullptr;
    }
    s_active = host.get();
    return host;
}

bool EngineHost::CreateSharedMemory()
{
    shm_size_ = PSX_SHM_TOTAL_SIZE;
    shm_fd_ = ASharedMemory_create("psxengine", shm_size_);
    if (shm_fd_ < 0) {
        log_err("ASharedMemory_create failed");
        return false;
    }
    void *p = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (p == MAP_FAILED) {
        log_err("mmap shm failed");
        return false;
    }
    shm_ = static_cast<psx_shm *>(p);
    memset(shm_, 0, sizeof(*shm_));
    shm_->total_size = (uint32_t)shm_size_;
    shm_->video_offset = (uint32_t)((sizeof(psx_shm) + 4095) & ~(size_t)4095);
    shm_->audio_offset = shm_->video_offset + PSX_VIDEO_SLOTS * PSX_VIDEO_SLOT_BYTES;
    // буферы кадра: 0 — у движка, 1 — посередине, 2 — у приложения
    video_back_ = 0;
    shm_->video_middle = 1;
    shm_->speed_percent = 100;
    const uint32_t center = PSX_AXIS_CENTER * 0x01010101u;
    for (int i = 0; i < PSX_PADS; i++) {
        shm_->pad_axes[i] = center;
        applied_axes_[i] = center;
    }
    shm_->version = PSX_SHM_VERSION;
    psx_shm_store(&shm_->magic, PSX_SHM_MAGIC);

    event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_fd_ < 0) {
        log_err("eventfd failed");
        return false;
    }
    return true;
}

EngineHost::~EngineHost()
{
    emu_quit_ = true;
    if (emu_thread_.joinable())
        emu_thread_.join();
    if (s_active == this) {
        emu_shutdown();
        s_active = nullptr;
    }
    if (shm_ != nullptr)
        munmap(shm_, shm_size_);
    if (shm_fd_ >= 0)
        close(shm_fd_);
    if (event_fd_ >= 0)
        close(event_fd_);
    log_info("session destroyed");
}

bool EngineHost::Boot(const std::string &disc)
{
    if (booted_)
        return false;
    disc_ = disc;
    emu_set_disc(disc_.empty() ? nullptr : disc_.c_str());
    if (emu_boot() != 0) {
        log_err("emu_boot failed");
        return false;
    }
    booted_ = true;
    log_info("boot ok (disc=%s)", disc_.empty() ? "<none>" : disc_.c_str());
    return true;
}

void EngineHost::Start()
{
    if (!booted_ || emu_thread_.joinable())
        return;
    emu_quit_ = false;
    paused_ = false;
    emu_thread_running_ = true;
    emu_thread_ = std::thread(&EngineHost::EmuLoop, this);
    log_info("emulation started");
}

void EngineHost::Pause()
{
    paused_ = true;
}

void EngineHost::Resume()
{
    if (booted_)
        paused_ = false;
}

void EngineHost::SetSpeedPercent(int pct)
{
    if (pct < 100) pct = 100;
    if (pct > 800) pct = 800;
    speed_pct_ = pct;
    psx_shm_store(&shm_->speed_percent, (uint32_t)pct);
}

// ---------------------------------------------------------------- suspend

void EngineHost::SuspendEmu()
{
    if (!emu_thread_running_.load()) // эму-поток не работает — ядро свободно
        return;
    suspend_requested_ = true;
    while (!suspended_.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void EngineHost::ResumeEmu()
{
    {
        std::lock_guard<std::mutex> lk(suspend_mtx_);
        suspend_requested_ = false;
    }
    suspend_cv_.notify_all();
}

bool EngineHost::SaveState(const std::string &path)
{
    std::vector<uint8_t> buf(EMU_STATE_SIZE);
    SuspendEmu();
    int r = emu_state_save(buf.data(), buf.size());
    ResumeEmu();
    if (r != 0)
        return false;
    FILE *f = fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    size_t n = fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    return n == buf.size();
}

bool EngineHost::LoadState(const std::string &path)
{
    std::vector<uint8_t> buf(EMU_STATE_SIZE);
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    size_t n = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (n != buf.size())
        return false;
    SuspendEmu();
    int r = emu_state_load(buf.data(), buf.size());
    ResumeEmu();
    return r == 0;
}

bool EngineHost::SwapDisc(const std::string &path, bool lid)
{
    SuspendEmu();
    disc_ = path;
    int r = emu_swap_disc(disc_.c_str(), lid ? 1 : 0);
    ResumeEmu();
    log_info("disc swap -> %s: %s", path.c_str(), r == 0 ? "ok" : "fail");
    return r == 0;
}

// ---------------------------------------------------------------- эму-поток

void EngineHost::EmuLoop()
{
    using clock = std::chrono::steady_clock;
    auto next_frame = clock::now();

    while (!emu_quit_) {
        // запрос на безопасный доступ к ядру (стейты, смена диска)
        if (suspend_requested_.load()) {
            std::unique_lock<std::mutex> lk(suspend_mtx_);
            suspended_ = true;
            suspend_cv_.notify_all();
            suspend_cv_.wait(lk, [this] { return !suspend_requested_.load(); });
            suspended_ = false;
            next_frame = clock::now();
        }

        if (paused_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            next_frame = clock::now();
            continue;
        }

        ApplyInput();
        auto t0 = clock::now();
        emu_run_frame();
        auto t1 = clock::now();

        // последний кадр GPU отдаём каждый раз: статичные сцены (BIOS-меню)
        // не генерируют флипов сами
        PublishFrame();
        PublishStatus(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0));

        // покадровый лимит: абсолютное расписание без дрейфа
        double frame_s = emu_get_frame_time() * 100.0 / speed_pct_.load();
        next_frame += std::chrono::nanoseconds((long long)(frame_s * 1e9));
        if (next_frame > t1) {
            std::this_thread::sleep_until(next_frame);
        } else {
            // отстаём (медленное устройство) — перезапускаем расписание
            next_frame = t1;
        }
    }
    emu_thread_running_ = false;
    log_info("emu loop exited");
}

// кнопки и оси из общей памяти — раз в кадр, перед исполнением (как input_poll в libretro)
void EngineHost::ApplyInput()
{
    for (int pad = 0; pad < PSX_PADS; pad++) {
        uint32_t buttons = psx_shm_load(&shm_->pad_buttons[pad]);
        uint32_t changed = buttons ^ applied_buttons_[pad];
        for (int dkey = 0; changed != 0 && dkey < 16; dkey++) {
            if (changed & (1u << dkey))
                emu_set_key(pad, dkey, (buttons >> dkey) & 1);
        }
        applied_buttons_[pad] = buttons;

        uint32_t axes = psx_shm_load(&shm_->pad_axes[pad]);
        if (axes != applied_axes_[pad]) {
            for (int axis = 0; axis < 4; axis++)
                emu_set_analog(pad, axis, (int)((axes >> (axis * 8)) & 0xff));
            applied_axes_[pad] = axes;
        }
    }
}

void EngineHost::PublishFrame()
{
    const uint16_t *px;
    int w, h, pitch;
    if (!emu_get_video_frame(&px, &w, &h, &pitch) || px == nullptr)
        return;
    if (w > PSX_VIDEO_MAX_W) w = PSX_VIDEO_MAX_W;
    if (h > PSX_VIDEO_MAX_H) h = PSX_VIDEO_MAX_H;

    uint16_t *dst = psx_shm_video_slot(shm_, video_back_);
    if (pitch == w * 2) {
        memcpy(dst, px, (size_t)w * h * 2);
    } else {
        for (int y = 0; y < h; y++)
            memcpy(dst + (size_t)y * w, (const uint8_t *)px + (size_t)y * pitch, (size_t)w * 2);
    }
    shm_->video_meta[video_back_].w = (uint32_t)w;
    shm_->video_meta[video_back_].h = (uint32_t)h;
    uint32_t prev = psx_shm_exchange(&shm_->video_middle, video_back_ | PSX_VIDEO_FRESH);
    video_back_ = prev & PSX_VIDEO_INDEX;
    psx_shm_store(&shm_->video_w, (uint32_t)w);
    psx_shm_store(&shm_->video_h, (uint32_t)h);

    uint64_t one = 1;
    (void)!write(event_fd_, &one, sizeof(one));
}

void EngineHost::PublishStatus(std::chrono::nanoseconds work)
{
    psx_shm_store(&shm_->cdrom_status, (int32_t)emu_cdr_stat());
    psx_shm_add(&shm_->emu_ns_total, (uint64_t)work.count());
    uint64_t n = psx_shm_add(&shm_->frames_done, (uint64_t)1) + 1;
    if (n % 600 == 0)
        log_info("frames=%llu exec=%.2fms", (unsigned long long)n, work.count() / 1e6);
}

// звук ядра -> кольцо в общей памяти; если приложение не успевает забирать,
// новые фреймы отбрасываются (указатель читателя трогает только читатель)
void EngineHost::WriteAudio(const int16_t *frames, int count)
{
    uint64_t head = __atomic_load_n(&shm_->audio_head, __ATOMIC_RELAXED);
    uint64_t tail = psx_shm_load(&shm_->audio_tail);
    uint64_t space = PSX_AUDIO_FRAMES - (head - tail);
    if ((uint64_t)count > space) {
        psx_shm_add(&shm_->audio_overflows, (uint64_t)1);
        count = (int)space;
    }
    int16_t *ring = psx_shm_audio(shm_);
    for (int i = 0; i < count; i++) {
        size_t idx = (size_t)((head + i) & (PSX_AUDIO_FRAMES - 1));
        ring[idx * 2] = frames[i * 2];
        ring[idx * 2 + 1] = frames[i * 2 + 1];
    }
    psx_shm_store(&shm_->audio_head, head + (uint64_t)count);
}

// ---------------------------------------------------------------- колбэки ядра

void EngineHost::OnAudioWrite(const int16_t *frames, int count)
{
    if (s_active != nullptr && count > 0)
        s_active->WriteAudio(frames, count);
}

void EngineHost::OnVibrate(int pad, int low, int high)
{
    psxengine_jni_vibrate(pad, low, high);
}

char *EngineHost::OnGameMemcardPath(const char *id, const char *label)
{
    return psxengine_jni_memcard_path(id, label);
}
