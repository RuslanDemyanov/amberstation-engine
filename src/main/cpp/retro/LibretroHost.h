// SPDX-License-Identifier: MIT
// Фронтенд libretro-ядра: то, что в RetroArch делает сам RetroArch. Ядро —
// отдельная .so (по одной на платформу), грузится dlopen'ом; хост один на
// процесс. Только программный вывод: кадр в памяти, наружу RGB565.
// Все вызовы ядра — из одного «потока ядра» (RetroEngineHost).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "libretro.h"

class LibretroHost {
public:
    struct Config {
        std::string core_path;    // .so ядра
        std::string system_dir;   // данные ядра
        std::string save_dir;     // батарейные сейвы картриджей (SRAM)
        std::string cache_dir;    // временная копия ROM для ядер с need_fullpath
        // значения опций ядра поверх его умолчаний (ключ -> значение)
        std::map<std::string, std::string> options;
    };

    // стерео s16 с частотой SampleRate()
    std::function<void(const int16_t *frames, size_t count)> on_audio;

    ~LibretroHost();

    bool Init(const Config &config);
    // ROM в памяти (ядро не открывает файлы само: SAF); path — имя для ядра
    // ядрам, которые читают файл сами (need_fullpath), ROM пишется во
    // временный файл в cache_dir
    bool LoadGame(const std::string &path, std::vector<uint8_t> data);
    void Run();
    void Deinit();

    double Fps() const { return av_.timing.fps > 0 ? av_.timing.fps : 60.0; }
    double SampleRate() const { return av_.timing.sample_rate > 0 ? av_.timing.sample_rate : 44100.0; }
    double AspectRatio() const;

    // ввод перед кадром: биты DKEY (раскладка PSX-пада, см. kDkeyToRetro)
    void SetInput(uint32_t dkey_buttons);

    // последний кадр RGB565 сверху-вниз (повторяется, если ядро не рисовало)
    bool Frame(const uint16_t **px, int *w, int *h) const;

    size_t SerializeSize();
    bool Serialize(void *data, size_t size);
    bool Unserialize(const void *data, size_t size);

    // батарейный сейв картриджа: файл save_dir/<crc ROM>.srm
    void LoadSram();
    void SaveSram();

private:
    struct Api {
        void (*init)();
        void (*deinit)();
        void (*get_system_info)(retro_system_info *);
        void (*get_system_av_info)(retro_system_av_info *);
        void (*set_environment)(retro_environment_t);
        void (*set_video_refresh)(retro_video_refresh_t);
        void (*set_audio_sample)(retro_audio_sample_t);
        void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
        void (*set_input_poll)(retro_input_poll_t);
        void (*set_input_state)(retro_input_state_t);
        void (*set_controller_port_device)(unsigned, unsigned);
        bool (*load_game)(const retro_game_info *);
        void (*unload_game)();
        void (*run)();
        size_t (*serialize_size)();
        bool (*serialize)(void *, size_t);
        bool (*unserialize)(const void *, size_t);
        void *(*get_memory_data)(unsigned);
        size_t (*get_memory_size)(unsigned);
    };

    bool LoadCore(const std::string &path);
    bool Environment(unsigned cmd, void *data);
    void VideoRefresh(const void *data, unsigned w, unsigned h, size_t pitch);
    int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id);
    std::string SramPath() const;

    static bool EnvironmentCb(unsigned cmd, void *data);
    static void VideoRefreshCb(const void *data, unsigned w, unsigned h, size_t pitch);
    static void AudioSampleCb(int16_t left, int16_t right);
    static size_t AudioBatchCb(const int16_t *data, size_t frames);
    static void InputPollCb();
    static int16_t InputStateCb(unsigned port, unsigned device, unsigned index, unsigned id);

    static LibretroHost *s_host;

    void *lib_ = nullptr;
    Api api_{};
    Config config_;
    bool inited_ = false;
    bool loaded_ = false;
    bool need_fullpath_ = false;
    retro_system_av_info av_{};
    retro_pixel_format pixel_format_ = RETRO_PIXEL_FORMAT_0RGB1555;
    std::map<std::string, std::string> variables_; // ключ -> текущее значение

    // игра: данные ROM живут, пока игра загружена (ядро может держать указатель)
    std::vector<uint8_t> rom_;
    std::string rom_path_, rom_dir_, rom_name_, rom_ext_;
    retro_game_info_ext info_ext_{};
    uint32_t rom_crc_ = 0;
    std::vector<uint8_t> sram_saved_; // содержимое файла SRAM

    uint32_t retro_buttons_ = 0;

    std::vector<uint16_t> frame_;
    int frame_w_ = 0, frame_h_ = 0;
};
