# Mupen64Plus PS5

mupen64plus-core 2.6.0, the Nintendo 64 emulator, as a **native home-screen app** for a jailbroken PS5: a game
shelf with covers, DualSense support and save states. The release is the app folder itself (`eboot.bin` and
its files): no payload to send.

- `../mupen64plus-core` is the original core source, **unmodified**.
- Everything that makes it a PS5 app is in this folder:
  - `n64/`: the core's PS5 side and its built-in plugins;
  - `coreorbis/`: the PS5 layer;
  - `frontend/`: the shelf and menus;
  - `proto/native/`: the eboot builder and signer;
  - `installer/`: the helper the app carries, and the optional installer payload.

The PS5 layer and the build come from **Snes9x PS5** (`../../snes9xPS5-2.0/ps5`), which follows the PS5SX2
model.

> **0.6.3:** in-app updates work. Up to 0.6.2 the updater wrote the new files without execute permission, so
> the console refused to start the updated app ("Can't start the game or app", CE-107750-0). 0.6.3 unpacks the
> update next to the app folder with the permissions an FTP copy gets, closes, and its helper swaps the folders
> so ShadowMountPlus installs the app again. The release zip is now `Mupen64PlusPS5.zip`, so 0.6.0–0.6.2 never
> offer an update they would break: install 0.6.3 by hand once.
>
> **0.6:** released as the app folder (`Mupen64PlusPS5.zip`): copy `PPSA99064/` to
> `/data/homebrew/` or a USB drive's `homebrew/`, and ShadowMountPlus lists it. No `.elf` to send: eboot.bin
> starts the helper it carries through the ELF loader by itself, and now prefers it over etaHEN's daemon.
> The app updates itself from the GitHub releases ([Updates](#updates)).
>
> **0.5.2:** rumble: the DualSense is switched to its compatible vibration mode (its default haptics mode
> ignores the classic rumble call); boot.log now shows the Rumble Pak commands, the vibration calls, and each
> game's save files when it starts and stops.
>
> **0.5.1:** fixes a crash when a second game (or the same one again) is started in the same session with the
> dynarec: the code cache lost its execute permission when the first game ended.
>
> **Status (0.5):** the dynarec runs on the console (`mprotect RWX works`): GoldenEye, Rogue Squadron and
> Donkey Kong 64 hold 59-61 VI/s. 0.5 lowers the audio latency from ~120 ms to ~67 ms (rate control with an
> integral term, and a target that follows the game's buffer size, up to 125 ms for games with big buffers).
>
> **0.4:** runs on the console (Super Mario 64 at full speed; GoldenEye dipped to 75–90% with the
> cached interpreter, where the CPU took ~50% of the time and the RSP ~30%). 0.4 adds the **x86-64 dynarec**:
> on the host it cuts the CPU's share from ~30% to ~3%. Whether the PS5 lets the app run generated code is
> tested at start-up (`[emu] dynarec:` in `boot.log`); if not, the cached interpreter runs as before.
>
> **0.3:** the per-part timing of the FPS counter uses the CPU's cycle counter (0.2's clock_gettime, a system
> call thousands of times per frame, slowed games down).
>
> **0.1:** it builds (`make ps5`), and the same code runs in the Linux host build with the PS5 calls
> simulated. There, F-Zero X (USA) boots, plays (menus, machine select, a race), saves its SRAM, makes and
> loads save states, and quits cleanly, all at full speed. The included test ROM checks the CPU, RDP, VI and
> audio paths. **It has not run on a PS5 yet.** If something fails there, `/data/mupen64plus/logs/boot.log`
> says where.

## How the emulator is put together

mupen64plus is a core plus four plugins (RSP, video, audio, input). On the desktop these are separate
libraries; here everything is linked into `eboot.bin`. `n64/core/osal_dynlib_static.c` replaces `dlopen`/`dlsym`
with tables of functions looked up by name.

| Part | What runs it on the PS5 |
|---|---|
| CPU (VR4300) | mupen64plus-core's **x86-64 new dynarec** when the console gives executable memory (`n64/core/jit_ps5.c`), else its cached interpreter |
| RSP | **mupen64plus-rsp-hle** for audio, MP3 and JPEG tasks (`n64/plugins/rsp_hle_ps5.c`), **mupen64plus-rsp-cxd4** for the rest: low level, it runs the game's own microcode, graphics included (`n64/plugins/rsp_cxd4_ps5.c`) |
| RDP + VI | **angrylion-rdp-plus**: software, pixel-accurate, 6 render threads (`n64/plugins/gfx_ps5.c`) |
| Audio | `n64/plugins/audio_ps5.cpp`: AI samples resampled to 48 kHz, into `libSceAudioOut` |
| Input | `n64/plugins/input_ps5.cpp`: `libScePad`, rumble through `scePadSetVibration` |
| Picture | `ProsperoVideo`: angrylion's frame scaled (4:3 / integer / stretch, sharp or smooth) and tiled into `libSceVideoOut` |

angrylion renders on the CPU at the N64's own resolution, which the PS5's eight Zen 2 cores can afford.

A GPU renderer, paraLLEl-RDP with a higher internal resolution, is being worked on (GitHub issue #1). PS5 homebrew
now has a Vulkan driver: RADV, Mesa's driver for AMD GPUs, as mihawk-99/PS5_Vulkan builds it for the console.
`make native VULKAN=1` (or `build-native.bat Vulkan`) links it into a test build, in `build-native-vulkan/`, that
runs a Vulkan self-test at start (`coreorbis/orbis-shims/ProsperoVulkan.h`). It needs PS5_Vulkan checked out with
its RADV archive built (`PS5_VULKAN`, default `/root/gpu/PS5_Vulkan`: `tools/setup-native-dependencies.sh`, then
`tools/build-radv.sh release`). On the console (2026-10-07) the self-test passed every check: the device, a compute
shader, the GPU writing memory the app allocated as it allocates RDRAM, and every feature paraLLEl-RDP uses; Super
Mario 64 then ran at full speed in the same build.

**What the core needs from SDL, libpng and dlopen is replaced:**

- `n64/sdl/` provides threads, mutexes and the clock on pthreads.
- `n64/core/vidext_ps5.c`, `eventloop_ps5.c` and `screenshot_ps5.c` stand in for the files that need a real
  SDL, OpenGL or libpng.

**Frame pacing** (`frontend/fe_emu.cpp`):

- The game runs inside `CoreDoCommand(M64CMD_EXECUTE)`, and once per video interrupt the video plugin hands
  over the picture.
- A presenter thread scales and flips it while the next frame is emulated.
- NTSC games are paced by the TV's 60 Hz flips (the core's speed limiter is off). The audio plugin stretches
  the sound by up to 0.5% to stay in step.
- PAL games keep the core's limiter.

## Requirements

- A jailbroken PS5 with **kstuff** (or kstuff-lite) and an **ELF loader** on port 9021 (elfldr, etaHEN, or the
  one PS5 Payload Manager uses).
- **ShadowMountPlus**, so the icon appears on the home screen.
- Your own N64 ROMs (`.z64 .n64 .v64 .rom`, or zipped). No games are included.

## Install and play

1. **Copy the `PPSA99064` folder** from `Mupen64PlusPS5.zip` (the folder itself, not just its
   contents) to one of:
   - `/data/homebrew/PPSA99064` on the console (FTP, PS5Upload...);
   - `homebrew/PPSA99064` on an exFAT USB drive.

   ShadowMountPlus adds the icon to the home screen within its next scan (15 s by default). To update, close
   the app and copy the new folder over the old one.
2. **Open the Mupen64Plus PS5 icon.**
   - The app sends the helper built into eboot.bin to the ELF loader (127.0.0.1:9021). The helper listens on
     127.0.0.1:9064, lets only PPSA99064 out of its sandbox (`/data`, USB drives, executable memory for the
     dynarec), and stays running until the console is turned off.
   - If no ELF loader answers, the app asks etaHEN (9028) or a 9069 jailbreak daemon instead.
   - Optional: `Mupen64PS5-v<version>.elf`, sent to port 9021, installs the same folder and starts the helper
     (the way before 0.6).
3. **Copy your ROMs** to `/data/mupen64plus/roms` (over FTP, for example), or to a `mupen64plus/roms` folder on
   a USB drive. Subfolders work.

Title ID `PPSA99064` and the folders below don't overlap with Snes9x PS5 (PPSA99009) or PS5SX2 (PPSA99203).

| Folder | Contents |
|---|---|
| `/data/mupen64plus/roms` | your games |
| `/data/mupen64plus/saves` | in-game saves (EEPROM, SRAM, FlashRAM, Controller Pak), named `<GoodName>-<MD5>` by the core |
| `/data/mupen64plus/states` | save states, `<rom file>.st0` to `.st9` |
| `/data/mupen64plus/config` | `mupen64plus.cfg`, the core's own settings (advanced) |
| `/data/mupen64plus/data` | `mupen64plus.ini`, the core's ROM catalog (save types, per-game fixes), written by the app |
| `/data/mupen64plus/covers` | downloaded covers and your own (`<rom file name>.png` / `.jpg`) |
| `/data/mupen64plus/logs` | `boot.log`, `installer.log`, `helper.log` and the previous session's `.prev.log` |
| `/data/mupen64plus/mupen64plus-ps5.ini` | the menu settings |

## Controls

**In a game** (buttons by position, as on the N64 pad)

| PS5 | N64 |
|---|---|
| left stick | analog stick (dead zone adjustable) |
| D-pad | D-pad |
| Cross / Square | A / B |
| L2 or R2 | Z |
| L1 / R1 | L / R |
| right stick | C buttons |
| Triangle / Circle | C-up / C-down |
| OPTIONS | Start |
| **touchpad click** or **L3 + R3** | pause menu |

**The pause menu** has:

- resume, save state, load state, and the state slot (0–9);
- speed (normal or fast forward);
- every setting below;
- reset, back to the game list, and quit.

**Settings** (marked `*` when they apply from the next game start):

| Setting | |
|---|---|
| Aspect ratio | 4:3 (default), integer scale, stretch 16:9 |
| Smooth scaling | bilinear (on) or sharp pixels |
| N64 video filter * | the N64's own VI filter (anti-aliasing, dither filter) |
| Hide overscan * | crops the black border the N64 draws around the picture |
| CPU core * | Dynarec (default, when the console allows it) or Interpreter (slower; for a game the dynarec gets wrong) |
| Audio processing * | Fast (HLE, default): rsp-hle does the RSP's audio work; Accurate (LLE): cxd4 runs the game's audio microcode (slower; for a game whose sound HLE gets wrong) |
| Render threads * | angrylion workers: 1–12, default 6 |
| Controller pak * | Controller Pak (default), Rumble Pak, none |
| Stick dead zone | 0–30% |
| Show FPS | the speed (`VI/s`, 100% = full speed) and, below it, where the emulation thread's time goes: `cpu` (VR4300 interpreter), `rsp` (rsp-hle + cxd4), `rdp` (angrylion's rasteriser, incl. waiting for its threads), `vi` (its VI filter), `idle` (waiting for the TV: spare time). Also logged every 10 s in `boot.log` |
| Sound, Download covers | |
| Check for updates | ask at start-up when a newer GitHub release exists (see [Updates](#updates)) |

**On the shelf:**

- Left/Right: choose a game
- Cross: play
- Triangle: settings
- Square: fetch the cover again
- OPTIONS: quit

## Updates

At every start, before it asks for `/data` (the console's HTTPS works only then, as for the covers), the app asks
`api.github.com` for the latest release of `TheRealRetro/mupen64plus-ps5` (`GITHUB_REPO` in the Makefile).

- **When the release's tag is newer** than the app's `contentVersion`, it downloads the release's
  `Mupen64PlusPS5.zip` and checks its size and SHA-256 against the release (`frontend/fe_update.cpp`).
- **Once `/data` is visible,** it shows "Mupen64Plus PS5 X is available", with the release notes:
  - ✕ unpacks the new version next to each copy of the app folder it finds (`/data/homebrew/PPSA99064`, a USB
    drive's `homebrew/PPSA99064`...), in `.mupen64plus-update/new/`, with the permissions an FTP copy gets
    (0777: the console doesn't start an `eboot.bin` without execute permission), writes a job file, starts its
    helper through the ELF loader and closes. The helper waits for the app to exit, moves the app folder aside,
    asks ShadowMountPlus to rescan (its API, 127.0.0.1:10101) until it has dropped the app, moves the new
    version in, rescans until it is installed again, and says so in a notification
    (`coreorbis/orbis-shims/ProsperoUpdateJob.cpp`; its steps go to `logs/helper.log`);
  - ○ carries on (asked again next start).
- **A release that was installed but didn't take** (the next start still ran an older eboot) isn't offered
  again (`/data/mupen64plus/update/installed.txt`).
- **Settings → Check for updates: No** skips the question.
- **An app running from a `.ffpfsc` image** can't be updated this way (read-only): copy the new folder by hand.
- **`boot.log` shows every step** (`[update]` lines).

## Game names and covers

- **Names:** a ROM's name comes from its file name when that is already a No-Intro name ("F-Zero X
  (USA).z64"). Otherwise it comes from the CRC in its cartridge header, looked up in a table built from the
  core's `mupen64plus.ini` (1891 games, `tools/make_n64db.py`).
- **Covers:** they come from libretro-thumbnails (Nintendo 64, `Named_Boxarts`). The source works exactly as
  in Snes9x PS5 (a prefetch at start-up, then a restart for new covers).
- **When a cover doesn't download:** the catalog's names are GoodN64 names converted to No-Intro style, and
  some differ from libretro's in capitalisation, which the download URL is sensitive to. Name the ROM file the
  No-Intro way, or put your own `.png` in `covers/`.

## Known limitations

- **The dynarec on the console is new in 0.4.** The app tests at start-up whether it may run generated code:
  first `mprotect`, then executable direct memory mapped over the code cache (what RetroArch's PS5 port uses
  for its recompilers). `boot.log` says which worked. If a game misbehaves with it, set CPU core to
  Interpreter.
- **The RSP is the next bottleneck.** cxd4 interprets every graphics microcode instruction (GoldenEye:
  ~30% of the time); audio goes through rsp-hle (Audio processing: Fast).
- **Unsupported:** 64DD, the Transfer Pak, screenshots, a cheats menu, netplay.
- PAL games are paced by the core's millisecond limiter, so motion is less smooth than NTSC.

## Building

Requirements:

- the [ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk), v0.42 or newer;
- clang and lld 18, g++, and python3 (plus Pillow and numpy only to redraw the art).

zlib is vendored.

From Windows, as the Homebrew Browser's build (WSL Ubuntu-24.04, the SDK of `/root/ps5-native`):

```
build-native.bat              app folder + zip
build-native.bat Ffpfsc       also a compressed .ffpfsc image
```

Output in `build-native/` (next to `ps5/`):

- `PPSA99064/`: the app folder to copy to `/data/homebrew/`.
- `Mupen64PlusPS5.zip`: the release artifact (just the folder). Its tag is the `contentVersion`, made from
  `VERSION` in the Makefile (0.6.0 -> `00.006.000`); a copy is kept in `release-assets/<contentVersion>/`.
- `PPSA99064.debug.elf`: the program with its symbols, for reading crash reports.
- `PPSA99064.ffpfsc`: with `Ffpfsc`, a compressed image ShadowMountPlus can mount.

By hand:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cd ps5
make native -j$(nproc)           # ../build-native/ as above (FORMAT=Ffpfsc for the image)
make ps5 -j$(nproc)              # build/ps5/Mupen64PS5.elf (installer + helper, with the app inside)
make send PS5_HOST=192.168.0.10  # sends it to the ELF loader (port 9021)
make dist                        # make native + build/dist/Mupen64PS5-v<version>.elf (optional installer)
                                 # + the source zip
make app                         # only build/app/PPSA99064/, to copy by hand
make host                        # Linux build: build/host/mupen64plus-ps5-host [rom]
```

**Host build and test ROM.** `tests/make_test_rom.py out.z64` writes a small N64 program that contains no
Nintendo code. It draws colour bars, sends the RDP a fill rectangle and plays a 440 Hz tone. Run it, or any
ROM, on Linux:

```sh
N64PS5_PS5_ROOT=/tmp/n64 N64PS5_HOST_DUMP=150 N64PS5_HOST_MAX_FLIPS=200 build/host/mupen64plus-ps5-host test.z64
```

The run writes flip 150 to a `.ppm` file. `N64PS5_HOST_PAD="frame:buttons;..."` scripts the pad (see
`host/sce_host.cpp`).

**Dynarec.** `new_dynarec.c` is built with `-Dmprotect=n64ps5_jit_mprotect` (the core stays unmodified),
its NASM stubs are translated to GNU syntax in `n64/core/linkage_x64.S`, and the struct offsets they need are
generated from the core's `asm_defines.c` with its own `tools/gen_asm_script.sh`. On the host,
`N64PS5_JIT=direct` exercises the PS5's direct-memory way (an anonymous RWX mapping stands in for it).

**Art.** `tools/make_art.py app/sce_sys` redraws `icon0.png`, `background-source.png` and the BC7
`pic0.dds`/`pic1.dds`.

## Source layout

- **`n64/plugins/`**: the four plugins.
  - `rsp_cxd4_ps5.c` builds cxd4 as one unit with its exports renamed; `rsp_hle_ps5.c` puts rsp-hle's
    core in front of it (what rsp-hle doesn't know, graphics above all, goes to cxd4).
  - `gfx_ps5.c` is angrylion's plugin glue without OpenGL.
  - `audio_ps5.cpp` and `input_ps5.cpp`.
- **`n64/core/`**: the core's PS5 side.
  - the static `osal_dynlib` and the core's own function table;
  - `vidext`, `eventloop` and `screenshot` replacements.
- **`n64/sdl/`**: the bit of SDL2 the core uses, on pthreads.
- **`n64/n64_bridge.h`**: what the plugins and the frontend say to each other.
- **`frontend/fe_emu.cpp`**: core start-up, ROM loading (zip included), plugin attach, the per-VI hook, the
  presenter thread and save states.
- **`frontend/`** (everything else): the shelf, covers, the game library (`fe_games.cpp`, N64 header CRCs)
  and the menus. These come from Snes9x PS5.
- **`coreorbis/`**: the PS5 layer from Snes9x PS5: VideoOut (with the N64 scaler), AudioOut, pads (with
  rumble), the sandbox request, install, crash log, notifications and paths.
- **`third_party/`**: angrylion-rdp-plus (`9c8b9ed`), mupen64plus-rsp-cxd4 (`00906a9`) and
  mupen64plus-rsp-hle (2.6.0, `src/` unmodified, minus `plugin.c` and the dlopen osal), plus zlib. Two
  changes are marked `M64PS5_*`:
  - cxd4 doesn't probe RDRAM with SIGSEGV;
  - angrylion's workers get 1 MiB stacks.

## License and credits

- **mupen64plus-core**: GPL-2.0-or-later. The app as a whole is therefore distributed under the GPL (v3,
  because of the native tooling below).
- **angrylion-rdp-plus**: MAME license (non-commercial). `gfx_ps5.c` replaces its GPL-2.0+ mupen64plus glue.
- **mupen64plus-rsp-cxd4**: CC0.
- **mupen64plus-rsp-hle**: GPL-2.0-or-later (`rsp_hle_ps5.c`, which builds on it, too).
- **New code in `ps5/n64`** and the PS5 layer: MIT.
- **Snes9x PS5** (github.com/MisterTemaki): the PS5 layer, the frontend and the build. **PS5SX2** (Spyros): the
  model they follow.
- **ps5-payload-dev SDK** (John Törnblom, GPLv3+) and **ps5-native-app-boilerplate** (BlackBearReloaded,
  GPL-3.0-or-later): the toolchain, `ps5-native-tool`, `app_crt.cpp` and the `libc.prx` generator.
- **zlib**, **stb**, and the UI fonts Roboto, PromptFont and Font Awesome Brands: see `frontend/assets/fonts/`.
- **Covers**: libretro-thumbnails, downloaded on the console, not included.
