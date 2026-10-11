# SPDX-License-Identifier: GPL-2.0-or-later
# Движок эмуляции: PCSX-ReARMed (GPLv2) + склейка с ядром и хост сессии в
# libpsxengine.so. Работает в отдельном процессе; с приложением общается
# через IPsxEngine (Binder) и общую память (api/include/psx_engine_shm.h).
# Список исходников и дефайны зеркалируют Makefile.libretro (таргет arm64,
# DYNAREC=off/M0, BUILTIN_GPU=neon, синхронные GPU/SPU/CD, HAVE_CHD=0).
cmake_minimum_required(VERSION 3.22.1)
project(psxengine LANGUAGES C CXX ASM)

set(PCSX ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pcsx_rearmed)

# генерируем revision.h, который ждёт ядро (frontend/main.c, libpcsxcore/misc.c)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/revision.h
     "#define REV \"psxengine\"\n")

add_library(psxengine SHARED
    # --- склейка с ядром и хост сессии ---
    emu/emu_frontend.c
    emu/emu_saf.c
    host/EngineHost.cpp
    host/engine_jni.cpp

    # --- libpcsxcore (точный список из корневого Makefile) ---
    ${PCSX}/libpcsxcore/cdriso.c
    # cdrom.c ядра — через обёртку с доступом к статусу привода (см. файл)
    emu/cdrom_probe.c
    ${PCSX}/libpcsxcore/cdrom-async.c
    ${PCSX}/libpcsxcore/cheat.c
    ${PCSX}/libpcsxcore/database.c
    ${PCSX}/libpcsxcore/decode_xa.c
    ${PCSX}/libpcsxcore/mdec.c
    ${PCSX}/libpcsxcore/misc.c
    ${PCSX}/libpcsxcore/plugins.c
    ${PCSX}/libpcsxcore/ppf.c
    ${PCSX}/libpcsxcore/psxbios.c
    ${PCSX}/libpcsxcore/psxcommon.c
    ${PCSX}/libpcsxcore/psxcounters.c
    ${PCSX}/libpcsxcore/psxdma.c
    ${PCSX}/libpcsxcore/psxhw.c
    ${PCSX}/libpcsxcore/psxinterpreter.c
    ${PCSX}/libpcsxcore/psxmem.c
    ${PCSX}/libpcsxcore/psxevents.c
    ${PCSX}/libpcsxcore/r3000a.c
    ${PCSX}/libpcsxcore/sio.c
    ${PCSX}/libpcsxcore/spu.c
    ${PCSX}/libpcsxcore/gpu.c
    ${PCSX}/libpcsxcore/pad.c
    ${PCSX}/libpcsxcore/gte.c
    ${PCSX}/libpcsxcore/gte_nf.c
    ${PCSX}/libpcsxcore/gte_divider.c
    ${PCSX}/libpcsxcore/gte_arm64.S
    ${PCSX}/libpcsxcore/gte_nf_arm64.S
    ${PCSX}/libpcsxcore/new_dynarec/emu_if.c
    # dynarec ari64 (arm64)
    ${PCSX}/libpcsxcore/new_dynarec/new_dynarec.c
    ${PCSX}/libpcsxcore/new_dynarec/pcsxmem.c
    ${PCSX}/libpcsxcore/new_dynarec/linkage_arm64.S

    # --- frontend (то, что не зависит от платформы libretro) ---
    ${PCSX}/frontend/main.c
    ${PCSX}/frontend/plugin.c
    ${PCSX}/frontend/cspace.c

    # --- встроенный GPU (gpulib + gpu_neon) ---
    ${PCSX}/plugins/gpulib/gpu.c
    ${PCSX}/plugins/gpulib/vout_pl.c
    ${PCSX}/plugins/gpulib/prim.c
    ${PCSX}/plugins/gpu_neon/psx_gpu_if.c
    ${PCSX}/plugins/gpu_neon/psx_gpu/psx_gpu_simd.c

    # --- rthreads (нужен dfsound в USE_ASYNC_SPU) ---
    ${PCSX}/frontend/pcsxr-threads.c
    emu/features_cpu_shim.c

    # --- встроенный SPU (dfsound) ---
    ${PCSX}/plugins/dfsound/dma.c
    ${PCSX}/plugins/dfsound/freeze.c
    ${PCSX}/plugins/dfsound/registers.c
    ${PCSX}/plugins/dfsound/spu.c
    ${PCSX}/plugins/dfsound/out.c
    ${PCSX}/plugins/dfsound/nullsnd.c

    # --- zlib (miniz, для ppf/cdriso) ---
    ${PCSX}/deps/libchdr/deps/miniz-3.1.2/miniz.c

    # --- libchdr (CHD-образы) ---
    ${PCSX}/deps/libchdr/src/libchdr_bitstream.c
    ${PCSX}/deps/libchdr/src/libchdr_cdrom.c
    ${PCSX}/deps/libchdr/src/libchdr_chd.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_cdfl.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_cdlz.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_cdzl.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_cdzs.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_flac.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_huff.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_lzma.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_zlib.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_zstd.c
    ${PCSX}/deps/libchdr/src/libchdr_flac.c
    ${PCSX}/deps/libchdr/src/libchdr_huffman.c
    ${PCSX}/deps/libchdr/deps/lzma-26.02/src/LzmaDec.c
    ${PCSX}/deps/libchdr/deps/lzma-26.02/Asm/arm64/LzmaDecOpt.S
    ${PCSX}/deps/libchdr/deps/zstd-1.5.7/zstddeclib.c
)

target_include_directories(psxengine PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}          # revision.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/include  # протокол (MIT)
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/emu
    ${PCSX}/include
    ${PCSX}/libpcsxcore
    ${PCSX}/libpcsxcore/new_dynarec
    ${PCSX}/frontend
    ${PCSX}/plugins/gpulib
    ${PCSX}/plugins/gpu_neon
    ${PCSX}/plugins/gpu_neon/psx_gpu
    ${PCSX}/plugins/dfsound
    ${PCSX}/deps/libchdr/deps/miniz-3.1.2
    ${PCSX}/deps/libchdr/include
    ${PCSX}/deps/libchdr/deps/lzma-26.02/include
    ${PCSX}/deps/libretro-common
    ${PCSX}/deps/libretro-common/include
)

# глобальные флаги — зеркало libretro-сборки для arm64
target_compile_options(psxengine PRIVATE
    -O3 -ffast-math -fomit-frame-pointer
    -Wall -Wno-unused-function
    -ffunction-sections -fdata-sections
)

target_compile_definitions(psxengine PRIVATE
    GPU_NEON=1                 # встроенный gpu_neon
    NO_FRONTEND=1              # frontend/main.c без main() и libpicofe
    USE_MINIZ=1                # zlib-символы даёт вшитый miniz
    HAVE_CHD=1                 # CHD-образы через libchdr
    P_HAVE_MMAP=1
    P_HAVE_POSIX_MEMALIGN=1
    DISABLE_MEM_LUTS=0
    _FILE_OFFSET_BITS=64
)

# libchdr: путники к заголовкам lzma
set_source_files_properties(
    ${PCSX}/deps/libchdr/src/libchdr_bitstream.c
    ${PCSX}/deps/libchdr/src/libchdr_cdrom.c
    ${PCSX}/deps/libchdr/src/libchdr_chd.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_cdfl.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_cdlz.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_cdzl.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_cdzs.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_flac.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_huff.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_lzma.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_zlib.c
    ${PCSX}/deps/libchdr/src/libchdr_codec_zstd.c
    ${PCSX}/deps/libchdr/src/libchdr_flac.c
    ${PCSX}/deps/libchdr/src/libchdr_huffman.c
    PROPERTIES COMPILE_OPTIONS "-I${PCSX}/deps/libchdr/deps/lzma-26.02/include")
# arm64-ассемблерный декодер LZMA подключается этим дефайном
set_source_files_properties(${PCSX}/deps/libchdr/deps/lzma-26.02/src/LzmaDec.c
    PROPERTIES COMPILE_DEFINITIONS "Z7_LZMA_DEC_OPT")

# как в Makefile: new_dynarec компилируем с -O2 (меньше объём)
set_source_files_properties(${PCSX}/libpcsxcore/new_dynarec/new_dynarec.c
    PROPERTIES COMPILE_OPTIONS "-O2")

# покомпильные дефайны как в Makefile
set_source_files_properties(${PCSX}/frontend/main.c PROPERTIES COMPILE_DEFINITIONS "BUILTIN_GPU=neon")
# misc.c: HAVE_LIBRETRO исключает его zlib-версию SaveFuncs (наша — в памяти, как в libretro.c)
set_source_files_properties(${PCSX}/libpcsxcore/misc.c PROPERTIES COMPILE_DEFINITIONS "BUILTIN_GPU=neon;HAVE_LIBRETRO")
set_source_files_properties(${PCSX}/plugins/gpu_neon/psx_gpu_if.c PROPERTIES COMPILE_DEFINITIONS "NEON_BUILD;SIMD_BUILD;TEXTURE_CACHE_4BPP;TEXTURE_CACHE_8BPP")
set_source_files_properties(${PCSX}/plugins/gpu_neon/psx_gpu/psx_gpu_simd.c PROPERTIES COMPILE_DEFINITIONS "SIMD_BUILD")
set_source_files_properties(${PCSX}/deps/libchdr/deps/miniz-3.1.2/miniz.c PROPERTIES COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_DEFLATE_APIS;MINIZ_NO_ARCHIVE_APIS")
# dfsound/out.c ждёт фронтендовый аудио-драйвер out_register_libretro — его даёт наш glue
set_source_files_properties(${PCSX}/plugins/dfsound/out.c PROPERTIES COMPILE_DEFINITIONS "HAVE_LIBRETRO")
# async SPU — как в libretro-сборке: сглаживает подачу выборок по циклам,
# убирает щелчки на швах кадров (проверено по PCM-дампам на Fold)
set_source_files_properties(
    ${PCSX}/plugins/dfsound/dma.c
    ${PCSX}/plugins/dfsound/freeze.c
    ${PCSX}/plugins/dfsound/registers.c
    ${PCSX}/plugins/dfsound/spu.c
    PROPERTIES COMPILE_DEFINITIONS "USE_ASYNC_SPU")

find_library(log-lib log)
find_library(android-lib android)
target_link_libraries(psxengine PRIVATE ${log-lib} ${android-lib})

# gc-sections, как в оригинальной сборке; --wrap=fopen: мост SAF-файлов
# 16 КБ страниц (Android 15+): LOAD-сегменты выравниваем на 16384,
# иначе .so не загрузится на 16КБ-устройствах
target_link_options(psxengine PRIVATE
    -Wl,--gc-sections
    -Wl,--wrap=fopen
    -Wl,-z,max-page-size=16384)

# ======================================================================
# Ретро-движок (Денди и др., процесс :retro): наш libretro-хост (MIT) и
# ядра — отдельными .so, хост грузит их dlopen'ом по расширению игры
# ======================================================================
set(FCEUMM ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fceumm)
set(LIBRETRO_COMMON ${FCEUMM}/src/drivers/libretro/libretro-common)

add_library(retroengine SHARED
    retro/LibretroHost.cpp
    retro/RetroEngineHost.cpp
    retro/retro_engine_jni.cpp
)
target_include_directories(retroengine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/retro
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../api/include  # протокол (MIT)
    ${LIBRETRO_COMMON}/include                                  # libretro.h (MIT)
)
target_compile_options(retroengine PRIVATE -O2 -Wall)
target_link_libraries(retroengine PRIVATE ${log-lib} ${android-lib} z dl)
target_link_options(retroengine PRIVATE -Wl,-z,max-page-size=16384)

# --- FCEUmm (GPLv2): Денди / NES / Famicom ---
file(GLOB FCEUMM_BOARDS ${FCEUMM}/src/boards/*.c)
file(GLOB FCEUMM_INPUT ${FCEUMM}/src/input/*.c)
add_library(fceumm_libretro SHARED
    ${FCEUMM_BOARDS}
    ${FCEUMM_INPUT}
    ${FCEUMM}/src/drivers/libretro/libretro.c
    ${FCEUMM}/src/drivers/libretro/libretro_dipswitch.c
    ${FCEUMM}/src/cart.c ${FCEUMM}/src/cheat.c ${FCEUMM}/src/crc32.c
    ${FCEUMM}/src/fceu-endian.c ${FCEUMM}/src/fceu-memory.c ${FCEUMM}/src/fceu.c
    ${FCEUMM}/src/fds.c ${FCEUMM}/src/fds_apu.c ${FCEUMM}/src/file.c
    ${FCEUMM}/src/filter.c ${FCEUMM}/src/general.c ${FCEUMM}/src/input.c
    ${FCEUMM}/src/md5.c ${FCEUMM}/src/nsf.c ${FCEUMM}/src/palette.c
    ${FCEUMM}/src/ppu.c ${FCEUMM}/src/sound.c ${FCEUMM}/src/state.c
    ${FCEUMM}/src/video.c ${FCEUMM}/src/vsuni.c ${FCEUMM}/src/ines.c
    ${FCEUMM}/src/unif.c ${FCEUMM}/src/x6502.c
    ${LIBRETRO_COMMON}/streams/memory_stream.c
    ${LIBRETRO_COMMON}/compat/compat_posix_string.c
    ${LIBRETRO_COMMON}/compat/compat_snprintf.c
    ${LIBRETRO_COMMON}/compat/compat_strcasestr.c
    ${LIBRETRO_COMMON}/compat/compat_strl.c
    ${LIBRETRO_COMMON}/compat/fopen_utf8.c
    ${LIBRETRO_COMMON}/encodings/encoding_utf.c
    ${LIBRETRO_COMMON}/file/file_path.c
    ${LIBRETRO_COMMON}/file/file_path_io.c
    ${LIBRETRO_COMMON}/streams/file_stream.c
    ${LIBRETRO_COMMON}/streams/file_stream_transforms.c
    ${LIBRETRO_COMMON}/string/stdstring.c
    ${LIBRETRO_COMMON}/time/rtime.c
    ${LIBRETRO_COMMON}/vfs/vfs_implementation.c
)
target_include_directories(fceumm_libretro PRIVATE
    ${FCEUMM}/src/drivers/libretro ${LIBRETRO_COMMON}/include
    ${FCEUMM}/src ${FCEUMM}/src/input ${FCEUMM}/src/boards
)
target_compile_definitions(fceumm_libretro PRIVATE
    __LIBRETRO__ PATH_MAX=1024 FCEU_VERSION_NUMERIC=9900 FRONTEND_SUPPORTS_RGB565 PSS_STYLE=1)
target_compile_options(fceumm_libretro PRIVATE -O2 -w)
target_link_options(fceumm_libretro PRIVATE
    -Wl,--version-script=${FCEUMM}/src/drivers/libretro/link.T -Wl,-z,max-page-size=16384)

# ---- Ядро Sega Mega Drive: clownmdemu (AGPLv3, отдельная .so) --------------
# Unity-сборка как у апстрима; CHD (Mega CD) не нужен — без libchdr
set(CLOWNMDEMU ${CMAKE_CURRENT_SOURCE_DIR}/third_party/clownmdemu)
add_library(clownmdemu_libretro SHARED ${CLOWNMDEMU}/unity.c)
target_include_directories(clownmdemu_libretro PRIVATE ${CLOWNMDEMU}/libretro-common/include)
target_compile_definitions(clownmdemu_libretro PRIVATE __LIBRETRO__ CLOWNCD_NO_CHD)
set_target_properties(clownmdemu_libretro PROPERTIES C_VISIBILITY_PRESET hidden)
target_compile_options(clownmdemu_libretro PRIVATE -O2 -w)
target_link_options(clownmdemu_libretro PRIVATE
    -Wl,--version-script=${CLOWNMDEMU}/link.T -Wl,-z,max-page-size=16384)
