// SPDX-License-Identifier: MIT
// Общая память между приложением и движком эмуляции.
//
// Движок создаёт регион (ASharedMemory) и отдаёт его через
// IPsxEngine.sharedMemory(). Внутри — заголовок, три буфера кадра и кольцо
// звука. Поля, которые меняют обе стороны на ходу, читаются и пишутся только
// атомарно (psx_shm_load/psx_shm_store ниже). Писатели:
//   движок     — кадр, звук (head), статус;
//   приложение — кнопки и оси, звук (tail).
#ifndef PSX_ENGINE_SHM_H
#define PSX_ENGINE_SHM_H

#include <stddef.h>
#include <stdint.h>

#define PSX_SHM_MAGIC   0x31585350u /* "PSX1" */
#define PSX_SHM_VERSION 1u

// ---- кадр: тройная буферизация RGB565, строки без зазоров (pitch = w*2)
#define PSX_VIDEO_SLOTS  3
#define PSX_VIDEO_MAX_W  1024
#define PSX_VIDEO_MAX_H  512
#define PSX_VIDEO_SLOT_BYTES (PSX_VIDEO_MAX_W * PSX_VIDEO_MAX_H * 2)
// в video_middle: номер буфера | PSX_VIDEO_FRESH, если в нём кадр,
// который читатель ещё не забрал
#define PSX_VIDEO_FRESH  0x4u
#define PSX_VIDEO_INDEX  0x3u

// ---- звук: кольцо стерео-фреймов s16 44100 Гц, размер — степень двойки
#define PSX_AUDIO_RATE   44100
#define PSX_AUDIO_FRAMES 16384

// ---- ввод
#define PSX_PADS         2
#define PSX_AXIS_CENTER  127

struct psx_video_meta {
    uint32_t w;
    uint32_t h;
};

struct psx_shm {
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;
    uint32_t video_offset;      // от начала региона: PSX_VIDEO_SLOTS буферов
    uint32_t audio_offset;      // от начала региона: PSX_AUDIO_FRAMES * 2 int16

    // кадр. Писатель заполняет свой буфер, затем меняет его местами с
    // video_middle (с флагом FRESH); читатель, увидев FRESH, меняет свой
    // буфер с video_middle. Буфер, которым владеет сторона, другая не трогает.
    uint32_t video_middle;
    struct psx_video_meta video_meta[PSX_VIDEO_SLOTS];
    uint32_t video_w;           // размер последнего кадра
    uint32_t video_h;

    // ввод: бит кнопки = 1 << dkey (DKEY_* ядра), оси — по байту на ось
    // (lx | ly << 8 | rx << 16 | ry << 24), 0..255, центр PSX_AXIS_CENTER
    uint32_t pad_buttons[PSX_PADS];
    uint32_t pad_axes[PSX_PADS];

    // статус движка
    int32_t  cdrom_status;      // статус-байт привода (STATUS_* ядра)
    uint32_t speed_percent;
    uint64_t frames_done;
    uint64_t emu_ns_total;      // время исполнения кадров, нс

    // звук: head двигает движок, tail — приложение
    uint64_t audio_overflows;
    uint64_t audio_head __attribute__((aligned(64)));
    uint64_t audio_tail __attribute__((aligned(64)));
};

#define PSX_SHM_TOTAL_SIZE                                                   \
    (((sizeof(struct psx_shm) + 4095) & ~(size_t)4095) +                     \
     (size_t)PSX_VIDEO_SLOTS * PSX_VIDEO_SLOT_BYTES +                        \
     (size_t)PSX_AUDIO_FRAMES * 2 * sizeof(int16_t))

// атомарный доступ к полям заголовка (одинаково для C и C++)
#define psx_shm_load(p)        __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define psx_shm_store(p, v)    __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define psx_shm_exchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_ACQ_REL)
#define psx_shm_or(p, v)       __atomic_fetch_or((p), (v), __ATOMIC_ACQ_REL)
#define psx_shm_and(p, v)      __atomic_fetch_and((p), (v), __ATOMIC_ACQ_REL)
#define psx_shm_add(p, v)      __atomic_fetch_add((p), (v), __ATOMIC_ACQ_REL)

static inline uint16_t *psx_shm_video_slot(struct psx_shm *shm, uint32_t index)
{
    return (uint16_t *)((uint8_t *)shm + shm->video_offset +
                        (size_t)index * PSX_VIDEO_SLOT_BYTES);
}

static inline int16_t *psx_shm_audio(struct psx_shm *shm)
{
    return (int16_t *)((uint8_t *)shm + shm->audio_offset);
}

#endif // PSX_ENGINE_SHM_H
