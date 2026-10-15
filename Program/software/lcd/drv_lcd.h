#ifndef DRV_LCD_H
#define DRV_LCD_H

/* drv_lcd —— ST7789 SPI 面板（240x320 竖屏，esp_lcd，无 LVGL）。
 * 引脚：SCLK=IO39 MOSI=IO38 MISO=IO40（与 SD 共用 SPI2）、CS=IO45、DC=IO42、
 *       RST=-1、背光=IO1。
 * 初始化：swap_xy(false) + mirror(true,false) + invert_color(true)，80MHz。
 * 像素为 RGB565 大端（高字节在前）。 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define DRV_LCD_H_RES 240
#define DRV_LCD_V_RES 320

/* 初始化总线/面板/背光。重复调用返回 ESP_ERR_INVALID_STATE。 */
esp_err_t drv_lcd_init(void);

bool drv_lcd_ready(void);

/* 贴一块 RGB565（大端）位图到 (x0,y0)-(x1,y1)（右下开区间）。
 * 缓冲区的回收由下一次调用的参数阶段保证，故调用方双缓冲轮换即可。 */
esp_err_t drv_lcd_draw_strip(int x0, int y0, int x1, int y1, const void *rgb565_be);

void drv_lcd_backlight(bool on);

#endif /* DRV_LCD_H */
