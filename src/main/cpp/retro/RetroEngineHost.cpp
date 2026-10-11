// SPDX-License-Identifier: MIT
#include "RetroEngineHost.h"

#include <android/log.h>
#include <android/sharedmem.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>

#define LOG_TAG "retroengine"
#define log_info(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define log_err(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct CoreSpec {
    const char *library;
    std::map<std::string, std::string> options;
};

// ядро по расширению игры (без точки, в нижнем регистре)
const CoreSpec *CoreFor(const std::string &ext)
{
    static const CoreSpec fceumm{
        "libfceumm_libretro.so",
        {
            {"fceumm_sndrate_hint", "44KHz"}, // звук в общей памяти — 44100 Гц
            // турбо A/B на X/Y RetroPad (кнопки оверлея «Турбо»), 3 кадра
            {"fceumm_turbo_enable", "Player 1"},
            {"fceumm_turbo_delay", "3"},
        },
    };
    // Sega Mega Drive / Genesis
    static const CoreSpec clownmdemu{
        "libclownmdemu_libretro.so",
        {{"clownmdemu_tv_standard", "ntsc"}, {"clownmdemu_overseas_region", "elsewhere"}},
    };
    if (ext == "nes" || ext == "fds" || ext == "unf" || ext == "unif")
        return &fceumm;
    if (ext == "md" || ext == "gen" || ext == "smd" || ext == "68k" || ext == "sgd")
        return &clownmdemu;
    return nullptr;
}

std::string ExtOf(const std::string &path)
{
    size_t dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)tolower(c); });
    return ext;
}

bool ReadAll(int fd, std::vector<uint8_t> *data)
{
    data->clear();
    uint8_t chunk[1 << 16];
    off_t pos = 0;
    for (;;) {
        ssize_t n = pread(fd, chunk, sizeof(chunk), pos);
        if (n < 0)
            return false;
        if (n == 0)
            return true;
        data->insert(data->end(), chunk, chunk + n);
        pos += n;
        if (data->size() > 64u * 1024 * 1024)
            return false; // не ROM старой приставки
    }
}

} // namespace

std::unique_ptr<RetroEngineHost> RetroEngineHost::Create(const std::string &workDir)
{
    auto host = std::unique_ptr<RetroEngineHost>(new RetroEngineHost());
    host->work_dir_ = workDir;
    mkdir((workDir + "/saves").c_str(), 0700);
    mkdir((workDir + "/cache").c_str(), 0700);
    if (!host->CreateSharedMemory())
        return nullptr;
    host->core_.on_audio = [h = host.get()](const int16_t *f, size_t n) { h->WriteAudio(f, n); };
    host->thread_ = std::thread(&RetroEngineHost::ThreadMain, host.get());
    return host;
}

RetroEngineHost::~RetroEngineHost()
{
    if (thread_.joinable()) {
        Call([this] {
            core_.Deinit();
            return true;
        });
        {
            std::lock_guard<std::mutex> lk(mtx_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    for (int fd : files_)
        close(fd);
    if (shm_ != nullptr)
        munmap(shm_, shm_size_);
    if (shm_fd_ >= 0)
        close(shm_fd_);
    if (event_fd_ >= 0)
        close(event_fd_);
    log_info("session destroyed");
}

bool RetroEngineHost::CreateSharedMemory()
{
    shm_size_ = PSX_SHM_TOTAL_SIZE;
    shm_fd_ = ASharedMemory_create("retroengine", shm_size_);
    if (shm_fd_ < 0)
        return false;
    void *p = mmap(nullptr, shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
    if (p == MAP_FAILED)
        return false;
    shm_ = static_cast<psx_shm *>(p);
    memset(shm_, 0, sizeof(*shm_));
    shm_->total_size = (uint32_t)shm_size_;
    shm_->video_offset = (uint32_t)((sizeof(psx_shm) + 4095) & ~(size_t)4095);
    shm_->audio_offset = shm_->video_offset + PSX_VIDEO_SLOTS * PSX_VIDEO_SLOT_BYTES;
    video_back_ = 0;
    shm_->video_middle = 1;
    shm_->speed_percent = 100;
    for (int i = 0; i < PSX_PADS; i++)
        shm_->pad_axes[i] = PSX_AXIS_CENTER * 0x01010101u;
    shm_->version = PSX_SHM_VERSION;
    psx_shm_store(&shm_->magic, PSX_SHM_MAGIC);
    event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    return event_fd_ >= 0;
}

int RetroEngineHost::RegisterFile(int fd)
{
    int copy = dup(fd);
    if (copy < 0)
        return -1;
    files_.push_back(copy);
    return (int)files_.size() - 1;
}

// ---------------------------------------------------------------- команды

bool RetroEngineHost::Call(std::function<bool()> task)
{
    if (std::this_thread::get_id() == thread_.get_id())
        return task();
    auto done = std::make_shared<std::promise<bool>>();
    auto result = done->get_future();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_.push_back([task = std::move(task), done] { done->set_value(task()); });
    }
    cv_.notify_all();
    return result.get();
}

bool RetroEngineHost::ReadGame(const std::string &game, std::vector<uint8_t> *data)
{
    if (game.rfind("saf:", 0) == 0) {
        int idx = atoi(game.c_str() + 4);
        if (idx < 0 || idx >= (int)files_.size())
            return false;
        return ReadAll(files_[idx], data);
    }
    FILE *f = fopen(game.c_str(), "rb");
    if (f == nullptr)
        return false;
    bool ok = ReadAll(fileno(f), data);
    fclose(f);
    return ok;
}

bool RetroEngineHost::Boot(const std::string &game)
{
    if (game.empty() || loaded_)
        return false;
    std::string ext = ExtOf(game);
    const CoreSpec *spec = CoreFor(ext);
    if (spec == nullptr) {
        log_err("no core for .%s", ext.c_str());
        return false;
    }
    std::vector<uint8_t> data;
    if (!ReadGame(game, &data) || data.empty()) {
        log_err("can't read %s", game.c_str());
        return false;
    }
    // путь для ядра: имя и расширение (по нему ядро угадывает формат);
    // сам файл не открывается — ROM передаём из памяти
    std::string name = game.rfind("saf:", 0) == 0 ? work_dir_ + "/game." + ext : game;
    bool ok = Call([&] {
        LibretroHost::Config cfg;
        cfg.core_path = spec->library;
        cfg.system_dir = work_dir_;
        cfg.save_dir = work_dir_ + "/saves";
        cfg.cache_dir = work_dir_ + "/cache";
        cfg.options = spec->options;
        return core_.Init(cfg) && core_.LoadGame(name, std::move(data));
    });
    loaded_ = ok;
    log_info("boot %s: %s", game.c_str(), ok ? "ok" : "fail");
    return ok;
}

void RetroEngineHost::Start()
{
    if (!loaded_)
        return;
    running_ = true;
    paused_ = false;
    cv_.notify_all();
}

void RetroEngineHost::SetSpeedPercent(int pct)
{
    pct = std::clamp(pct, 100, 800);
    speed_pct_ = pct;
    psx_shm_store(&shm_->speed_percent, (uint32_t)pct);
}

bool RetroEngineHost::SaveState(const std::string &path)
{
    std::vector<uint8_t> buf;
    bool ok = Call([&] {
        size_t size = core_.SerializeSize();
        if (size == 0)
            return false;
        buf.resize(size);
        return core_.Serialize(buf.data(), buf.size());
    });
    if (!ok)
        return false;
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (f == nullptr)
        return false;
    ok = fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

bool RetroEngineHost::LoadState(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    std::vector<uint8_t> buf;
    bool read_ok = ReadAll(fileno(f), &buf);
    fclose(f);
    if (!read_ok || buf.empty())
        return false;
    return Call([&] { return core_.Unserialize(buf.data(), buf.size()); });
}

// ---------------------------------------------------------------- поток ядра

void RetroEngineHost::ThreadMain()
{
    using clock = std::chrono::steady_clock;
    auto next_frame = clock::now();
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (quit_ && tasks_.empty())
                break;
            bool idle = !running_ || paused_;
            if (tasks_.empty() && idle)
                cv_.wait_for(lk, std::chrono::milliseconds(20));
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
        }
        if (task) {
            task();
            next_frame = clock::now();
            continue;
        }
        if (!running_ || paused_) {
            if (loaded_)
                core_.SaveSram(); // на паузе (свернули приложение) — сейв картриджа на диск
            next_frame = clock::now();
            continue;
        }

        Frame();

        // покадровый лимит: абсолютное расписание без дрейфа
        double frame_s = 1.0 / core_.Fps() * 100.0 / speed_pct_.load();
        next_frame += std::chrono::nanoseconds((long long)(frame_s * 1e9));
        auto now = clock::now();
        if (next_frame > now)
            std::this_thread::sleep_until(next_frame);
        else
            next_frame = now;
    }
    log_info("core thread exited");
}

void RetroEngineHost::Frame()
{
    // кнопки — раз в кадр, перед исполнением (как input_poll в libretro)
    core_.SetInput(psx_shm_load(&shm_->pad_buttons[0]));
    auto t0 = std::chrono::steady_clock::now();
    core_.Run();
    auto work = std::chrono::steady_clock::now() - t0;
    PublishFrame();
    psx_shm_add(&shm_->emu_ns_total,
                (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(work).count());
    psx_shm_add(&shm_->frames_done, (uint64_t)1);
    // сейв картриджа — раз в ~10 с, только если менялся
    if (++sram_frames_ % 600 == 0)
        core_.SaveSram();
}

void RetroEngineHost::PublishFrame()
{
    const uint16_t *px;
    int w, h;
    if (!core_.Frame(&px, &w, &h))
        return;
    int cw = w > PSX_VIDEO_MAX_W ? PSX_VIDEO_MAX_W : w;
    int ch = h > PSX_VIDEO_MAX_H ? PSX_VIDEO_MAX_H : h;
    uint16_t *dst = psx_shm_video_slot(shm_, video_back_);
    for (int y = 0; y < ch; y++)
        memcpy(dst + (size_t)y * cw, px + (size_t)y * w, (size_t)cw * 2);
    shm_->video_meta[video_back_].w = (uint32_t)cw;
    shm_->video_meta[video_back_].h = (uint32_t)ch;
    uint32_t prev = psx_shm_exchange(&shm_->video_middle, video_back_ | PSX_VIDEO_FRESH);
    video_back_ = prev & PSX_VIDEO_INDEX;
    psx_shm_store(&shm_->video_w, (uint32_t)cw);
    psx_shm_store(&shm_->video_h, (uint32_t)ch);
    uint64_t one = 1;
    (void)!write(event_fd_, &one, sizeof(one));
}

// звук ядра -> PSX_AUDIO_RATE: выше — усреднение по окну (заодно грубый ФНЧ,
// у Mega Drive ~223 кГц), ниже — линейная интерполяция, равная — как есть
void RetroEngineHost::WriteAudio(const int16_t *frames, size_t count)
{
    const double in_rate = core_.SampleRate();
    if (in_rate == PSX_AUDIO_RATE) {
        WriteRing(frames, count);
        return;
    }
    const double step = in_rate / PSX_AUDIO_RATE; // входных фреймов на выходной
    resample_out_.clear();
    if (step > 1.0) {
        for (size_t i = 0; i < count; i++) {
            resample_acc_[0] += frames[i * 2];
            resample_acc_[1] += frames[i * 2 + 1];
            resample_cnt_++;
            resample_pos_ += 1.0;
            if (resample_pos_ >= step) {
                resample_pos_ -= step;
                resample_out_.push_back((int16_t)(resample_acc_[0] / resample_cnt_));
                resample_out_.push_back((int16_t)(resample_acc_[1] / resample_cnt_));
                resample_acc_[0] = resample_acc_[1] = 0;
                resample_cnt_ = 0;
            }
        }
    } else {
        // resample_pos_ — позиция выхода между prev (0) и текущим (1) фреймом
        for (size_t i = 0; i < count; i++) {
            int16_t cur[2] = {frames[i * 2], frames[i * 2 + 1]};
            while (resample_pos_ < 1.0) {
                for (int c = 0; c < 2; c++)
                    resample_out_.push_back((int16_t)(resample_prev_[c] + (cur[c] - resample_prev_[c]) * resample_pos_));
                resample_pos_ += step;
            }
            resample_pos_ -= 1.0;
            resample_prev_[0] = cur[0];
            resample_prev_[1] = cur[1];
        }
    }
    WriteRing(resample_out_.data(), resample_out_.size() / 2);
}

// -> кольцо в общей памяти; ядро идёт по нашему таймеру кадров,
// так что переполнение — только если приложение не читает (пауза)
void RetroEngineHost::WriteRing(const int16_t *frames, size_t count)
{
    uint64_t head = __atomic_load_n(&shm_->audio_head, __ATOMIC_RELAXED);
    uint64_t tail = psx_shm_load(&shm_->audio_tail);
    uint64_t space = PSX_AUDIO_FRAMES - (head - tail);
    if (count > space) {
        psx_shm_add(&shm_->audio_overflows, (uint64_t)1);
        count = (size_t)space;
    }
    int16_t *ring = psx_shm_audio(shm_);
    for (size_t i = 0; i < count; i++) {
        size_t idx = (size_t)((head + i) & (PSX_AUDIO_FRAMES - 1));
        ring[idx * 2] = frames[i * 2];
        ring[idx * 2 + 1] = frames[i * 2 + 1];
    }
    psx_shm_store(&shm_->audio_head, head + (uint64_t)count);
}
