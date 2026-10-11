// Интерфейс между нашим рантаймом (C++) и ядром PCSX-ReARMed (C).
// Единственная точку входа в ядро: все вызовы — только из эму-потока.
#ifndef EMU_FRONTEND_H
#define EMU_FRONTEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct emu_callbacks {
    // готовый RGB565-кадр (вызывается из эму-потока по флипу GPU)
    void (*video_frame)(const uint16_t *pixels, int w, int h, int pitch_bytes);
    // стерео s16 44100 Гц (вызывается из эму-потока)
    void (*audio_write)(const int16_t *frames, int frame_count);
    // вибрация от ядра (low/high 0..255)
    void (*vibrate)(int pad, int low, int high);
    // после CheckCdrom: игра опознана; фронтенд синхронно возвращает путь
    // к карте памяти этой игры (malloc'ed, ядро освободит) или NULL
    char *(*game_memcard_path)(const char *disc_id, const char *disc_label);
};

struct emu_paths {
    const char *bios_dir;  // каталог с openbios.bin
    const char *mcd1;      // путь до карты памяти 1 (или NULL)
    const char *mcd2;      // путь до карты памяти 2 (или NULL)
};

int  emu_init(const struct emu_paths *paths, const struct emu_callbacks *cbs);
// path == NULL -> загрузка в BIOS-меню (без диска)
int  emu_set_disc(const char *path);
// смена диска на ходу; lid=1 — с «открытием крышки» (игра заметит), 0 — тихо
int  emu_swap_disc(const char *path, int lid);
// статус-байт привода (STATUS_* из cdrom_bits.h): мотор, чтение, крышка
int  emu_cdr_stat(void);
// полный старт: плагины, SysReset, загрузка экзешника с CD (если диск есть)
int  emu_boot(void);
void emu_run_frame(void);      // один кадр: psxCpu->Execute()
void emu_shutdown(void);

// сейв-стейты в память (буфер >= EMU_STATE_SIZE)
#define EMU_STATE_SIZE 0x440000
int  emu_state_save(void *buf, size_t size);
int  emu_state_load(const void *buf, size_t size);

int  emu_is_pal(void);

// последний показанный кадр (как video_cb в libretro: статичные сцены
// не флипаются GPU, фронтенд обязан дублировать последний кадр).
// Вызывать только из эму-потока. 0 = кадра ещё не было.
int  emu_get_video_frame(const uint16_t **buf, int *w, int *h, int *pitch_bytes);

// ввод (потокобезопасно, можно из любого потока)
void emu_set_key(int pad, int dkey, int pressed); // dkey: 0..15, см. plugin_lib.h
void emu_set_analog(int pad, int axis, int value); // axis: 0=lx 1=ly 2=rx 3=ry, value 0..255

#ifdef __cplusplus
}
#endif

#endif // EMU_FRONTEND_H
