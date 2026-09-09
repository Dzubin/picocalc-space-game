/*
 *  PicoCalc Space Game  -  F5 screenshot  (OPTIONAL, PicoCalc-only)
 *  ---------------------------------------------------------------------------
 *  Reads the LCD controller's frame memory back over SPI (RAMRD / 0x2E) and
 *  streams it to a randomly-named 24-bit .BMP in the SD-card root.  Fully
 *  self-contained: space_game_screenshot.c + this header + the vendored
 *  drivers/sdcard.c and drivers/fat32.c.
 *
 *  To DROP screenshot support (hardware with no readable framebuffer / no SD):
 *    1. `#undef PICOCALC` (or remove its `#define`) in space_game.h
 *    2. delete space_game_screenshot.c and space_game_screenshot.h
 *    3. remove space_game_screenshot.c - and, if nothing else needs them,
 *       drivers/sdcard.c, drivers/fat32.c and `pico_sync` - from CMakeLists.txt
 *  The F5 glue that stays in space_game.c is all `#ifdef PICOCALC` and compiles
 *  to nothing without it.
 */
#ifndef SPACE_GAME_SCREENSHOT_H
#define SPACE_GAME_SCREENSHOT_H

#ifdef PICOCALC
/* Capture the whole LCD to a fresh random BMP on the SD card.  Returns a short
 * status string to flash on screen ("Saved SHOTxxxx.BMP", "No SD card", ...);
 * it points at static storage, so copy it before the next call. */
const char *screenshot_to_sd(void);
#endif

#endif /* SPACE_GAME_SCREENSHOT_H */
