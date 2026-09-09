/*
 *  PicoCalc Space Game  -  F5 screenshot implementation
 *  ---------------------------------------------------------------------------
 *  Pulled out of space_game.c so it can be deleted wholesale on hardware that
 *  can't read its framebuffer back or has no file storage - see the recipe in
 *  space_game_screenshot.h.
 *
 *  The vendored lcd driver is write-only, so the GRAM read is done here with
 *  raw spi1 ops (mirroring lcd_write_cmd's CS/DCX handling) at the slower
 *  SHOT_READ_HZ clock.  RAMRD returns 1 dummy byte then 3 bytes/pixel (6-bit
 *  R,G,B in the high bits).  See space_game.h for the format notes and the two
 *  clone-panel fix knobs (SHOT_FIX_INVERT / SHOT_FIX_SWAP_RB).
 */

#include "space_game.h"

#ifdef PICOCALC

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "hardware/spi.h"
#include "hardware/sync.h"

#include "drivers/sdcard.h"
#include "drivers/fat32.h"

#include "space_game_screenshot.h"

static inline void shot_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Read one screen row (WIDTH px) out of GRAM via RAMRD.  dst gets WIDTH*3
 * bytes.  IRQs are held off for the transaction so nothing else can touch
 * spi1 mid-read. */
static void shot_read_row(int y, uint8_t *dst)
{
    const uint8_t caset[4] = { 0, 0, (uint8_t)((WIDTH - 1) >> 8), (uint8_t)((WIDTH - 1) & 0xFF) };
    const uint8_t raset[4] = { (uint8_t)(y >> 8), (uint8_t)(y & 0xFF),
                               (uint8_t)(y >> 8), (uint8_t)(y & 0xFF) };
    uint8_t cmd, dummy;

    uint32_t irqs = save_and_disable_interrupts();

    cmd = LCD_CMD_CASET;
    gpio_put(LCD_DCX, 0); gpio_put(LCD_CSX, 0); spi_write_blocking(LCD_SPI, &cmd, 1); gpio_put(LCD_CSX, 1);
    gpio_put(LCD_DCX, 1); gpio_put(LCD_CSX, 0); spi_write_blocking(LCD_SPI, caset, 4); gpio_put(LCD_CSX, 1);

    cmd = LCD_CMD_RASET;
    gpio_put(LCD_DCX, 0); gpio_put(LCD_CSX, 0); spi_write_blocking(LCD_SPI, &cmd, 1); gpio_put(LCD_CSX, 1);
    gpio_put(LCD_DCX, 1); gpio_put(LCD_CSX, 0); spi_write_blocking(LCD_SPI, raset, 4); gpio_put(LCD_CSX, 1);

    cmd = LCD_CMD_RAMRD;
    gpio_put(LCD_DCX, 0); gpio_put(LCD_CSX, 0); spi_write_blocking(LCD_SPI, &cmd, 1);
    gpio_put(LCD_DCX, 1);                          /* data phase - CS stays low */
    spi_read_blocking(LCD_SPI, 0xFF, &dummy, 1);            /* discard dummy byte */
    spi_read_blocking(LCD_SPI, 0xFF, dst, (size_t)WIDTH * 3);
    gpio_put(LCD_CSX, 1);

    restore_interrupts(irqs);
}

/* Local xorshift32 for the random filename - kept independent of the game's
 * rng so this file has no back-dependency on space_game.c. */
static uint32_t shot_rng(void)
{
    static uint32_t s = 0;
    if (s == 0) s = time_us_32() | 1u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

const char *screenshot_to_sd(void)
{
    static char    result[24];
    static uint8_t gram[WIDTH * 3];       /* one row as the controller returns it */
    static uint8_t rowbuf[WIDTH * 3];     /* one row, 24-bit BGR, for the file    */

    fat32_init();                         /* idempotent: GPIO + hot-plug timer   */
    if (fat32_mount() != FAT32_OK)
        return "No SD card";

    char path[16];
    for (int tries = 0; ; tries++)
    {
        snprintf(path, sizeof path, SHOT_NAME_FMT, (unsigned)(shot_rng() & 0xFFFFu));
        fat32_file_t probe;
        if (fat32_open(&probe, path) != FAT32_OK)
            break;                        /* name is free */
        fat32_close(&probe);
        if (tries >= 24)
            return "SD full of shots";
    }

    fat32_file_t f;
    if (fat32_create(&f, path) != FAT32_OK)
        return "BMP create failed";

    const uint32_t row_bytes = (uint32_t)WIDTH * 3;        /* 4-aligned already */
    const uint32_t pix_bytes = row_bytes * (uint32_t)HEIGHT;
    uint8_t hdr[54] = { 0 };
    hdr[0] = 'B'; hdr[1] = 'M';
    shot_put_u32(hdr + 2,  54 + pix_bytes);                /* file size         */
    shot_put_u32(hdr + 10, 54);                            /* pixel data offset */
    shot_put_u32(hdr + 14, 40);                            /* info header size  */
    shot_put_u32(hdr + 18, (uint32_t)WIDTH);
    shot_put_u32(hdr + 22, (uint32_t)HEIGHT);              /* +ve -> bottom-up  */
    hdr[26] = 1;                                           /* planes            */
    hdr[28] = 24;                                          /* bits per pixel    */
    shot_put_u32(hdr + 34, pix_bytes);
    shot_put_u32(hdr + 38, 2835);                          /* ~72 dpi, x        */
    shot_put_u32(hdr + 42, 2835);                          /* ~72 dpi, y        */

    size_t wr;
    bool ok = (fat32_write(&f, hdr, sizeof hdr, &wr) == FAT32_OK && wr == sizeof hdr);

    spi_set_baudrate(LCD_SPI, SHOT_READ_HZ);               /* slow clock to read */
    spi_set_format(LCD_SPI, 8, 0, 0, SPI_MSB_FIRST);

    for (int i = 0; i < HEIGHT && ok; i++)
    {
        shot_read_row(HEIGHT - 1 - i, gram);              /* BMP row i = bottom-up */
        for (int x = 0; x < WIDTH; x++)
        {
            uint8_t a = gram[x * 3 + 0], g = gram[x * 3 + 1], b = gram[x * 3 + 2];
#if SHOT_FIX_SWAP_RB
            uint8_t t = a; a = b; b = t;
#endif
#if SHOT_FIX_INVERT
            a = (uint8_t)~a; g = (uint8_t)~g; b = (uint8_t)~b;
#endif
            rowbuf[x * 3 + 0] = (uint8_t)((b & 0xFC) | (b >> 6));   /* B */
            rowbuf[x * 3 + 1] = (uint8_t)((g & 0xFC) | (g >> 6));   /* G */
            rowbuf[x * 3 + 2] = (uint8_t)((a & 0xFC) | (a >> 6));   /* R */
        }
        if (fat32_write(&f, rowbuf, row_bytes, &wr) != FAT32_OK || wr != row_bytes)
            ok = false;
    }

    spi_set_baudrate(LCD_SPI, LCD_BAUDRATE);               /* restore fast clock */
    spi_set_format(LCD_SPI, 8, 0, 0, SPI_MSB_FIRST);

    fat32_close(&f);
    if (!ok)
    {
        fat32_delete(path);
        return "BMP write failed";
    }
    snprintf(result, sizeof result, "Saved %s", path);
    return result;
}

#endif /* PICOCALC */
