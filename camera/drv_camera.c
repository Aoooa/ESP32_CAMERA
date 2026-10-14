/* drv_camera.c —— OV5640 DVP 摄像头封装（esp32-camera） */

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "drv_camera.h"
#include "esp_log.h"

static const char *TAG = "drv_camera";

static bool              s_ready;
static drv_camera_cfg_t  s_cfg;

/* esp32-camera 默认支持的分辨率短名（与 sensor.h 的 framesize_t 顺序一致） */
static const struct { const char *name; framesize_t fs; } s_sizes[] = {
    { "96x96",  FRAMESIZE_96X96   },
    { "qqvga",  FRAMESIZE_QQVGA   },
    { "128x128",FRAMESIZE_128X128 },
    { "qcif",   FRAMESIZE_QCIF    },
    { "hqvga",  FRAMESIZE_HQVGA   },
    { "240x240",FRAMESIZE_240X240 },
    { "qvga",   FRAMESIZE_QVGA    },
    { "320x320",FRAMESIZE_320X320 },
    { "cif",    FRAMESIZE_CIF     },
    { "hvga",   FRAMESIZE_HVGA    },
    { "vga",    FRAMESIZE_VGA     },
    { "svga",   FRAMESIZE_SVGA    },
    { "xga",    FRAMESIZE_XGA     },
    { "hd",     FRAMESIZE_HD      },
    { "sxga",   FRAMESIZE_SXGA    },
    { "uxga",   FRAMESIZE_UXGA    },
    { "fhd",    FRAMESIZE_FHD     },
    { "p_hd",   FRAMESIZE_P_HD    },
    { "p_3mp",  FRAMESIZE_P_3MP   },
    { "qxga",   FRAMESIZE_QXGA    },
    { "qhd",    FRAMESIZE_QHD     },
    { "wqxga",  FRAMESIZE_WQXGA   },
    { "p_fhd",  FRAMESIZE_P_FHD   },
    { "qsxga",  FRAMESIZE_QSXGA   },
    { "5mp",    FRAMESIZE_5MP     },
};

void drv_camera_default_cfg(drv_camera_cfg_t *cfg)
{
    cfg->framesize    = FRAMESIZE_VGA;   /* 640x480 */
    cfg->jpeg_quality = 12;
    cfg->fb_count     = 2;
}

/* 把用户配置展开成 esp32-camera 的 camera_config_t（引脚 = 本工程头文件常量） */
static void build_config(camera_config_t *c, const drv_camera_cfg_t *u)
{
    memset(c, 0, sizeof(*c));

    c->pin_pwdn     = DRV_CAMERA_PIN_PWDN;
    c->pin_reset    = DRV_CAMERA_PIN_RESET;   /* -1：靠传感器软复位 */
    c->pin_xclk     = DRV_CAMERA_PIN_XCLK;
    c->pin_sccb_sda = DRV_CAMERA_PIN_SIOD;
    c->pin_sccb_scl = DRV_CAMERA_PIN_SIOC;

    c->pin_d7 = DRV_CAMERA_PIN_D7;
    c->pin_d6 = DRV_CAMERA_PIN_D6;
    c->pin_d5 = DRV_CAMERA_PIN_D5;
    c->pin_d4 = DRV_CAMERA_PIN_D4;
    c->pin_d3 = DRV_CAMERA_PIN_D3;
    c->pin_d2 = DRV_CAMERA_PIN_D2;
    c->pin_d1 = DRV_CAMERA_PIN_D1;
    c->pin_d0 = DRV_CAMERA_PIN_D0;

    c->pin_vsync = DRV_CAMERA_PIN_VSYNC;
    c->pin_href  = DRV_CAMERA_PIN_HREF;
    c->pin_pclk  = DRV_CAMERA_PIN_PCLK;

    c->xclk_freq_hz = DRV_CAMERA_XCLK_HZ;
    /* ESP32-S3 由 LCD_CAM 产生 XCLK，不用 LEDC，这两项给占位值即可 */
    c->ledc_timer   = LEDC_TIMER_0;
    c->ledc_channel = LEDC_CHANNEL_0;

    c->pixel_format  = PIXFORMAT_JPEG;
    c->frame_size    = u->framesize;
    c->jpeg_quality  = u->jpeg_quality;
    c->fb_count      = u->fb_count;
    c->fb_location   = CAMERA_FB_IN_PSRAM;
    c->grab_mode     = CAMERA_GRAB_LATEST;
    c->jpeg_buffer_size = 0;                 /* 0 = 组件默认（按分辨率自动算） */
}

esp_err_t drv_camera_open(const drv_camera_cfg_t *cfg)
{
    if (s_ready) {
        ESP_LOGE(TAG, "already opened, use drv_camera_set_framesize()/close()");
        return ESP_ERR_INVALID_STATE;
    }
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg = *cfg;
    if (s_cfg.jpeg_quality < 0)   s_cfg.jpeg_quality = 0;
    if (s_cfg.jpeg_quality > 63)  s_cfg.jpeg_quality = 63;
    if (s_cfg.fb_count < 1)       s_cfg.fb_count = 1;

    camera_config_t c;
    build_config(&c, &s_cfg);

    ESP_LOGI(TAG, "init: JPEG %ux%u quality=%d fb=%u psram xclk=%dHz sccb=%d/%d",
             resolution[s_cfg.framesize].width, resolution[s_cfg.framesize].height,
             s_cfg.jpeg_quality, (unsigned)s_cfg.fb_count, DRV_CAMERA_XCLK_HZ,
             DRV_CAMERA_PIN_SIOD, DRV_CAMERA_PIN_SIOC);

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s (0x%x)", esp_err_to_name(err), err);
        return err;
    }

    s_ready = true;

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
        ESP_LOGI(TAG, "sensor: %s PID=0x%02x VER=0x%02x addr=0x%02x xclk=%dHz",
                 info ? info->name : "?", s->id.PID, s->id.VER, s->slv_addr, s->xclk_freq_hz);
        if (info && s_cfg.framesize > info->max_size) {
            ESP_LOGW(TAG, "%s max size is %ux%u，请求的分辨率被组件降到该值",
                     info->name, resolution[info->max_size].width,
                     resolution[info->max_size].height);
        }
    }
    return ESP_OK;
}

esp_err_t drv_camera_close(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_camera_deinit();
    s_ready = false;
    return err;
}

esp_err_t drv_camera_set_framesize(framesize_t fs)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    drv_camera_cfg_t want = s_cfg;
    want.framesize = fs;

    camera_config_t c;
    build_config(&c, &want);

    /* reconfigure = deinit + init：帧缓冲按新分辨率重建 */
    esp_err_t err = esp_camera_reconfigure(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfigure to %ux%u failed: %s",
                 resolution[fs].width, resolution[fs].height, esp_err_to_name(err));
        s_ready = false;   /* 组件已 deinit，状态不能留在 ready */
        return err;
    }
    s_cfg = want;
    ESP_LOGI(TAG, "framesize -> %s (%ux%u)", drv_camera_framesize_str(fs),
             resolution[fs].width, resolution[fs].height);
    return ESP_OK;
}

esp_err_t drv_camera_set_quality(int quality)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (quality < 0 || quality > 63) {
        return ESP_ERR_INVALID_ARG;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s->set_quality(s, quality) != 0) {
        return ESP_FAIL;
    }
    s_cfg.jpeg_quality = quality;
    return ESP_OK;
}

camera_fb_t *drv_camera_capture(void)
{
    if (!s_ready) {
        return NULL;
    }
    return esp_camera_fb_get();
}

void drv_camera_release(camera_fb_t *fb)
{
    if (fb) {
        esp_camera_fb_return(fb);
    }
}

sensor_t *drv_camera_sensor(void)
{
    return s_ready ? esp_camera_sensor_get() : NULL;
}

const drv_camera_cfg_t *drv_camera_cfg(void)
{
    return &s_cfg;
}

bool drv_camera_ready(void)
{
    return s_ready;
}

const char *drv_camera_framesize_str(framesize_t fs)
{
    for (size_t i = 0; i < sizeof(s_sizes) / sizeof(s_sizes[0]); i++) {
        if (s_sizes[i].fs == fs) {
            return s_sizes[i].name;
        }
    }
    return "?";
}

bool drv_camera_framesize_parse(const char *name, framesize_t *out)
{
    if (!name || !out) {
        return false;
    }

    char low[16];
    size_t n = 0;
    for (; name[n] && n < sizeof(low) - 1; n++) {
        low[n] = (char)tolower((unsigned char)name[n]);
    }
    low[n] = 0;

    unsigned w = 0, h = 0;
    if (sscanf(low, "%ux%u", &w, &h) == 2) {           /* 直接按 640x480 给 */
        for (int i = 0; i < FRAMESIZE_INVALID; i++) {
            if (resolution[i].width == w && resolution[i].height == h) {
                *out = (framesize_t)i;
                return true;
            }
        }
        return false;
    }

    for (size_t i = 0; i < sizeof(s_sizes) / sizeof(s_sizes[0]); i++) {
        if (strcmp(low, s_sizes[i].name) == 0) {
            *out = s_sizes[i].fs;
            return true;
        }
    }
    return false;
}
