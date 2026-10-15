/* app_video —— 与传输无关的视频帧源。见 app_video.h。 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "app_video.h"
#include "drv_camera.h"
#include "frame_bus.h"

static const char *TAG = "app_video";

static bool     s_enabled = true;     /* 命令控制的开关 */
static volatile bool s_close_req;     /* 请求在下一次 get() 里真正关掉摄像头 */
static int      s_w, s_h;             /* 上次成功打开的分辨率（关流重开用） */
static int      s_quality = 12;       /* JPEG 质量（drv_camera 默认值一致） */
static volatile int s_quality_req = -1;

static volatile uint32_t s_frames_out;
static volatile uint32_t s_bytes_out;

/* 帧泵 + 分辨率请求 */
static TaskHandle_t s_pump;
static bool       (*s_need_frames)(void);
static volatile bool s_size_req;
static volatile bool s_size_req_done;
static volatile int  s_size_req_w;
static volatile int  s_size_req_h;

bool app_video_framesize_from_wh(int w, int h, framesize_t *out)
{
    switch (w) {
    case 320:  if (h == 240)  { *out = FRAMESIZE_QVGA; return true; } break;
    case 480:  if (h == 320)  { *out = FRAMESIZE_HVGA; return true; } break;
    case 640:  if (h == 480)  { *out = FRAMESIZE_VGA;  return true; } break;
    case 800:  if (h == 600)  { *out = FRAMESIZE_SVGA; return true; } break;
    case 1280: if (h == 720)  { *out = FRAMESIZE_HD;   return true; } break;
    case 1920: if (h == 1080) { *out = FRAMESIZE_FHD;  return true; } break;
    default: break;
    }
    return false;
}

esp_err_t app_video_open(int width, int height)
{
    framesize_t fs;
    if (!app_video_framesize_from_wh(width, height, &fs)) {
        ESP_LOGE(TAG, "不支持的分辨率 %dx%d", width, height);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;
    if (!drv_camera_ready()) {
        drv_camera_cfg_t cfg;
        drv_camera_default_cfg(&cfg);
        cfg.framesize    = fs;
        cfg.jpeg_quality = s_quality;   /* 重开也保持运行时设的质量 */
        err = drv_camera_open(&cfg);
    } else if (drv_camera_cfg()->framesize != fs) {
        err = drv_camera_set_framesize(fs);
    } else {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头切到 %dx%d 失败：%s", width, height, esp_err_to_name(err));
        return err;
    }

    s_w = width;
    s_h = height;
    /* 注意：这里**不要**把 s_enabled 置 true —— 开关状态归 `uvc on|off` 命令管。
     * 否则主机每次重开流都会把用户的 `uvc off` 悄悄撤销（实测踩到）。 */
    return ESP_OK;
}

bool app_video_ready(void)
{
    return drv_camera_ready();
}

camera_fb_t *app_video_get(void)
{
    /* 质量请求：在这里（帧源任务上下文）下发 */
    if (s_quality_req >= 0 && drv_camera_ready()) {
        int q = s_quality_req;
        s_quality_req = -1;
        if (drv_camera_set_quality(q) == ESP_OK) {
            s_quality = q;
            ESP_LOGI(TAG, "JPEG 质量 → %d", q);
        }
    }

    /* 被关流：真正关摄像头放在这里做（帧源所属任务上下文 → drv_camera 单线程） */
    if (!s_enabled) {
        if (s_close_req) {
            s_close_req = false;
            drv_camera_close();
            ESP_LOGI(TAG, "视频流已关闭（命令）");
        }
        return NULL;
    }

    /* 之前被关过 → 按上次分辨率重开 */
    if (!drv_camera_ready()) {
        framesize_t fs;
        if (!s_w || !app_video_framesize_from_wh(s_w, s_h, &fs)) {
            return NULL;
        }
        drv_camera_cfg_t cfg;
        drv_camera_default_cfg(&cfg);
        cfg.framesize    = fs;
        cfg.jpeg_quality = s_quality;
        if (drv_camera_open(&cfg) != ESP_OK) {
            return NULL;
        }
        ESP_LOGI(TAG, "视频流已重新打开（命令）");
    }

    camera_fb_t *fb = drv_camera_capture();
    if (!fb) {
        return NULL;
    }
    if (fb->format != PIXFORMAT_JPEG || fb->len == 0) {
        ESP_LOGW(TAG, "丢帧：format=%d len=%u", (int)fb->format, (unsigned)fb->len);
        drv_camera_release(fb);
        return NULL;
    }

    s_frames_out++;
    s_bytes_out += (uint32_t)fb->len;
    return fb;
}

void app_video_release(camera_fb_t *fb)
{
    if (fb) {
        drv_camera_release(fb);
    }
}

void app_video_set_enabled(bool on)
{
    s_enabled = on;
    if (!on) {
        s_close_req = true;       /* 实际关闭由 app_video_get() 在帧源任务里执行 */
    }
}

bool app_video_enabled(void)
{
    return s_enabled;
}

void app_video_get_stats(uint32_t *frames, uint32_t *bytes)
{
    if (frames) {
        *frames = s_frames_out;
    }
    if (bytes) {
        *bytes = s_bytes_out;
    }
}

void app_video_set_quality(int q)
{
    if (q >= 0 && q <= 63) {
        s_quality_req = q;
    }
}

int app_video_get_quality(void)
{
    return (s_quality_req >= 0) ? s_quality_req : s_quality;
}

/* ---------------- 帧泵：唯一取帧者，把每帧发布到帧总线 ---------------- */

bool app_video_request_size(int width, int height)
{
    framesize_t fs;
    if (!app_video_framesize_from_wh(width, height, &fs)) {
        return false;
    }
    s_size_req_w    = width;
    s_size_req_h    = height;
    s_size_req_done = false;
    s_size_req      = true;
    return true;
}

bool app_video_wait_size(int width, int height, int timeout_ms)
{
    int64_t t0 = esp_timer_get_time();

    while ((esp_timer_get_time() - t0) < (int64_t)timeout_ms * 1000) {
        if (!s_size_req && s_w == width && s_h == height && drv_camera_ready()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

/* 在帧泵上下文应用分辨率请求 */
static void pump_apply_size(void)
{
    int w = s_size_req_w;
    int h = s_size_req_h;
    s_size_req = false;

    esp_err_t err = app_video_open(w, h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "帧泵切到 %dx%d 失败：%s", w, h, esp_err_to_name(err));
    }
    s_size_req_done = true;
}

static void pump_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_size_req) {
            pump_apply_size();
        }

        /* 关流：仍调一次 get()，关摄像头动作发生在帧泵上下文 */
        if (!s_enabled) {
            app_video_get();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 没有消费者要帧：不取帧 */
        if (s_need_frames && !s_need_frames()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        camera_fb_t *fb = app_video_get();
        if (!fb) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        frame_bus_publish(fb->buf, fb->len, fb->width, fb->height, fb->timestamp);
        app_video_release(fb);
    }
}

void app_video_pump_start(bool (*need_frames)(void))
{
    if (s_pump) {
        return;
    }
    s_need_frames = need_frames;

    if (frame_bus_init(APP_VIDEO_BUS_SLOT_LEN) != ESP_OK) {
        ESP_LOGE(TAG, "帧总线初始化失败，视频流不可用");
        return;
    }

    /* 默认 QVGA；主机开流时可再改 */
    app_video_request_size(320, 240);

    if (xTaskCreatePinnedToCore(pump_task, "vpump", 4096, NULL, 4, &s_pump, 0) != pdPASS) {
        s_pump = NULL;
        ESP_LOGE(TAG, "帧泵任务创建失败");
        return;
    }
    ESP_LOGI(TAG, "帧泵已启动（core 0, prio 4）");
}

