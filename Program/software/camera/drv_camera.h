#ifndef DRV_CAMERA_H
#define DRV_CAMERA_H

/* drv_camera —— OV5640（DVP 并口）摄像头硬件封装：只做 esp32-camera 的板级参数 + 薄封装。
 *
 * 板级：Waveshare ESP32-S3-Touch-LCD-2 的 24pin FPC 摄像头座。
 * 引脚表来源（两处官方例程互相印证，非推测）：
 *   示例程序/ESP32-S3-Touch-LCD-2-Demo/ESP-IDF/05_lvgl_camera/main/main.c
 *   示例程序/ESP32-S3-Touch-LCD-2-Demo/Arduino/examples/10_camera_web_server/10_camera_web_server.ino
 *
 *   PWDN=IO17   RESET=-1(做软复位)   XCLK=IO8   SIOD(SDA)=IO21   SIOC(SCL)=IO16
 *   D0..D7 = IO12,IO13,IO15,IO11,IO14,IO10,IO7,IO2
 *   VSYNC=IO6   HREF=IO4   PCLK=IO9
 *
 * 引脚冲突（决定本工程形态的关键事实）：
 *   IO2/IO4 = Embedded_debug_tool 的 UART1（XFR 高速口），IO16/IO17 = UART2，
 *   IO40 = LCD 的 MISO。摄像头必须用 IO2/IO4/IO16/IO17，所以**本工程不初始化
 *   UART1/UART2/LCD/触摸**。SD 卡走 SPI2(IO38/39/40 + CS=IO41)，与摄像头不冲突。
 *
 * 输出格式：PIXFORMAT_JPEG —— OV5640 内部 JPEG 编码器直接把 YUV 压成 JPEG 码流，
 *   DVP 上跑的是压缩数据，不占 CPU（esp32-camera 的 sensor.c 里 OV5640 support_jpeg=true）。
 *   RGB565 需要 LCD_CAM 转换 + CPU 编码，本测试工程不走那条路。
 *
 * 线程模型：非线程安全。全部接口须由**同一个任务**串行调用（本工程 = console 任务）；
 *   esp_camera_fb_get() 之外不要在其他任务里抓帧，否则帧缓冲所有权会乱。
 */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_camera.h"

/* ── 板级引脚（见文件头来源说明） ── */
#define DRV_CAMERA_PIN_PWDN    17
#define DRV_CAMERA_PIN_RESET   (-1)
#define DRV_CAMERA_PIN_XCLK     8
#define DRV_CAMERA_PIN_SIOD    21
#define DRV_CAMERA_PIN_SIOC    16
#define DRV_CAMERA_PIN_D0      12
#define DRV_CAMERA_PIN_D1      13
#define DRV_CAMERA_PIN_D2      15
#define DRV_CAMERA_PIN_D3      11
#define DRV_CAMERA_PIN_D4      14
#define DRV_CAMERA_PIN_D5      10
#define DRV_CAMERA_PIN_D6       7
#define DRV_CAMERA_PIN_D7       2
#define DRV_CAMERA_PIN_VSYNC    6
#define DRV_CAMERA_PIN_HREF     4
#define DRV_CAMERA_PIN_PCLK     9

#define DRV_CAMERA_XCLK_HZ 20000000   /* 与厂商例程一致的 20MHz 主时钟 */

/* 用户侧配置（与 esp32-camera 的 camera_config_t 解耦，只暴露本测试需要的项） */
typedef struct {
    framesize_t framesize;    /* FRAMESIZE_*，见 drv_camera_framesize_parse() */
    int         jpeg_quality; /* 0-63，越小越清晰（OV5640 内部编码的量化档） */
    size_t      fb_count;     /* PSRAM 帧缓冲个数：2 = 可边抓边存不丢帧 */
} drv_camera_cfg_t;

/* 默认参数：VGA(640x480) JPEG quality 12，双帧缓冲 */
void drv_camera_default_cfg(drv_camera_cfg_t *cfg);

/* 初始化摄像头（esp_camera_init）。重复调用返回 ESP_ERR_INVALID_STATE。 */
esp_err_t drv_camera_open(const drv_camera_cfg_t *cfg);

/* 改分辨率：内部走 esp_camera_reconfigure（= deinit+init，重建帧缓冲）。
 * 注意：重配置会把 vflip/hmirror/colorbar 恢复到寄存器默认值，
 * 调用方需自行重设（app_camera 已记住并自动重放）。 */
esp_err_t drv_camera_set_framesize(framesize_t fs);

/* 改 JPEG 质量：直接写传感器寄存器，不重建帧缓冲。0-63，越小越清晰。 */
esp_err_t drv_camera_set_quality(int quality);

/* 关闭摄像头（esp_camera_deinit）。未打开时返回 ESP_ERR_INVALID_STATE。 */
esp_err_t drv_camera_close(void);

/* 取一帧（阻塞直到有帧；超时/坏帧返回 NULL）。用完必须 drv_camera_release()。 */
camera_fb_t *drv_camera_capture(void);

/* 归还帧缓冲（fb 为 drv_camera_capture 返回值，NULL 安全） */
void drv_camera_release(camera_fb_t *fb);

/* 传感器对象（用于 set_hmirror/set_vflip/set_colorbar/set_reg 等）。未打开返回 NULL。 */
sensor_t *drv_camera_sensor(void);

/* 当前生效的配置（只读；未打开时返回默认值） */
const drv_camera_cfg_t *drv_camera_cfg(void);

/* 摄像头是否已就绪 */
bool drv_camera_ready(void);

/* 分辨率 → "VGA" 这类短名（未知返回 "?"） */
const char *drv_camera_framesize_str(framesize_t fs);

/* "vga"/"640x480" 等 → framesize_t；不识别返回 false（大小写不敏感） */
bool drv_camera_framesize_parse(const char *name, framesize_t *out);

#endif /* DRV_CAMERA_H */
