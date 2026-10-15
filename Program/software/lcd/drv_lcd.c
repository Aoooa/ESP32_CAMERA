/* drv_lcd.c —— ST7789 SPI 面板驱动（esp_lcd，无 LVGL）。见 drv_lcd.h。 */

#include "drv_lcd.h"

#include "bus_spi2.h"

#include "driver/gpio.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "drv_lcd";

#define LCD_PIN_CS   45
#define LCD_PIN_DC   42
#define LCD_PIN_RST  (-1)
#define LCD_PIN_BL   1

#define LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define LCD_CMD_BITS       8
#define LCD_PARAM_BITS     8

static bool                   s_ready;
static esp_lcd_panel_handle_t s_panel;
/* esp_lcd 的 SPI panel IO 有共享事务池（trans_pool/num_trans_inflight），
 * 显示任务与 console 命令都会绘制，故此处串行化。 */
static SemaphoreHandle_t      s_lock;

esp_err_t drv_lcd_init(void)
{
    if (s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /* SPI2 与 SD 共用：只在这里加引用，不重复 spi_bus_initialize */
    esp_err_t err = bus_spi2_acquire();
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = LCD_PIN_CS,
        .dc_gpio_num       = LCD_PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_io_spi 失败：%s", esp_err_to_name(err));
        bus_spi2_release();
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian    = LCD_RGB_DATA_ENDIAN_BIG,   /* 与 strip 缓冲的大端字节序一致 */
    };
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_st7789 失败：%s", esp_err_to_name(err));
        bus_spi2_release();
        return err;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* 240x320 竖屏；摄像头画面左右相反，用面板 MX 位镜像 */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    drv_lcd_backlight(true);

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "ST7789 就绪：%dx%d 竖屏 SPI2 %dHz CS=%d DC=%d BL=%d",
             DRV_LCD_H_RES, DRV_LCD_V_RES, LCD_PIXEL_CLOCK_HZ, LCD_PIN_CS,
             LCD_PIN_DC, LCD_PIN_BL);
    return ESP_OK;
}

bool drv_lcd_ready(void)
{
    return s_ready;
}

esp_err_t drv_lcd_draw_strip(int x0, int y0, int x1, int y1, const void *rgb565_be)
{
    if (!s_ready || !rgb565_be) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, rgb565_be);
    xSemaphoreGive(s_lock);
    return err;
}

void drv_lcd_backlight(bool on)
{
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_PIN_BL,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(LCD_PIN_BL, on ? 1 : 0);
}
