/* app_usb_uvc —— UVC 传输适配层。
 *
 * 帧来自 video/app_video（**与传输无关的帧源**）；本文件只做 UVC 特有的事：
 *   ① 把主机协商的 format/分辨率转给帧源；
 *   ② "没人取帧就别送帧"的保护（用组件的完成帧计数，UVC 特有）；
 *   ③ 把帧描述成 uvc_fb_t 交给组件，并把上限校验在**本传输自己的缓冲**上；
 *   ④ 复合设备里那个 CDC 串口（命令 + 日志，见下）。
 *
 * 以后加别的通信方式时：照本文件的形状再写一个适配层即可，帧源不用改。
 *
 * ⚠️ USB-OTG 与 USB-Serial/JTAG 共用 ESP32-S3 的内部 PHY，二者只能用一个：
 *    本工程启用 UVC 后 **PC 上没有 USB-Serial/JTAG 串口**，只有一个复合设备
 *    （相机 + CDC）。要烧录用 console 的 `boot` 命令进 ROM 下载模式。
 *
 * 线程模型：4 个回调都由 usb_device_uvc 的 UVC 任务调用，故帧源（drv_camera）
 *          的全部操作都在那一个任务里，符合它的单线程要求。
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs.h"

#include "usb_device_uvc.h"
#include "tusb.h"

#include "app_usb_uvc.h"
#include "app_video.h"

static const char *TAG = "usb_uvc";

/* 传输缓冲：组件会把每帧 memcpy 进这块缓冲再交给 TinyUSB。
 * 必须是内部 DMA 内存（ESP32-S3 的 USB DMA 到不了 PSRAM）。
 * 这也是"单帧最大可送字节数"的来源 —— 大小上限属于本传输，不属于帧源。 */
#define UVC_BUF_SIZE (75 * 1024)

static bool         s_started;
static camera_fb_t *s_cam_fb;        /* 当前已抓、尚未归还的帧源帧 */
static uvc_fb_t     s_uvc_fb;        /* 交给组件的帧描述（buf 指向 s_cam_fb） */

/* "还有没有人在真正取帧"（UVC 特有）：组件的完成帧计数 + 它上次变化的时刻。
 * 只在主机开过流之后（s_feed_armed）才启用。 */
static uint32_t s_feed_done;
static uint32_t s_feed_done_ms;
static bool     s_feed_armed;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 主机开流：把分辨率交给帧源（传输无关），并复位保护基准 */
static esp_err_t on_start(uvc_format_t format, int width, int height, int rate, void *cb_ctx)
{
    (void)cb_ctx;

    if (format != UVC_FORMAT_JPEG) {
        ESP_LOGE(TAG, "只支持 MJPEG，主机请求 format=%d", (int)format);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = app_video_open(width, height);
    if (err != ESP_OK) {
        return err;
    }

    /* 开流：把判据基准重置到现在。否则下面的保护会拿更早的时刻当基准，
     * 第一帧就被拒 → 永远没有完成帧 → 永久卡死。 */
    s_feed_done    = uvc_device_frame_done_count();
    s_feed_done_ms = now_ms();
    s_feed_armed   = true;

    ESP_LOGI(TAG, "开流 %dx%d @%dfps", width, height, rate);
    return ESP_OK;
}

/* 组件要一帧 */
static uvc_fb_t *on_fb_get(void *cb_ctx)
{
    (void)cb_ctx;

    /* 主机开过流、但完成帧计数长时间不动 → 没人在真正取帧了。
     * 继续送帧会把传输/端点留给下一次会话一个坏状态：现象是"关掉摄像头再打开
     * 永久 0 帧，只有复位才能好"。返回 NULL，组件会跳过本帧、不起传输。
     *
     * 判据必须用完成帧计数，不能用 tud_video_n_streaming()：后者在 bulk 模式下
     * 主机设过一次 alt=1 就锁死为 true（Windows 关掉播放器也不发 SET_INTERFACE(alt=0)）。 */
    uint32_t done = uvc_device_frame_done_count();
    uint32_t now  = now_ms();
    if (done != s_feed_done) {
        s_feed_done    = done;
        s_feed_done_ms = now;
    } else if (s_feed_armed && (now - s_feed_done_ms) >= APP_USB_UVC_FEED_IDLE_MS) {
        return NULL;
    }

    s_cam_fb = app_video_get();          /* 传输无关的取帧 */
    if (!s_cam_fb) {
        return NULL;                     /* 帧源没帧/被关流，组件会跳过 */
    }

    if (s_cam_fb->len > UVC_BUF_SIZE) {  /* 上限属于本传输的缓冲 */
        ESP_LOGW(TAG, "帧 %u 字节 > UVC 缓冲 %d，丢弃", (unsigned)s_cam_fb->len, UVC_BUF_SIZE);
        app_video_release(s_cam_fb);
        s_cam_fb = NULL;
        return NULL;
    }

    s_uvc_fb.buf       = s_cam_fb->buf;
    s_uvc_fb.len       = s_cam_fb->len;
    s_uvc_fb.width     = s_cam_fb->width;
    s_uvc_fb.height    = s_cam_fb->height;
    s_uvc_fb.format    = UVC_FORMAT_JPEG;
    s_uvc_fb.timestamp = s_cam_fb->timestamp;
    return &s_uvc_fb;
}

/* 组件已把帧 memcpy 到自己的传输缓冲（或丢弃）→ 归还给帧源 */
static void on_fb_return(uvc_fb_t *fb, void *cb_ctx)
{
    (void)fb;
    (void)cb_ctx;
    app_video_release(s_cam_fb);
    s_cam_fb = NULL;
}

static void on_stop(void *cb_ctx)
{
    (void)cb_ctx;
    ESP_LOGI(TAG, "停流");
}

/* ---------------- CDC-ACM（复合设备里的串口） ---------------- */

int app_usb_uvc_cdc_getc(void)
{
    if (!tud_cdc_n_connected(0)) {
        return -1;
    }
    uint8_t c;
    if (tud_cdc_n_read(0, &c, 1) == 1) {
        return (int)c;
    }
    return -1;
}

/* ESP_LOG 改写走 CDC。CDC 没连上就丢弃：日志可能在任何上下文里打，绝不能阻塞。 */
static int cdc_log_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0 && tud_cdc_n_connected(0)) {
        uint32_t len = (n < (int)sizeof(buf)) ? (uint32_t)n : (uint32_t)sizeof(buf) - 1;
        tud_cdc_n_write(0, buf, len);
        tud_cdc_n_write_flush(0);
    }
    return n;
}

void app_usb_uvc_log_to_cdc(void)
{
    esp_log_set_vprintf(cdc_log_vprintf);
}

/* 把 CDC 注册成 VFS 设备并把 stdout 指过去。
 * 这样 main 里现有的控制台（printf / con_handle 的全部输出）自动改走 USB CDC，
 * 一行都不用改；配合 _IONBF 让输出立即出现。 */
#define CDC_VFS_PATH "/dev/cdc"

static int cdc_vfs_open(const char *path, int flags, int mode)
{
    (void)path; (void)flags; (void)mode;
    return 0;
}

static ssize_t cdc_vfs_write(int fd, const void *data, size_t size)
{
    (void)fd;
    if (tud_cdc_n_connected(0)) {
        (void)tud_cdc_n_write(0, data, size);   /* 能塞多少塞多少 */
        (void)tud_cdc_n_write_flush(0);
    }
    /* ⚠️ 必须**永远报告"全部写完"**，绝不能返回短计数：
     * newlib/fflush 遇到短写会一直重试，而 esp_restart() 会 flush 控制台 ——
     * 一旦 CDC 发送 FIFO 满（主机没读），重试就变成死循环，命令会卡死在那里。
     * 宁可丢日志，也不能返回短计数。 */
    return (ssize_t)size;
}

static ssize_t cdc_vfs_read(int fd, void *dst, size_t size)
{
    (void)fd;
    if (!tud_cdc_n_connected(0)) {
        return 0;
    }
    return (ssize_t)tud_cdc_n_read(0, dst, size);
}

static int cdc_vfs_close(int fd)
{
    (void)fd;
    return 0;
}

void app_usb_uvc_console_to_cdc(void)
{
    esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open  = cdc_vfs_open,
        .close = cdc_vfs_close,
        .read  = cdc_vfs_read,
        .write = cdc_vfs_write,
    };
    if (esp_vfs_register(CDC_VFS_PATH, &vfs, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_register 失败");
        return;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!freopen(CDC_VFS_PATH, "w", stdout)) {
        ESP_LOGE(TAG, "freopen(%s) 失败：控制台输出仍在 UART0", CDC_VFS_PATH);
    }
}

esp_err_t app_usb_uvc_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *buf = heap_caps_malloc(UVC_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf) {
        ESP_LOGE(TAG, "内部 DMA 内存 %d 字节分配失败（当前最大连续块 %u）",
                 UVC_BUF_SIZE,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }

    uvc_device_config_t config = {
        .uvc_buffer      = buf,
        .uvc_buffer_size = UVC_BUF_SIZE,
        .start_cb        = on_start,
        .fb_get_cb       = on_fb_get,
        .fb_return_cb    = on_fb_return,
        .stop_cb         = on_stop,
        .cb_ctx          = NULL,
    };

    esp_err_t err = uvc_device_config(0, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_device_config 失败：%s", esp_err_to_name(err));
        return err;
    }

    err = uvc_device_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uvc_device_init 失败：%s", esp_err_to_name(err));
        return err;
    }
    s_started = true;

    ESP_LOGI(TAG, "USB UVC 已启动（PC 侧：python scripts/uvc_view.py）");
    return ESP_OK;
}
