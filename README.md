# AmberStation Engine

Emulation engine of the AmberStation Android app. It runs in separate
processes as Android services; the app talks to it only through Binder
(AIDL) and shared memory, defined in [`api/`](api).

| Service | Process | Systems | Core |
|---|---|---|---|
| `com.leomze.ps1.engine.EngineService` | `:engine` | PlayStation | [PCSX-ReARMed](https://github.com/notaz/pcsx_rearmed) |
| `com.leomze.ps1.engine.RetroEngineService` | `:retro` | NES / Famicom | [FCEUmm](https://github.com/libretro/libretro-fceumm) |
| | | Sega Mega Drive / Genesis | [clownmdemu](https://github.com/Clownacy/clownmdemu-libretro) |

## Layout

- `src/main/cpp/third_party/` — emulator cores, unmodified, as git submodules.
- `src/main/cpp/emu/` — PCSX-ReARMed frontend glue: video, audio, input, CD images, save states, disc swap.
- `src/main/cpp/host/` — PlayStation session: emulation thread with frame pacing, shared memory, JNI.
- `src/main/cpp/retro/` — a minimal libretro frontend: loads a core `.so`, chosen by the ROM extension.
- `src/main/kotlin/` — the Binder services.
- `api/` — the protocol between the app and the engine: `IPsxEngine.aidl` and `include/psx_engine_shm.h`
  (a shared-memory block with a triple-buffered RGB565 frame, a 44100 Hz audio ring, pad state and status).

## Building

Requires JDK 17+, Android SDK (compileSdk 37) and NDK 27.0.12077973.

    git clone --recursive https://github.com/RuslanDemyanov/amberstation-engine.git
    cd amberstation-engine
    ./gradlew assembleRelease

The output is `build/outputs/aar/amberstation-engine-release.aar` with
`libpsxengine.so`, `libretroengine.so` and the core libraries (arm64-v8a).

`--recursive` also fetches clownmdemu's desktop frontend dependencies (SDL, Dear ImGui, FreeType);
they are not used by this build.

## Licenses

- The engine as a whole (PlayStation host and glue, `src/main/cpp/emu`, `src/main/cpp/host`, the services):
  GNU GPL 2.0 or later, see [`LICENSE`](LICENSE).
- The libretro frontend (`src/main/cpp/retro`) and the protocol (`api/`): MIT, see the SPDX headers and [`api/LICENSE`](api/LICENSE).
- Cores keep their own licenses: PCSX-ReARMed — GPL 2.0, FCEUmm — GPL 2.0, clownmdemu — AGPL 3.0.
  Each core is built as a separate shared library.
