#ifndef APP_LCD_H
#define APP_LCD_H

/* app_lcd —— LCD 消费者：frame_bus 取最新一帧 JPEG → ROM TJpgDec 解码 → 条带贴屏。
 * 摄像头帧水平居中裁剪到 240 宽、垂直居中：QVGA 显示为 240x240，上下各 40 行黑边。 */

#include <stdbool.h>

#include "esp_err.h"

/* 初始化面板 + 起显示任务。重复调用返回 ESP_ERR_INVALID_STATE。 */
esp_err_t app_lcd_start(void);

/* 显示开关：off 时任务不再解码，面板保持点亮。 */
void app_lcd_set_enabled(bool on);
bool app_lcd_enabled(void);

void app_lcd_report(void);

/* 全屏纯色：color = "r"/"g"/"b"/"w"/"k"；无参则依次红→绿→蓝→白→黑各 800ms。
 * 执行时暂停显示任务（图案保持可见），恢复用 `lcd on`。 */
void app_lcd_fill(const char *color);

/* 4 条横带，自上而下 红/绿/蓝/白。 */
void app_lcd_test_pattern(void);

/* 全屏动画刷 n 帧（<=0 取 200），报帧率与带宽。 */
void app_lcd_bench(int frames);

#endif /* APP_LCD_H */
