// Android-фронтенд ядра PCSX-ReARMed.
// Портировано по образцу frontend/libretro.c (GPLv2+, (C) notaz 2012-2015):
// реализует frontend-хуки ядра — видео (vout), аудио (dfsound out-драйвер),
// ввод (in_keystate/...), сейв-стейты (SaveFuncs) и вибрацию.
//
// This work is licensed under the terms of the GNU GPLv2 or later.
// See COPYING in the pcsx_rearmed submodule.

#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <android/log.h>

#include "psxcommon.h"
#include "r3000a.h"
#include "psxcounters.h"
#include "psxmem_map.h"
#include "misc.h"
#include "cdrom.h"
#include "plugins.h"
#include "gpu.h"
#include "sio.h"
#include "cheat.h"
#include "database.h"
#include "plugin.h"        // frontend/plugin.h
#include "plugin_lib.h"    // frontend/plugin_lib.h (rearmed_cbs, in_*)
#include "main.h"          // frontend/main.h (emu_on_new_cd, set_cd_image, ...)
#include "cspace.h"
#include "psemu_plugin_defs.h"
#include "revision.h"

#include "../plugins/dfsound/out.h"
#include "../plugins/dfsound/spu_config.h"
#include "cdrom-async.h"

#include "emu_frontend.h"

#define LOG_TAG "ps1emu.core"
#define log_info(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define log_err(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------- глобалы ввода
// читаются ядром (frontend/plugin.c PAD1/2_readPort), пишутся из JNI lock-free
int in_type[8] = {
    PSE_PAD_TYPE_ANALOGPAD, PSE_PAD_TYPE_ANALOGPAD,
    PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE,
    PSE_PAD_TYPE_NONE, PSE_PAD_TYPE_NONE
};
int in_analog_left[8][2]  = { { 127, 127 }, { 127, 127 } };
int in_analog_right[8][2] = { { 127, 127 }, { 127, 127 } };
unsigned short in_keystate[8];
int in_mouse[8][2];
int multitap1, multitap2;
int in_enable_vibration = 1;

// ---------------------------------------------------------------- состояние
static struct emu_callbacks g_cbs;
static int g_inited, g_booted;
static const char *g_disc_path;

static uint16_t *g_vout_buf;        // RGB565
static int g_vout_w = 320, g_vout_h = 240;

// gpulib может обращаться к этому (скриншоты и т.п.)
void *pl_vout_buf;
int g_layer_x, g_layer_y, g_layer_w, g_layer_h;

// ---------------------------------------------------------------- видео
#define VOUT_MAX_WIDTH  1024
#define VOUT_MAX_HEIGHT 512

static int vout_open(void) { return 0; }
static void vout_close(void) {}

static void vout_set_mode(int w, int h, int raw_w, int raw_h, int bpp)
{
    (void)raw_w; (void)raw_h; (void)bpp;
    if (w <= 0 || h <= 0)
        return;
    if (w > VOUT_MAX_WIDTH)  w = VOUT_MAX_WIDTH;
    if (h > VOUT_MAX_HEIGHT) h = VOUT_MAX_HEIGHT;
    g_vout_w = w;
    g_vout_h = h;
    memset(g_vout_buf, 0, (size_t)VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);
}

// упрощённый порт vout_flip() из libretro.c (только RGB565)
static void vout_flip(const void *vram_, int vram_ofs, int bgr24,
      int x, int y, int w, int h, int dims_changed)
{
    void (*bgr_to_fb)(void *dst, const void *src, int bytes) =
        bgr24 ? bgr888_to_rgb565 : bgr555_to_rgb565;
    int bytes_pp = 2;
    int bytes_pp_s = bgr24 ? 3 : 2;
    uint8_t *dest = (uint8_t *)g_vout_buf;
    const uint8_t *vram = vram_;
    int dstride = g_vout_w * 2;
    int sstride = 2048;
    int w_blit = w < g_vout_w ? w : g_vout_w;
    int h1;

    if (vram == NULL || dims_changed) {
        memset(dest, 0, (size_t)dstride * g_vout_h);
        if (vram == NULL)
            goto out;
    }

    if (h >= g_vout_h * 3 / 2) {  // 480i: GPU чередует строки
        sstride = 4096;
        h /= 2;
    }
    if (h > g_vout_h) h = g_vout_h;
    dest += x * bytes_pp + y * dstride;

    for (h1 = h; h1-- > 0; dest += dstride) {
        bgr_to_fb(dest, vram + vram_ofs, w_blit);
        vram_ofs = (vram_ofs + sstride) & 0xfffff;
    }

out:
    pl_vout_buf = g_vout_buf;
    if (g_cbs.video_frame)
        g_cbs.video_frame(g_vout_buf, g_vout_w, g_vout_h, dstride);
}

static void *pl_mmap(unsigned int size)
{
    return psxMap(0, size, 0, MAP_TAG_VRAM);
}

static void pl_munmap(void *ptr, unsigned int size)
{
    psxUnmap(ptr, size, MAP_TAG_VRAM);
}

struct rearmed_cbs pl_rearmed_cbs = {
    .pl_vout_open     = vout_open,
    .pl_vout_set_mode = vout_set_mode,
    .pl_vout_flip     = vout_flip,
    .pl_vout_close    = vout_close,
    .cspace_blit      = bgr555_to_rgb565,
    .mmap             = pl_mmap,
    .munmap           = pl_munmap,
    .gpu_state_change = gpu_state_change,
    /* из psxcounters */
    .gpu_hcnt         = &hSyncCount,
    .gpu_frame_count  = &frame_counter,
};

// тактирование: ядро вызывает EmuUpdate -> pl_frame_limit раз в кадр,
// psxRegs.stop++ заставляет psxCpu->Execute() вернуться => кадр завершён
void pl_frame_limit(void)
{
    psxRegs.stop++;
}

static double g_frame_time; // сек на кадр, выставляется в pl_timing_prepare
void pl_timing_prepare(int is_pal)
{
    g_frame_time = is_pal ? 1.0 / 50.0 : 1.0 / 60.0;
}

double emu_get_frame_time(void)
{
    return g_frame_time > 0 ? g_frame_time : 1.0 / 60.0;
}

// ---------------------------------------------------------------- вибрация
void plat_trigger_vibrate(int pad, int low, int high)
{
    if (g_cbs.vibrate)
        g_cbs.vibrate(pad, low, high);
}

// световые пистолеты не поддерживаем (M0); pad.c пишет сюда байты конвейра gun
void pl_gun_byte2(int port, unsigned char byte)
{
    (void)port; (void)byte;
}

// ---------------------------------------------------------------- аудио
static int snd_init(void) { return 0; }
static void snd_finish(void) {}
static int snd_busy(void) { return 0; }

static void snd_feed(void *buf, int bytes)
{
    if (g_cbs.audio_write)
        g_cbs.audio_write((const int16_t *)buf, bytes / 4);
}

// dfsound/out.c регистрирует этот драйвер (собран с -DHAVE_LIBRETRO)
void out_register_libretro(struct out_driver *drv)
{
    drv->name   = "android";
    drv->init   = snd_init;
    drv->finish = snd_finish;
    drv->busy   = snd_busy;
    drv->feed   = snd_feed;
}

// ---------------------------------------------------------------- сейв-стейты
struct save_fp {
    char *buf;
    size_t pos;
    size_t size;
    int is_write;
};

static void *save_open(const char *name, const char *mode)
{
    struct save_fp *fp;
    if (name == NULL || mode == NULL)
        return NULL;
    fp = malloc(sizeof(*fp));
    if (fp == NULL)
        return NULL;
    fp->buf = (char *)name;
    fp->pos = 0;
    fp->size = EMU_STATE_SIZE;
    fp->is_write = (mode[0] == 'w' || mode[1] == 'w');
    return fp;
}

static int save_read(void *file, void *buf, u32 len)
{
    struct save_fp *fp = file;
    if (fp == NULL || buf == NULL)
        return -1;
    if (fp->pos + len > fp->size)
        return -1;
    memcpy(buf, fp->buf + fp->pos, len);
    fp->pos += len;
    return len;
}

static int save_write(void *file, const void *buf, u32 len)
{
    struct save_fp *fp = file;
    if (fp == NULL || buf == NULL)
        return -1;
    if (fp->pos + len > fp->size)
        return -1;
    memcpy(fp->buf + fp->pos, buf, len);
    fp->pos += len;
    return len;
}

static long save_seek(void *file, long offs, int whence)
{
    struct save_fp *fp = file;
    if (fp == NULL)
        return -1;
    switch (whence) {
    case SEEK_CUR:
        fp->pos += offs;
        return fp->pos;
    case SEEK_SET:
        fp->pos = offs;
        return fp->pos;
    default:
        return -1;
    }
}

static void save_close(void *file)
{
    struct save_fp *fp = file;
    if (fp == NULL)
        return;
    if (fp->is_write && fp->pos < fp->size)
        memset(fp->buf + fp->pos, 0, fp->size - fp->pos);
    free(fp);
}

struct PcsxSaveFuncs SaveFuncs = {
    save_open, save_read, save_write, save_seek, save_close
};

int emu_state_save(void *buf, size_t size)
{
    int ret;
    if (!g_booted || buf == NULL || size < EMU_STATE_SIZE)
        return -1;
    CdromFrontendId = 0;
    // SaveState() открывает "файл" через SaveFuncs: для нас это буфер в памяти
    ret = SaveState((const char *)buf);
    return ret == 0 ? 0 : -1;
}

int emu_state_load(const void *buf, size_t size)
{
    int ret;
    if (!g_booted || buf == NULL || size < EMU_STATE_SIZE)
        return -1;
    CdromFrontendId = -1;
    ret = LoadState((const char *)buf);
    return ret == 0 ? 0 : -1;
}

// ---------------------------------------------------------------- ввод
void emu_set_key(int pad, int dkey, int pressed)
{
    if ((unsigned)pad >= 8 || (unsigned)dkey >= 16)
        return;
    unsigned short mask = (unsigned short)(1u << dkey);
    if (pressed)
        __sync_or_and_fetch(&in_keystate[pad], mask);
    else
        __sync_and_and_fetch(&in_keystate[pad], (unsigned short)~mask);
}

void emu_set_analog(int pad, int axis, int value)
{
    if ((unsigned)pad >= 8 || (unsigned)axis >= 4)
        return;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    int *p = axis < 2 ? &in_analog_left[pad][axis] : &in_analog_right[pad][axis - 2];
    __atomic_store_n(p, value, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------- жизненный цикл
int emu_init(const struct emu_paths *paths, const struct emu_callbacks *cbs)
{
    int ret;

    if (g_inited)
        return -1;
    g_cbs = *cbs;

    g_vout_buf = malloc((size_t)VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);
    if (g_vout_buf == NULL)
        return -1;
    pl_vout_buf = g_vout_buf;

    ret = emu_core_preinit();  // Config = 0 + дефолты rearmed

    // свои пути: OpenBIOS на все регионы, карты памяти
    snprintf(Config.BiosDir, sizeof(Config.BiosDir), "%s", paths->bios_dir);
    for (int i = 0; i < PSX_REGION_COUNT; i++)
        snprintf(Config.Bios[i], sizeof(Config.Bios[i]), "openbios.bin");
    if (paths->mcd1)
        snprintf(Config.Mcd1, sizeof(Config.Mcd1), "%s", paths->mcd1);
    else
        snprintf(Config.Mcd1, sizeof(Config.Mcd1), "none");
    if (paths->mcd2)
        snprintf(Config.Mcd2, sizeof(Config.Mcd2), "%s", paths->mcd2);
    else
        snprintf(Config.Mcd2, sizeof(Config.Mcd2), "none");

    ret |= emu_core_init();    // EmuInit() + LoadMcds()
    if (ret != 0) {
        log_err("PCSX init failed\n");
        return -1;
    }

    // DualShock: стартует цифровым, игры с поддержкой аналога включают его сами
    // (как настоящий пад); играм, которым аналог нужен сразу, его включает база
    // хаков ядра (dualshock_init_analog). Со STANDARD стики и вибрация не работали.
    in_type[0] = PSE_PAD_TYPE_ANALOGPAD;
    in_type[1] = PSE_PAD_TYPE_ANALOGPAD;

    g_inited = 1;
    return 0;
}

int emu_set_disc(const char *path)
{
    g_disc_path = path;
    return 0;
}

// Смена диска на ходу (многодисковые игры) — как disk control libretro-сборки:
// крышка открыта -> новый образ -> крышка закрывается через ~2 с, игра сама
// перечитывает диск. lid=0 — без крышки: перед загрузкой сейва, который сам
// восстанавливает состояние привода. Вызывать при остановленном эму-потоке.
int emu_swap_disc(const char *path, int lid)
{
    if (!g_booted || path == NULL)
        return -1;
    if (lid) {
        SetCdOpenCaseTime(-1);
        LidInterrupt();
    }
    set_cd_image(path);
    if (ReloadCdromPlugin() < 0) {
        log_err("swap: ReloadCdromPlugin failed\n");
        return -1;
    }
    if (cdra_open() < 0) {
        log_err("swap: cdra_open failed (%s)\n", path);
        return -1;
    }
    if (lid) {
        SetCdOpenCaseTime(time(NULL) + 2);
        LidInterrupt();
    }
    g_disc_path = path;
    return 0;
}

int emu_boot(void)
{
    if (!g_inited || g_booted)
        return -1;

    if (g_disc_path != NULL)
        set_cd_image(g_disc_path);

    if (LoadPlugins() == -1) {
        log_err("LoadPlugins failed\n");
        return -1;
    }
    // memcards грузим позже: сначала опознаем игру (per-game карты)
    if (OpenPlugins(0) == -1) {
        log_err("OpenPlugins failed\n");
        return -1;
    }

    int has_disc = g_disc_path != NULL;
    if (has_disc && CheckCdrom() == -1) {
        log_err("unsupported/invalid CD image: %s\n", g_disc_path);
        return -1;
    }

    // игра опознана (CdromId/CdromLabel) — спрашиваем у фронтенда карту памяти
    if (has_disc && g_cbs.game_memcard_path != NULL) {
        char *mcd = g_cbs.game_memcard_path(CdromId, CdromLabel);
        if (mcd != NULL) {
            snprintf(Config.Mcd1, sizeof(Config.Mcd1), "%s", mcd);
            free(mcd);
        }
    }
    LoadMcds(Config.Mcd1, Config.Mcd2);

    plugin_call_rearmed_cbs();
    SysReset();                  // здесь psxMemReset() грузит OpenBIOS

    if (has_disc) {
        if (LoadCdromMainExe(NULL) != 0) {
            log_err("could not load main exe from CD\n");
            return -1;
        }
        emu_on_new_cd(0);
    }

    pl_timing_prepare(Config.PsxType);
    g_booted = 1;
    return 0;
}

void emu_run_frame(void)
{
    psxRegs.stop = 0;
    psxCpu->Execute(&psxRegs);
}

void emu_shutdown(void)
{
    if (!g_inited)
        return;
    if (g_booted) {
        ClosePlugins();
        g_booted = 0;
    }
    SysClose();
    {
        extern void emu_saf_reset(void);
        emu_saf_reset();
    }
    g_inited = 0;
    free(g_vout_buf);
    g_vout_buf = NULL;
    pl_vout_buf = NULL;
}

int emu_is_pal(void)
{
    return Config.PsxType;
}

int emu_get_video_frame(const uint16_t **buf, int *w, int *h, int *pitch_bytes)
{
    if (g_vout_buf == NULL)
        return 0;
    *buf = g_vout_buf;
    *w = g_vout_w;
    *h = g_vout_h;
    *pitch_bytes = g_vout_w * 2;
    return g_vout_w > 0 && g_vout_h > 0;
}
