// SPDX-License-Identifier: MIT
#include "LibretroHost.h"

#include <android/log.h>
#include <dlfcn.h>
#include <zlib.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#define LOG_TAG "retrohost"
#define log_info(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define log_err(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

LibretroHost *LibretroHost::s_host = nullptr;

namespace {

// бит DKEY (SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT, L2, R2, L1, R1,
// TRIANGLE, CIRCLE, CROSS, SQUARE) -> id кнопки libretro.
// Кружок = A, крест = B, треугольник = X, квадрат = Y — как у RetroArch.
constexpr unsigned kDkeyToRetro[16] = {
    RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3,
    RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_RIGHT,
    RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_L2,
    RETRO_DEVICE_ID_JOYPAD_R2, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R,
    RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
    RETRO_DEVICE_ID_JOYPAD_Y,
};

void RetroLog(enum retro_log_level level, const char *fmt, ...)
{
    static const int prio[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(prio[level <= RETRO_LOG_ERROR ? level : RETRO_LOG_ERROR], "retrocore", fmt, ap);
    va_end(ap);
}

inline uint16_t Rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

} // namespace

LibretroHost::~LibretroHost()
{
    Deinit();
}

bool LibretroHost::LoadCore(const std::string &path)
{
    lib_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (lib_ == nullptr) {
        log_err("dlopen %s: %s", path.c_str(), dlerror());
        return false;
    }
    bool ok = true;
    auto sym = [&](const char *name) {
        void *p = dlsym(lib_, name);
        if (p == nullptr) {
            log_err("core: no %s", name);
            ok = false;
        }
        return p;
    };
#define LOAD(field, name) api_.field = reinterpret_cast<decltype(api_.field)>(sym(name))
    LOAD(init, "retro_init");
    LOAD(deinit, "retro_deinit");
    LOAD(get_system_info, "retro_get_system_info");
    LOAD(get_system_av_info, "retro_get_system_av_info");
    LOAD(set_environment, "retro_set_environment");
    LOAD(set_video_refresh, "retro_set_video_refresh");
    LOAD(set_audio_sample, "retro_set_audio_sample");
    LOAD(set_audio_sample_batch, "retro_set_audio_sample_batch");
    LOAD(set_input_poll, "retro_set_input_poll");
    LOAD(set_input_state, "retro_set_input_state");
    LOAD(set_controller_port_device, "retro_set_controller_port_device");
    LOAD(load_game, "retro_load_game");
    LOAD(unload_game, "retro_unload_game");
    LOAD(run, "retro_run");
    LOAD(serialize_size, "retro_serialize_size");
    LOAD(serialize, "retro_serialize");
    LOAD(unserialize, "retro_unserialize");
    LOAD(get_memory_data, "retro_get_memory_data");
    LOAD(get_memory_size, "retro_get_memory_size");
#undef LOAD
    return ok;
}

bool LibretroHost::Init(const Config &config)
{
    if (s_host != nullptr) {
        log_err("host already exists");
        return false;
    }
    config_ = config;
    if (!LoadCore(config.core_path))
        return false;
    s_host = this;
    api_.set_environment(EnvironmentCb);
    api_.set_video_refresh(VideoRefreshCb);
    api_.set_audio_sample(AudioSampleCb);
    api_.set_audio_sample_batch(AudioBatchCb);
    api_.set_input_poll(InputPollCb);
    api_.set_input_state(InputStateCb);
    api_.init();
    inited_ = true;

    retro_system_info info{};
    api_.get_system_info(&info);
    need_fullpath_ = info.need_fullpath;
    log_info("core %s %s, need_fullpath=%d", info.library_name ? info.library_name : "?",
             info.library_version ? info.library_version : "", need_fullpath_ ? 1 : 0);
    return true;
}

bool LibretroHost::LoadGame(const std::string &path, std::vector<uint8_t> data)
{
    rom_ = std::move(data);
    rom_crc_ = (uint32_t)crc32(0L, rom_.data(), (uInt)rom_.size());
    rom_path_ = path;
    // ядро откроет файл само (и, возможно, без GET_GAME_INFO_EXT): даём копию
    // ROM по обычному пути. Картриджи — единицы мегабайт
    if (need_fullpath_ && !config_.cache_dir.empty()) {
        size_t s = path.find_last_of('/');
        rom_path_ = config_.cache_dir + "/" + (s == std::string::npos ? path : path.substr(s + 1));
        FILE *f = fopen(rom_path_.c_str(), "wb");
        bool ok = f != nullptr && fwrite(rom_.data(), 1, rom_.size(), f) == rom_.size();
        if (f != nullptr)
            ok = (fclose(f) == 0) && ok;
        if (!ok) {
            log_err("can't write %s", rom_path_.c_str());
            return false;
        }
    }
    const std::string &rpath = rom_path_;
    size_t slash = rpath.find_last_of('/');
    rom_dir_ = slash == std::string::npos ? "" : rpath.substr(0, slash);
    std::string file = slash == std::string::npos ? rpath : rpath.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    rom_name_ = dot == std::string::npos ? file : file.substr(0, dot);
    rom_ext_ = dot == std::string::npos ? "" : file.substr(dot + 1);

    info_ext_ = {};
    info_ext_.full_path = rom_path_.c_str();
    info_ext_.dir = rom_dir_.c_str();
    info_ext_.name = rom_name_.c_str();
    info_ext_.ext = rom_ext_.c_str();
    info_ext_.data = rom_.data();
    info_ext_.size = rom_.size();
    info_ext_.file_in_archive = false;
    info_ext_.persistent_data = true;

    retro_game_info game{};
    game.path = rom_path_.c_str();
    game.data = rom_.data();
    game.size = rom_.size();
    if (!api_.load_game(&game)) {
        log_err("retro_load_game failed: %s", path.c_str());
        return false;
    }
    loaded_ = true;
    // retro_set_controller_port_device не зовём: ядра по умолчанию подключают
    // свой пад, а на RETRO_DEVICE_JOYPAD некоторые (FCEUmm) отключают порт
    api_.get_system_av_info(&av_);
    log_info("loaded %s (crc %08x): %ux%u @ %.2f fps, audio %.0f Hz", path.c_str(), rom_crc_,
             av_.geometry.base_width, av_.geometry.base_height, av_.timing.fps, av_.timing.sample_rate);
    LoadSram();
    return true;
}

void LibretroHost::Run()
{
    if (loaded_)
        api_.run();
}

void LibretroHost::Deinit()
{
    if (!inited_)
        return;
    if (loaded_) {
        SaveSram();
        api_.unload_game();
    }
    api_.deinit();
    loaded_ = false;
    inited_ = false;
    if (s_host == this)
        s_host = nullptr;
    // dlclose не зовём: ядра не рассчитаны на выгрузку и повторную загрузку
}

double LibretroHost::AspectRatio() const
{
    if (av_.geometry.aspect_ratio > 0)
        return av_.geometry.aspect_ratio;
    return av_.geometry.base_height ? (double)av_.geometry.base_width / av_.geometry.base_height : 4.0 / 3.0;
}

void LibretroHost::SetInput(uint32_t dkey_buttons)
{
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++) {
        if (dkey_buttons & (1u << i))
            mask |= 1u << kDkeyToRetro[i];
    }
    retro_buttons_ = mask;
}

bool LibretroHost::Frame(const uint16_t **px, int *w, int *h) const
{
    if (frame_w_ <= 0 || frame_h_ <= 0)
        return false;
    *px = frame_.data();
    *w = frame_w_;
    *h = frame_h_;
    return true;
}

size_t LibretroHost::SerializeSize()
{
    return loaded_ ? api_.serialize_size() : 0;
}

bool LibretroHost::Serialize(void *data, size_t size)
{
    return loaded_ && api_.serialize(data, size);
}

bool LibretroHost::Unserialize(const void *data, size_t size)
{
    return loaded_ && api_.unserialize(data, size);
}

// ---------------------------------------------------------------- SRAM

std::string LibretroHost::SramPath() const
{
    char name[16];
    snprintf(name, sizeof(name), "%08x", rom_crc_);
    return config_.save_dir + "/" + name + ".srm";
}

void LibretroHost::LoadSram()
{
    size_t size = api_.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *mem = api_.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (size == 0 || mem == nullptr)
        return;
    FILE *f = fopen(SramPath().c_str(), "rb");
    if (f == nullptr)
        return;
    size_t n = fread(mem, 1, size, f);
    fclose(f);
    sram_saved_.assign(static_cast<uint8_t *>(mem), static_cast<uint8_t *>(mem) + size);
    log_info("sram loaded: %zu of %zu bytes", n, size);
}

void LibretroHost::SaveSram()
{
    if (!loaded_)
        return;
    size_t size = api_.get_memory_size(RETRO_MEMORY_SAVE_RAM);
    void *mem = api_.get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (size == 0 || mem == nullptr)
        return;
    // пишем только изменения: зовётся и по таймеру
    if (sram_saved_.size() == size && memcmp(sram_saved_.data(), mem, size) == 0)
        return;
    std::string tmp = SramPath() + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (f == nullptr)
        return;
    bool ok = fwrite(mem, 1, size, f) == size;
    ok = (fclose(f) == 0) && ok;
    if (ok && rename(tmp.c_str(), SramPath().c_str()) == 0)
        sram_saved_.assign(static_cast<uint8_t *>(mem), static_cast<uint8_t *>(mem) + size);
}

// ---------------------------------------------------------------- окружение

bool LibretroHost::Environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *static_cast<const char **>(data) = config_.system_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char **>(data) = config_.save_dir.c_str();
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        pixel_format_ = *static_cast<const retro_pixel_format *>(data);
        return true;
    case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
        // ROM из памяти: ядро не открывает файл по пути (SAF)
        *static_cast<const retro_game_info_ext **>(data) = &info_ext_;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        // "Описание; умолчание|вариант|…" — берём умолчание, если не задано нами
        for (auto *v = static_cast<const retro_variable *>(data); v && v->key; v++) {
            std::string value;
            if (v->value) {
                const char *semi = strchr(v->value, ';');
                std::string opts = semi ? semi + 1 : v->value;
                size_t start = opts.find_first_not_of(' ');
                opts = start == std::string::npos ? "" : opts.substr(start);
                value = opts.substr(0, opts.find('|'));
            }
            auto it = config_.options.find(v->key);
            variables_[v->key] = it != config_.options.end() ? it->second : value;
        }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto *var = static_cast<retro_variable *>(data);
        auto it = var && var->key ? variables_.find(var->key) : variables_.end();
        if (it == variables_.end())
            return false;
        var->value = it->second.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool *>(data) = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback *>(data)->log = RetroLog;
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        av_.geometry = *static_cast<const retro_game_geometry *>(data);
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        av_ = *static_cast<const retro_system_av_info *>(data);
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- видео

void LibretroHost::VideoRefresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (data == nullptr || w == 0 || h == 0)
        return; // кадр не изменился — оставляем прошлый
    frame_.resize((size_t)w * h);
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = static_cast<const uint8_t *>(data) + y * pitch;
        uint16_t *dst = &frame_[(size_t)y * w];
        switch (pixel_format_) {
        case RETRO_PIXEL_FORMAT_RGB565:
            memcpy(dst, row, (size_t)w * 2);
            break;
        case RETRO_PIXEL_FORMAT_XRGB8888:
            for (unsigned x = 0; x < w; x++) {
                const uint8_t *p = row + x * 4; // B G R X
                dst[x] = Rgb565(p[2], p[1], p[0]);
            }
            break;
        default: // 0RGB1555
            for (unsigned x = 0; x < w; x++) {
                uint16_t p = reinterpret_cast<const uint16_t *>(row)[x];
                dst[x] = (uint16_t)(((p & 0x7c00) << 1) | ((p & 0x03e0) << 1) | (p & 0x001f));
            }
            break;
        }
    }
    frame_w_ = (int)w;
    frame_h_ = (int)h;
}

// ---------------------------------------------------------------- ввод

int16_t LibretroHost::InputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
        return (int16_t)retro_buttons_;
    return id < 16 ? (int16_t)((retro_buttons_ >> id) & 1) : 0;
}

// ---------------------------------------------------------------- статические мосты

bool LibretroHost::EnvironmentCb(unsigned cmd, void *data)
{
    return s_host != nullptr && s_host->Environment(cmd, data);
}

void LibretroHost::VideoRefreshCb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    if (s_host != nullptr)
        s_host->VideoRefresh(data, w, h, pitch);
}

void LibretroHost::AudioSampleCb(int16_t left, int16_t right)
{
    if (s_host != nullptr && s_host->on_audio) {
        int16_t frame[2] = {left, right};
        s_host->on_audio(frame, 1);
    }
}

size_t LibretroHost::AudioBatchCb(const int16_t *data, size_t frames)
{
    if (s_host != nullptr && s_host->on_audio)
        s_host->on_audio(data, frames);
    return frames;
}

void LibretroHost::InputPollCb()
{
}

int16_t LibretroHost::InputStateCb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    return s_host != nullptr ? s_host->InputState(port, device, index, id) : 0;
}
