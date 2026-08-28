/*
 * oled.h — tiny SSD1306 driver for the onboard 0.42" OLED on the ESP32-C3
 * boards (72x40, I2C 0x3C, SDA=GPIO5 SCL=GPIO6). Text only, 5x7 font.
 *
 * The panel is a 72x40 window inside the SSD1306's 128x64 memory, so it needs a
 * column offset (OLED_COL_OFFSET) — if the image is shifted, that's the knob.
 */
#ifndef OLED_H
#define OLED_H

#include <stdint.h>
#include <stdbool.h>

#define OLED_COLS   72
#define OLED_PAGES  5          /* 40 rows / 8 */
#define OLED_LINES  OLED_PAGES /* one text line per page (7px glyphs)         */
#define OLED_CHARS  12         /* 72 / 6 px per char                          */

bool oled_init(void);          /* returns false if the panel didn't ACK       */
void oled_clear(void);         /* clear the framebuffer                       */
void oled_str(uint8_t col_px, uint8_t page, const char *s); /* draw into fb   */
void oled_line(uint8_t page, const char *s);   /* clear+draw a whole line     */
void oled_show(void);          /* flush framebuffer to the panel              */

#endif /* OLED_H */
