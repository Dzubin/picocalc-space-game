# Changelog

All notable changes to PicoCalc Space Game.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/);
the version here matches `pico_set_program_version` in `CMakeLists.txt`
and the string shown on the splash screen (`VERSION_STR` in `space_game.h`).

## [Unreleased]

- Updated the vendored `picocalc-text-starter` to Blair Leduc's code of 2026-10-18 (commit
  `85a002e`) and moved to Pico SDK 2.3.1. The search-hint, deleted-entry and busy-card fixes this
  copy carried are in his code now; four small local fixes remain (README, Third-party). The audio
  driver in it now sounds 20 to 20000 Hz, so the local change that raised its top limit to 2115 Hz
  is gone.
- Vendored starter: `fat32.c` `fat32_write()` no longer leaks a cluster each time a write starts on a
  cluster boundary. The program itself is unchanged; not rebuilt.

- Vendored starter: `fat32.c` skips deleted directory entries before long-name parts (an
  out-of-bounds stack write) and `sdcard.c` waits for a busy card by the clock and before
  every command (failed reads and writes on a card that was still programming). The
  program itself is unchanged; not rebuilt.

## [V1.00B] - 2026-10-02

### Added
- Build outputs are named without the `picocalc-` prefix, since the chip or system
  at the end of the name already says what they are for: `space-game-RP2040.uf2`, `space-game-RP2350.uf2`, `space-game-Windows.exe` and `space-game-Linux`.
- Leaving: `ESC` or `Q` on the start screen now leaves for the PicoCalc UF2
  Loader menu (it asks the loader for its menu through the watchdog scratch
  registers; with no loader installed the program just restarts, and on a PC it
  closes). The start screen says so. `~` still goes to BOOTSEL.
- Vendored `drivers/fat32.c`: `get_next_free_cluster()` now advances its search
  hint past the cluster it hands out (a local fix, not in upstream), so a long
  sequential write to the SD card no longer re-scans every cluster already given
  out and looks hung.
- Desktop build: the build logic that was repeated in `desktop/CMakeLists.txt` now
  lives in `desktop/shim_desktop.cmake` (that file now only names the program and
  its sources), and the shim files were refreshed from a shared copy. The numeric
  keypad now works in the desktop build.
- Windows build: `desktop/` holds a small SDL2 shim that re-implements the
  picocalc-text-starter driver API (LCD, keyboard, audio) so the same
  `space_game.c` runs on a PC; SDL2 is linked statically, giving one
  self-contained `space-game-Windows.exe`. `F5` saves a
  `SHOTxxxx.BMP` to the current folder there (`desktop/shim_screenshot.c`).
- Linux build: the `desktop/` build (formerly `windows/`) now also builds on
  Linux and produces `space-game-Linux`. SDL2 is linked dynamically there and the
  build copies `libSDL2-2.0.so.0` next to the executable (found through an
  `$ORIGIN` run-path); needs the SDL2 development package (e.g. `libsdl2-dev`).
- The firmware build now names every output with the chip (the `.uf2`, `.elf`, `.bin` and `.hex` all end in `-RP2040` / `-RP2350`) and copies the `.uf2` to the top-level folder as
  `space-game-RP2040.uf2` / `space-game-RP2350.uf2`, so the
  two chips' builds sit side by side.

## [V1.00A] - 2026-09-08

First public release.
