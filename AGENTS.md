# AGENTS.md: notes for anyone extending or forking PolyBrik

Read this before changing code, whether you are a person or a coding agent. Each point is something that bit me while building this, with where it lives.

## Layout

- `tracker-ce/`: Polyend Tracker CE, vendored as a git subtree. Treat it as upstream.
- `platforms/sdl/`: everything PolyBrik adds (platform layer, key map, display emulator, presenters, pak, tests). New code goes here.
- `platforms/sdl/PATCHES.md`: every change to `tracker-ce/`, one entry per patch number, with the reason.
- `platforms/sdl/INPUT-MAP.md`: the key map as designed; `platforms/sdl/InputMap.cpp` (`kBrickBindings`) is the data.

## Rules for changing the firmware (`tracker-ce/`)

- Wrap every edit in `#ifdef PLATFORM_SDL` (or put it in an SDL-only file) and log it in `PATCHES.md`. This keeps upstream pulls and cherry-picks merging cleanly. Do not restyle or tidy upstream code you did not need to touch.
- Some upstream files use CRLF line endings. Keep the file's own line endings in the lines you add.
- Apply patch files from the repo root with `git apply --directory=tracker-ce <patch>`. Running `git apply` inside `tracker-ce/` silently applies nothing.
- Never reference anything saved or serialised by index or enum position (lists, entries, bindings). Use stable string ids; indices are only runtime lookups.

## Licenses (read `README.md`, section Licenses)

- `tracker-ce/synth/lib/libPolySynth.a` and `libPolyDSP.a` are proprietary. Never modify, disassemble or inspect their contents. Symbol tables (`nm`, `readelf -A`, `ar t`) are fine. Builds that contain them must stay free.
- The Tracker CE license forbids selling the application or a port of it. Keep forks free.
- The 32-bit binary links alsa-lib and glibc statically (LGPL 2.1): keep the full source and build scripts public.

## Building

- **macOS host build:** Apple clang rejects the firmware (`case SR->GAIN_COL:`). Use Homebrew GCC:
  `cmake -S platforms/sdl -B build-host -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc-16 -DCMAKE_CXX_COMPILER=g++-16 && cmake --build build-host -j`
  The linker is Apple ld64, so GNU ld flags need ld64 equivalents (the CMake file has both).
- **32-bit app (the release):** static armhf binary with the Tracker Mini flags (`-DTRACKER_MINI -DPLATFORM_IMX -DFEATURE_SYNTH -DPLATFORM_SDL`, gnu++2a). Toolchain: GCC 10 with a static glibc 2.28 whose minimum kernel is at or below the Brick's Linux 4.9.191 (a glibc configured for a newer kernel, such as the Homebrew crosstool sysroot's 5.10, will not start there; see the comment in `cmake/t32-armhf.cmake`). Every translation unit uses `-fshort-enums` to match the archives; SDL's enum-size assert is skipped with `DOXYGEN_SHOULD_IGNORE_THIS`. `platforms/sdl/t32/deps.sh` builds static SDL 2.30.8 and alsa-lib 1.1.9; `platforms/sdl/cmake/t32-armhf.cmake` is the toolchain file. The toolchain itself is rebuilt from `platforms/sdl/t32/toolchain/` (crosstool-NG config, two macOS host patches, step-by-step notes).
- **Do not change** `AUDIO_BLOCK_SAMPLES` (128). The audio lock is a recursive mutex. `isCoProEmpty()` is paced per frame, not constant.

## Display

- The firmware draws for an FT812 (EVE) chip; `platforms/sdl/eve/` emulates it into an 800 x 480 canvas. The presenters (`io/raw/FbdevDisplayOut` on the device, `io/sdl/SdlDisplayOut` on desktop) scale it and draw the legend strip.
- **Control drawings are cached** in RAM_G slots (one per control) until the control refreshes. Any rule that hides or changes how a control draws (for example the label suppression in `libs/NewDisplay/label.cpp`) must not change while a page is open, or stale drawings stay on screen.
- SDL builds have 80 control slots (`controlsCount`). The image viewer's picture lives above the slot snapshots (`sdlImageViewerJpegAddress`); move it if you add slots.

## Input and the action list

- Physical buttons map to Mini keys in `InputMap.cpp`, with a second column for "L4 held". Each key keeps the meaning it had when pressed (latched until release), which is what lets L4 be released while an Fn key stays held.
- Platform-only actions (paste, screen scale) are pseudo ids handled in `io/sdl/SdlInput.cpp` and never reach the firmware.
- The R4 action list is `tracker-ce/core/interface/core/sdlActionList.{h,cpp}`. To give a page a list: define `SdlActionEntry` tables in the module's `f*.cpp` (see the Pattern page in `patternEditor/fPatternEdit.cpp`), call `sdlActionList.setList(&list)` in the function that binds the page's screen keys, and give every firmware window that rebinds the screen keys its own list (or `nullptr`). Value entries must call the same function the firmware uses for that value, so clamping and redraw stay the firmware's.
- Labels are at most 10 characters; line 2 is fitted by `fitLine2` (short names table, then the 12 px font, then a measured cut).
- Two-button windows answer to B and X through `sdlSetYesNo` (patch 27). The alias ends when either screen key is rebound; call `sdlClearYesNo()` where a window closes without rebinding.
- `interfaceGlobals.invertValForList` decides whether +1 moves up or down a list. PolyBrik sets it false (patch 45); check it when you add a value entry on a list.

## MIDI and USB audio (`platforms/sdl/midi/`)

- `MidiHub` owns every raw MIDI port (`/dev/snd/midiC*D*`, plus `POLYBRIK_MIDI_DEV` paths for tests) on one detached pthread: hot-plug scan once a second, `select()` on the inputs, timed output. `UsbMidiImpl` / `UartMidiImpl` are thin adapters onto its two buses (USB, and Jack for names in `<sd>/midi-jack`). Channels are 1..16 at this interface, as on the IMX.
- The firmware drains input in `midiUpdate()`, called from `updateAudioBuffers()`, so input handlers run on the audio thread under the audio lock, as they run in the audio ISR on the hardware.
- Output timing: `SdlAudioOut` brackets each rendered block with `MidiHub::beginAudioBlock(playUs)` / `endAudioBlock()`. Sends inside a block are queued until `playUs`; `TimerImpl::scheduleMidiClockUs` adds the firmware's sub-block offset. Sends from any other thread go out immediately. The play time follows a frame counter with a slow anchor, so callback jitter does not reach the clock.
- Do not use `std::thread` in device code: static glibc 2.28 can throw "Enable multithreading" without whole-archive libpthread. `MidiHub` uses `pthread_create`.
- `--sim-clock` (golden runs) and `--no-midi` keep the hub off; the impls are then no-ops and the clock callback runs at once (the `Timer` default), so goldens are unchanged.
- USB audio out is launcher-only: `launch.sh` points `AUDIODEV` at `plughw:<usb card>,<dev>`, and `SdlAudioOut` tries `POLYBRIK_AUDIODEV_FALLBACK` (space separated: ALSA names contain commas) when that fails.
- `midi_test` runs the hub over two raw pseudo-terminals. macOS resets a pty's termios when its last slave fd closes, so the test keeps one open.

## Testing

- Host tests: run the binaries in `build-host/tests/` directly (`actionlist_test`, `input_test`, `legend_test`, `canvas_test`, `eve_test`, `fs_test`, `audio_test`, `png_test`, `midi_test`).
- Golden screenshots: `bash platforms/sdl/golden/replay.sh <outdir>` replays every `.keys` script against a fresh copy of `sdcard/` and compares PNGs. Look at every DIFF before storing a new golden.
- Useful flags: `--no-window --sim-clock --from-ready` (deterministic, timed from the first settled page), `--keys "200:fn2,900:list"` (Mini ids or `pad.<input>`), `--screenshot`, `--screenshot-strip`, `--device brick|brickpro`.
- zsh trap: `"$at:list"` expands `:l` as a modifier. Write `"${at}:list"`.

## On the device (NextUI)

- Never kill `nextui.elf` over SSH; launch paks from the Tools menu.
- `platforms/sdl/devtests/brick-suite.sh [--update] [--os knulli|nextui] [HOST]` runs the screen regression suite (`screens.txt`, goldens in `devtests/golden/`) headless on the device over SSH and diffs every screenshot. Add a case to `screens.txt` for each visual bug you fix.
- `platforms/sdl/devtests/brick-shot.sh 'KEYS' out.png` runs the app headless from `/tmp` and copies a screenshot back, without touching the screen or the user's SD folder. `BIN=path` runs a local build.
- NextUI passes `DEVICE` (`brick` or `brickpro`); the key map and legend follow it.
- The deploy scripts and `brick-shot.sh` reach the device over SSH at `$BRICK_HOST` (default `brick`, an SSH alias or address of your own).
- The device has no `timeout` command, and busybox `cp` has no `-n` (no-overwrite): test shell code on the device.
- The 64-bit binary needs `LD_LIBRARY_PATH=/usr/trimui/lib:/usr/lib`; the 32-bit binary is static.
- Do not overwrite a pak's binary while it runs (`ps | grep tracker-sdl`).

## Factory content (`platforms/sdl/pak/content/`)

- It travels inside the pak; `launch.sh` applies it once per content version (`content.version`, a hash of names and bytes, stamped as `.factory-content` in the SD folder).
- Folders under `content/Samples/` are app-owned and replaced whole on a new version. Everything else (patches, license files) is copied only where missing, because users save their own patches into the same folders.

## Known traps

- The 64-bit build (`PolyBrik64`, without `PLATFORM_IMX`) keeps only the **left** channel of stereo samples and has no synths.
- Patch 22 (hiding the Fn binding group) was reverted; if you hide a Config group again, adjust `baseMenuGroupCount` in `configMenu.cpp`, or an empty slot can crash the Config page.
- Upstream bug found and fixed under SDL (patch 44): after a Delete unused that deletes nothing, the next Delete also removed unused instruments.
