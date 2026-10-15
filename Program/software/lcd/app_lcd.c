/* app_lcd.c —— LCD 消费者：JPEG → RGB565 → ST7789。见 app_lcd.h。 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/tjpgd.h"

#include "app_lcd.h"
#include "drv_lcd.h"
#include "frame_bus.h"

static const char *TAG = "app_lcd";

/* 条带 = 16 行（JPEG MCU 行高），最多 240 像素宽 */
#define LCD_STRIP_ROWS 16
#define LCD_STRIP_PX   (DRV_LCD_H_RES * LCD_STRIP_ROWS)

/* ST7789 默认大端：内存里高字节在前 */
static inline uint16_t be16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static volatile bool s_enabled = true;
static TaskHandle_t  s_task;

static uint8_t *s_strip[2];      /* 解码用条带（内部 DMA） */
static uint8_t *s_test_strip;    /* 测试图案/动画用条带 */
static uint8_t *s_workbuf;       /* TJpgDec 工作区（ROM 版需 ~3.1KB） */

/* 上一次视频覆盖的纵向范围；变化时（含刚恢复显示）先整屏刷黑 */
static int s_prev_y0 = -1;
static int s_prev_y1 = -1;

/* 统计（lcd_task 写，console 读；单写单读，不加锁） */
static volatile uint32_t s_frames;
static volatile uint32_t s_errors;
static volatile uint32_t s_idle;
static volatile uint32_t s_fps_x10;
static volatile uint32_t s_decode_us;
static volatile uint32_t s_flush_us;

/* ---------------- 解码上下文 ---------------- */

/* JDEC 只有一个 device 字段，输入/输出回调共用它 */
typedef struct {
    /* 输入侧 */
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    /* 输出侧 */
    int      view_w, view_h;  /* 实际显示窗口（水平居中裁剪 + 垂直居中） */
    int      crop_x0;         /* 源图里的水平裁剪起点 */
    int      x_off, y_off;    /* 窗口在面板里的偏移 */
    int      cur;             /* 当前条带号 */
    int      rows;            /* 当前条带已覆盖行数 */
    int      strip_y0;        /* 当前条带首行（源图坐标） */
    uint32_t flush_us;        /* 本帧累计传输耗时 */
} lcd_jpeg_ctx_t;

/* ---------------- TJpgDec 输入/输出回调 ---------------- */

static UINT lcd_jpeg_in(JDEC *jd, BYTE *buff, UINT nbyte)
{
    lcd_jpeg_ctx_t *c = (lcd_jpeg_ctx_t *)jd->device;
    size_t avail = c->len - c->pos;
    UINT n = (nbyte > avail) ? (UINT)avail : nbyte;

    if (buff) {
        memcpy(buff, c->data + c->pos, n);
    }
    c->pos += n;
    return n;
}

/* 发出当前条带；esp_lcd 在下一次设置窗口前排空在途传输 */
static void strip_flush(lcd_jpeg_ctx_t *d)
{
    if (d->rows == 0) {
        return;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = drv_lcd_draw_strip(d->x_off, d->y_off + d->strip_y0,
                                       d->x_off + d->view_w,
                                       d->y_off + d->strip_y0 + d->rows,
                                       s_strip[d->cur]);
    d->flush_us += (uint32_t)(esp_timer_get_time() - t0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "draw_strip 失败：%s（rows=%d y=%d）",
                 esp_err_to_name(err), d->rows, d->strip_y0);
    }

    d->rows = 0;
    d->cur ^= 1;    /* 轮换：上一条正在 DMA */
}

/* 每个 MCU 调一次；rect 是输出图里的绝对矩形（tjpgd.c: mcu_output） */
static UINT lcd_jpeg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    lcd_jpeg_ctx_t *d = (lcd_jpeg_ctx_t *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    const int ncols = rect->right - rect->left + 1;
    const int nrows = rect->bottom - rect->top + 1;
    const int win0 = d->crop_x0;                  /* 源坐标的水平窗口 */
    const int win1 = d->crop_x0 + d->view_w;

    for (int r = 0; r < nrows; r++) {
        const int oy = rect->top + r;             /* 源图绝对行 */
        if (oy >= d->view_h) {                    /* 之后的行都超出窗口 */
            break;
        }
        if (d->rows == 0 || oy < d->strip_y0 ||
            oy >= d->strip_y0 + LCD_STRIP_ROWS) {
            strip_flush(d);                       /* 不在窗口内 → 刷掉，重开 */
            d->strip_y0 = oy;
        }

        /* 本 MCU 的源列范围 → 与水平窗口求交，**两端都夹到 [0, ncols]** */
        int c0 = 0;
        int c1 = ncols;
        if (rect->left < win0) {
            c0 = win0 - rect->left;
        }
        if (rect->left + ncols > win1) {
            c1 = win1 - rect->left;
        }
        if (c0 < 0) {
            c0 = 0;
        }
        if (c1 > ncols) {
            c1 = ncols;
        }
        if (c1 <= c0) {                           /* 整块都在窗口外 */
            continue;
        }

        const int row = oy - d->strip_y0;
        uint16_t *dst = (uint16_t *)(s_strip[d->cur] +
                                     (size_t)row * d->view_w * 2)
                        + (rect->left + c0 - win0);
        const uint8_t *p = src + ((size_t)r * ncols + c0) * 3;

        for (int c = c0; c < c1; c++, p += 3) {
            uint16_t v = (uint16_t)(((p[0] & 0xF8) << 8) |
                                    ((p[1] & 0xFC) << 3) |
                                     (p[2] >> 3));
            *dst++ = be16(v);
        }
        d->rows = row + 1;
    }
    return 1;
}

/* 整屏刷黑；用 s_strip[0]（此刻未被本帧解码占用） */
static void clear_screen_black(void)
{
    uint16_t *p = (uint16_t *)s_strip[0];
    for (int i = 0; i < LCD_STRIP_PX; i++) {
        p[i] = 0;                      /* be16(0x0000) == 0 */
    }
    for (int y = 0; y < DRV_LCD_V_RES; y += LCD_STRIP_ROWS) {
        drv_lcd_draw_strip(0, y, DRV_LCD_H_RES, y + LCD_STRIP_ROWS, s_strip[0]);
    }
}

/* 解一帧并显示。返回解码+传输总耗时（us），失败返回 0。 */
static uint32_t lcd_show_jpeg(const uint8_t *jpg, size_t len)
{
    JDEC           jd;
    lcd_jpeg_ctx_t c = { .data = jpg, .len = len, .pos = 0 };

    JRESULT res = jd_prepare(&jd, lcd_jpeg_in, s_workbuf, 4096, &c);
    if (res != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare 失败：%d（帧 %u 字节）", (int)res, (unsigned)len);
        return 0;
    }

    /* 选最小缩放比：解码尺寸控制在 480 以内 */
    int div = 8;
    for (int cand = 1; cand <= 8; cand *= 2) {
        if ((int)jd.width / cand <= 480 && (int)jd.height / cand <= 480) {
            div = cand;
            break;
        }
    }

    /* jd_decomp 的 scale 是 log2 除数（0/1/2/3），不是除数本身（tjpgd.c: jd->scale） */
    int scale = 0;
    while ((1 << scale) < div) {
        scale++;
    }

    const int src_w = (int)jd.width >> scale;
    const int src_h = (int)jd.height >> scale;
    if (src_w <= 0 || src_h <= 0) {
        return 0;
    }

    /* 水平居中裁剪到面板宽、垂直居中：不变形、满宽 */
    c.view_w  = (src_w > DRV_LCD_H_RES) ? DRV_LCD_H_RES : src_w;
    c.view_h  = (src_h > DRV_LCD_V_RES) ? DRV_LCD_V_RES : src_h;
    c.crop_x0 = (src_w - c.view_w) / 2;
    c.x_off   = (DRV_LCD_H_RES - c.view_w) / 2;
    c.y_off   = (DRV_LCD_V_RES - c.view_h) / 2;

    /* 覆盖区域变化或刚恢复显示：整屏刷黑 */
    if (c.y_off != s_prev_y0 || c.y_off + c.view_h != s_prev_y1) {
        clear_screen_black();
        s_prev_y0 = c.y_off;
        s_prev_y1 = c.y_off + c.view_h;
    }

    int64_t t0 = esp_timer_get_time();
    res = jd_decomp(&jd, lcd_jpeg_out, (BYTE)scale);
    if (res != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp 失败：%d（%ux%u → %dx%d scale=%d）",
                 (int)res, (unsigned)jd.width, (unsigned)jd.height,
                 src_w, src_h, scale);
        return 0;
    }
    strip_flush(&c);   /* 尾部不足一条的行 */

    s_flush_us = c.flush_us;
    return (uint32_t)(esp_timer_get_time() - t0);
}

/* ---------------- 显示任务 ---------------- */

static void lcd_task(void *arg)
{
    (void)arg;
    uint32_t last_seq = 0;

    uint32_t win_frames = 0, win_decode = 0, win_flush = 0;
    int64_t  win_t0 = esp_timer_get_time();

    while (1) {
        if (!s_enabled) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const uint8_t   *buf;
        size_t           len;
        int              w, h;
        struct timeval   ts;
        int slot = frame_bus_acquire(&last_seq, &buf, &len, &w, &h, &ts);
        if (slot < 0) {
            s_idle++;
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        uint32_t dec_us = lcd_show_jpeg(buf, len);
        frame_bus_release(slot);

        if (dec_us) {
            s_frames++;
            win_frames++;
            win_decode += (dec_us > s_flush_us) ? (dec_us - s_flush_us) : 0;  /* 纯解码 */
            win_flush  += s_flush_us;
        } else {
            s_errors++;
        }

        /* 每满 1 秒刷新一次 fps / 平均耗时 */
        int64_t now = esp_timer_get_time();
        if (now - win_t0 >= 1000000) {
            uint32_t el = (uint32_t)(now - win_t0);
            s_fps_x10 = (uint32_t)((uint64_t)win_frames * 10000000ULL / el);
            if (win_frames) {
                s_decode_us = win_decode / win_frames;
                s_flush_us  = win_flush / win_frames;
            }
            win_frames = win_decode = win_flush = 0;
            win_t0 = now;
        }
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t app_lcd_start(void)
{
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = drv_lcd_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "面板初始化失败：%s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < 2; i++) {
        s_strip[i] = heap_caps_aligned_alloc(64, LCD_STRIP_PX * 2,
                                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_strip[i]) {
            ESP_LOGE(TAG, "条带缓冲分配失败（内部 DMA）");
            return ESP_ERR_NO_MEM;
        }
    }
    s_test_strip = heap_caps_aligned_alloc(64, LCD_STRIP_PX * 2,
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_workbuf    = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL);
    if (!s_test_strip || !s_workbuf) {
        ESP_LOGE(TAG, "缓冲分配失败");
        return ESP_ERR_NO_MEM;
    }

    /* core 0、优先级 3（低于 UVC 4/5） */
    if (xTaskCreatePinnedToCore(lcd_task, "lcd", 6144, NULL, 3, &s_task, 0) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "显示任务已启动：%dx%d 竖屏，条带 %d 行 x2（内部 DMA）",
             DRV_LCD_H_RES, DRV_LCD_V_RES, LCD_STRIP_ROWS);
    return ESP_OK;
}

void app_lcd_set_enabled(bool on)
{
    s_enabled = on;
    if (on) {
        s_prev_y0 = -1;      /* 恢复显示时先清屏一次，去掉测试图案残留 */
    }
}

bool app_lcd_enabled(void)
{
    return s_enabled;
}

void app_lcd_report(void)
{
    uint32_t pub, drop, over;
    frame_bus_stats(&pub, &drop, &over);

    printf("[lcd ] ST7789 %dx%d | 显示 %u 帧 | %u.%u fps | 解码 %ums 传输 %ums | "
           "失败 %u | 等帧 %u\n",
           DRV_LCD_H_RES, DRV_LCD_V_RES,
           (unsigned)s_frames, (unsigned)(s_fps_x10 / 10), (unsigned)(s_fps_x10 % 10),
           (unsigned)(s_decode_us / 1000), (unsigned)(s_flush_us / 1000),
           (unsigned)s_errors, (unsigned)s_idle);
    printf("[lcd ] 帧总线：发布 %u 覆盖 %u 丢弃 %u\n",
           (unsigned)pub, (unsigned)over, (unsigned)drop);
}

/* ---------------- 纯显示通路测试（不涉及摄像头/解码） ---------------- */

/* 把整屏刷成一个颜色。paused 为真时同时暂停显示任务（图案保持可见）。 */
static void fill_screen(uint16_t color, bool pause_task)
{
    if (pause_task) {
        s_enabled = false;
        vTaskDelay(pdMS_TO_TICKS(120));      /* 等显示任务让出当前帧 */
    }

    uint16_t c = be16(color);
    for (int i = 0; i < LCD_STRIP_PX; i++) {
        ((uint16_t *)s_test_strip)[i] = c;
    }
    for (int y = 0; y < DRV_LCD_V_RES; y += LCD_STRIP_ROWS) {
        drv_lcd_draw_strip(0, y, DRV_LCD_H_RES, y + LCD_STRIP_ROWS, s_test_strip);
    }
}

void app_lcd_fill(const char *color)
{
    static const struct { const char *name; uint16_t rgb565; } tab[] = {
        { "r", 0xF800 }, { "g", 0x07E0 }, { "b", 0x001F },
        { "w", 0xFFFF }, { "k", 0x0000 },
    };

    if (!s_test_strip) {
        printf("[lcd ] 未初始化\n");
        return;
    }

    if (color && *color) {
        for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
            if (tab[i].name[0] == color[0]) {
                fill_screen(tab[i].rgb565, true);
                printf("[lcd ] 全屏 %s（显示任务已暂停，恢复用 lcd on）\n", tab[i].name);
                return;
            }
        }
        printf("[lcd ] 颜色只认 r/g/b/w/k\n");
        return;
    }

    /* 不带参数：依次红→绿→蓝→白→黑，各 800ms */
    s_enabled = false;
    vTaskDelay(pdMS_TO_TICKS(120));
    printf("[lcd ] 依次全屏：红 绿 蓝 白 黑（各 800ms）\n");
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        fill_screen(tab[i].rgb565, false);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    printf("[lcd ] 完成（显示任务已暂停，恢复用 lcd on）\n");
}

void app_lcd_test_pattern(void)
{
    static const uint16_t colors[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };  /* R G B W */
    static const char    *names[4] = { "红", "绿", "蓝", "白" };
    const int band = DRV_LCD_V_RES / 4;    /* 320/4 = 80：沿 y 分 4 条横带 */

    if (!s_test_strip) {
        printf("[lcd ] 未初始化\n");
        return;
    }

    s_enabled = false;
    vTaskDelay(pdMS_TO_TICKS(120));

    int strips = 0;
    int64_t t0 = esp_timer_get_time();

    for (int b = 0; b < 4; b++) {
        uint16_t c = be16(colors[b]);
        for (int i = 0; i < LCD_STRIP_PX; i++) {
            ((uint16_t *)s_test_strip)[i] = c;
        }
        for (int y = b * band; y < (b + 1) * band; y += LCD_STRIP_ROWS) {
            int y1 = (y + LCD_STRIP_ROWS < (b + 1) * band) ? (y + LCD_STRIP_ROWS)
                                                           : (b + 1) * band;
            drv_lcd_draw_strip(0, y, DRV_LCD_H_RES, y1, s_test_strip);
            strips++;
        }
    }

    /* 计时覆盖 strips-1 条完整传输（最后一条还没排空） */
    int64_t dt = esp_timer_get_time() - t0;
    uint32_t bytes = (uint32_t)(strips - 1) * DRV_LCD_H_RES * LCD_STRIP_ROWS * 2;

    printf("[lcd ] 4 条横带：自上而下 %s/%s/%s/%s（显示任务已暂停，恢复用 lcd on）\n",
           names[0], names[1], names[2], names[3]);
    printf("[lcd ] SPI 带宽：%d 条带 %u 字节 %d ms → %.2f MB/s\n",
           strips - 1, (unsigned)bytes, (int)(dt / 1000),
           (dt > 0) ? (double)bytes / (double)dt : 0.0);
}

/* ---------------- 全屏动画基准 ---------------- */

/* 一条 16 行的全屏条带：深蓝底 + 一条 24 行高的亮带沿 y 上下扫动 */
static void bench_fill(uint8_t *buf, int y, int frame)
{
    uint16_t *p = (uint16_t *)buf;
    int band = ((frame * 6) % (DRV_LCD_V_RES + 48)) - 24;

    for (int r = 0; r < LCD_STRIP_ROWS; r++) {
        uint16_t *row = p + (size_t)r * DRV_LCD_H_RES;
        int yy = y + r;
        bool lit = (yy >= band) && (yy < band + 24);

        for (int x = 0; x < DRV_LCD_H_RES; x++) {
            uint16_t v = lit ? (uint16_t)(0xFFE0 | ((x + frame) & 0x1F))
                             : (uint16_t)(0x001F | (((x + frame * 2) & 0x3F) << 5));
            row[x] = be16(v);
        }
    }
}

void app_lcd_bench(int frames)
{
    if (!s_test_strip) {
        printf("[lcd ] 未初始化\n");
        return;
    }
    if (frames <= 0) {
        frames = 200;
    }

    /* 暂停显示任务 */
    s_enabled = false;
    vTaskDelay(pdMS_TO_TICKS(150));

    const int strips_per_frame = DRV_LCD_V_RES / LCD_STRIP_ROWS;   /* 20 */

    int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < frames; f++) {
        for (int y = 0; y < DRV_LCD_V_RES; y += LCD_STRIP_ROWS) {
            bench_fill(s_test_strip, y, f);
            drv_lcd_draw_strip(0, y, DRV_LCD_H_RES, y + LCD_STRIP_ROWS, s_test_strip);
        }
    }
    int64_t dt = esp_timer_get_time() - t0;

    double sec = (double)dt / 1e6;
    double mb  = (double)frames * DRV_LCD_H_RES * DRV_LCD_V_RES * 2 / 1e6;

    printf("[lcd ] 全屏动画：%d 帧 x %d 条带，%.0f ms → %.1f fps，%.2f MB/s\n",
           frames, strips_per_frame, dt / 1000.0, frames / sec, mb / sec);
    printf("[lcd ] 显示任务已暂停，恢复用 lcd on\n");
}
