# Changelog

All notable changes to PicoCalc Space Game.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/);
the version here matches `pico_set_program_version` in `CMakeLists.txt`
and the string shown on the splash screen (`VERSION_STR` in `space_game.h`).

## [Unreleased]

### Added
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
  self-contained `picocalc-space-game-Windows.exe`. `F5` saves a
  `SHOTxxxx.BMP` to the current folder there (`desktop/shim_screenshot.c`).
- Linux build: the `desktop/` build (formerly `windows/`) now also builds on
  Linux and produces `picocalc-space-game-Linux`. SDL2 is linked dynamically there and the
  build copies `libSDL2-2.0.so.0` next to the executable (found through an
  `$ORIGIN` run-path); needs the SDL2 development package (e.g. `libsdl2-dev`).
- The firmware build now names every output with the chip (the `.uf2`, `.elf`, `.bin` and `.hex` all end in `-RP2040` / `-RP2350`) and copies the `.uf2` to the top-level folder as
  `picocalc-space-game-RP2040.uf2` / `picocalc-space-game-RP2350.uf2`, so the
  two chips' builds sit side by side.

## [V1.00A] - 2026-09-08

First public release.
