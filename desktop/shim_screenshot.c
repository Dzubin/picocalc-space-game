/*
 * shim_screenshot.c - Windows replacement for space_game_screenshot.c.
 *
 * The PicoCalc build reads the LCD's frame memory back over SPI and writes a
 * 24-bit .BMP to the SD card. Here the shim already holds the whole picture
 * in shim_framebuffer, so F5 just writes the same kind of BMP (same
 * SHOT_NAME_FMT name) to the current folder. Same function, same status
 * strings, so space_game.c needs no changes.
 *
 * Author: Thomas Dzubin
 */
#include <stdio.h>

#include "shim.h"
#include "space_game.h"
#include "space_game_screenshot.h"

#define BMP_HEADER_BYTES  54
#define BMP_BYTES_PER_PIXEL 3

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

const char *screenshot_to_sd(void)
{
    static char msg[40];
    char name[16];
    uint8_t header[BMP_HEADER_BYTES] = { 'B', 'M' };
    uint8_t row[WIDTH * BMP_BYTES_PER_PIXEL];
    uint32_t image_bytes = (uint32_t)sizeof row * HEIGHT;
    FILE *f;

    snprintf(name, sizeof name, SHOT_NAME_FMT, (unsigned)(time_us_32() & 0xFFFFu));
    f = fopen(name, "wb");
    if (!f) {
        snprintf(msg, sizeof msg, "Can't write file");
        return msg;
    }

    put32(header + 2, BMP_HEADER_BYTES + image_bytes);   /* file size          */
    put32(header + 10, BMP_HEADER_BYTES);                /* pixel data offset  */
    put32(header + 14, 40);                              /* BITMAPINFOHEADER   */
    put32(header + 18, WIDTH);
    put32(header + 22, HEIGHT);                          /* positive = bottom-up */
    put16(header + 26, 1);                               /* planes             */
    put16(header + 28, 24);                              /* bits per pixel     */
    put32(header + 34, image_bytes);
    fwrite(header, 1, sizeof header, f);

    for (int y = HEIGHT - 1; y >= 0; y--) {
        for (int x = 0; x < WIDTH; x++) {
            uint16_t p = shim_framebuffer[y * WIDTH + x];
            uint8_t r = (uint8_t)((p >> 11) & 0x1F);
            uint8_t g = (uint8_t)((p >> 5) & 0x3F);
            uint8_t b = (uint8_t)(p & 0x1F);

            row[x * 3 + 0] = (uint8_t)((b << 3) | (b >> 2));
            row[x * 3 + 1] = (uint8_t)((g << 2) | (g >> 4));
            row[x * 3 + 2] = (uint8_t)((r << 3) | (r >> 2));
        }
        fwrite(row, 1, sizeof row, f);
    }
    fclose(f);
    snprintf(msg, sizeof msg, "Saved %s", name);
    return msg;
}
