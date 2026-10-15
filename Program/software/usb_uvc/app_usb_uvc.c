/* app_usb_uvc —— UVC 传输适配层：帧取自 frame_bus，另含复合设备里的 CDC 串口
 * （命令 / 日志 / stdout 重定向）。4 个回调均由 usb_device_uvc 的 UVC 任务调用。 */

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
#include "frame_bus.h"

static const char *TAG = "usb_uvc";

/* 传输缓冲：组件把每帧 memcpy 进来再交 TinyUSB；必须是内部 DMA 内存 */
#define UVC_BUF_SIZE (75 * 1024)

static bool     s_started;
static int      s_slot = -1;         /* 当前已取、尚未归还的帧总线槽位 */
static uint32_t s_last_seq;          /* 上次交给主机的帧序号 */
static uvc_fb_t s_uvc_fb;

/* 组件的完成帧计数 + 它上次变化的时刻，只在主机开过流后启用 */
static uint32_t s_feed_done;
static uint32_t s_feed_done_ms;
static bool     s_feed_armed;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* 主机在取流吗 */
bool app_usb_uvc_host_active(void)
{
    return s_feed_armed && (now_ms() - s_feed_done_ms) < APP_USB_UVC_FEED_IDLE_MS;
}

/* 主机开流：投递分辨率并等生效，同时复位保护基准 */
static esp_err_t on_start(uvc_format_t format, int width, int height, int rate, void *cb_ctx)
{
    (void)cb_ctx;

    if (format != UVC_FORMAT_JPEG) {
        ESP_LOGE(TAG, "只支持 MJPEG，主机请求 format=%d", (int)format);
        return ESP_ERR_INVALID_ARG;
    }

    /* 先武装取帧判据，再投递分辨率请求 */
    s_feed_done    = uvc_device_frame_done_count();
    s_feed_done_ms = now_ms();
    s_feed_armed   = true;

    /* 帧源归帧泵独占：只投递请求并等生效 */
    if (!app_video_request_size(width, height)) {
        ESP_LOGE(TAG, "不支持的分辨率 %dx%d", width, height);
        s_feed_armed = false;
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_video_wait_size(width, height, 1000)) {
        ESP_LOGE(TAG, "帧泵 1s 内未切到 %dx%d", width, height);
        s_feed_armed = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "开流 %dx%d @%dfps", width, height, rate);
    return ESP_OK;
}

/* 组件要一帧 */
static uvc_fb_t *on_fb_get(void *cb_ctx)
{
    (void)cb_ctx;

    /* 主机开过流但完成帧计数长时间不动 = 主机已停止取帧：
     * 返回 NULL 让组件跳过，避免把传输/端点留给下一次会话一个坏状态。
     * 判据用完成帧计数：tud_video_n_streaming() 在 bulk 模式下恒为 true。 */
    uint32_t done = uvc_device_frame_done_count();
    uint32_t now  = now_ms();
    if (done != s_feed_done) {
        s_feed_done    = done;
        s_feed_done_ms = now;
    } else if (s_feed_armed && (now - s_feed_done_ms) >= APP_USB_UVC_FEED_IDLE_MS) {
        return NULL;
    }

    const uint8_t   *buf;
    size_t           len;
    int              w, h;
    struct timeval   ts;
    int slot = frame_bus_acquire(&s_last_seq, &buf, &len, &w, &h, &ts);
    if (slot < 0) {
        vTaskDelay(1);       /* 组件在返回 NULL 后会立刻重试，故让出 CPU */
        return NULL;
    }

    if (len > UVC_BUF_SIZE) {
        ESP_LOGW(TAG, "帧 %u 字节 > UVC 缓冲 %d，丢弃", (unsigned)len, UVC_BUF_SIZE);
        frame_bus_release(slot);
        return NULL;
    }

    s_slot = slot;
    s_uvc_fb.buf       = (uint8_t *)buf;
    s_uvc_fb.len       = len;
    s_uvc_fb.width     = (size_t)w;
    s_uvc_fb.height    = (size_t)h;
    s_uvc_fb.format    = UVC_FORMAT_JPEG;
    s_uvc_fb.timestamp = ts;
    return &s_uvc_fb;
}

static void on_fb_return(uvc_fb_t *fb, void *cb_ctx)
{
    (void)fb;
    (void)cb_ctx;
    if (s_slot >= 0) {
        frame_bus_release(s_slot);
        s_slot = -1;
    }
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
    /* 必须始终返回完整长度。newlib/fflush 遇到短写会反复重试，而 esp_restart() 会 flush
     * 控制台 —— CDC 发送 FIFO 满时该重试即变死循环，命令无法返回。 */
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
