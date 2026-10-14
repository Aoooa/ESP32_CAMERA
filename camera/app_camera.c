/* app_camera.c —— 摄像头测试业务层：取图/落盘/base64 外发/状态 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_camera.h"
#include "drv_sdcard.h"

#include "esp_camera.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"

/* 累计统计（跨命令累积，clear 清零）。本模块一律用 printf 输出（console 协议），
 * 不用 ESP_LOG，所以没有 TAG。 */
static struct {
    uint32_t ok;            /* 成功抓到的帧数 */
    uint32_t fail;          /* esp_camera_fb_get() 返回 NULL 的次数 */
    uint32_t eoi_ok;        /* JPEG 头尾完整（FFD8…FFD9）的帧数 */
    uint32_t saved;         /* 成功写卡的帧数 */
    uint32_t save_fail;     /* 写卡失败次数 */
    size_t   bytes_min;
    size_t   bytes_max;
    uint64_t bytes_total;
    int64_t  cap_us_total;  /* 等帧（队列）耗时累加，反映是否有现成帧可用 */
    int64_t  save_us_total; /* 写卡耗时累加 */
    int64_t  itv_us_last;   /* 最近一次连拍测到的帧间隔（微秒，来自帧时间戳） */
} s_st;

static uint32_t s_seq;                    /* 全局帧号（B64-BEGIN 用，dump/shot 都递增） */
static uint32_t s_file_seq;               /* 落盘文件名序号（只由 shot 递增，dump 不占号） */
static uint32_t s_dump_seq;               /* 第几次 dump 调用 */
static bool     s_seq_seeded;             /* 是否已按卡上现有文件把 s_file_seq 推到末尾 */
static bool     s_mirror, s_flip, s_bar;  /* 寄存器开关，reconfigure 后要重放 */

/* 序号只存在内存里，重启就归零 → 会覆盖上一轮的同名图片。首次落盘前扫一遍目录，
 * 把序号推到已有最大值之后（文件名格式 cam_<分辨率>_<4位序号>.jpg，取最后一段数字）。 */
static void seed_file_seq_from_card(void)
{
    if (s_seq_seeded) {
        return;
    }
    s_seq_seeded = true;
    if (!drv_sdcard_mounted()) {
        return;
    }

    DIR *d = opendir(DRV_SDCARD_IMAGE_DIR);
    if (!d) {
        return;                       /* 目录还没建（SD 刚挂载时 drv_sdcard 已建好） */
    }

    uint32_t mx = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *p = strrchr(e->d_name, '_');
        if (!p) {
            continue;
        }
        unsigned long v = strtoul(p + 1, NULL, 10);
        if (v > mx && v < 1000000UL) {   /* 只认我们自己的编号，别的文件不参与 */
            mx = (uint32_t)v;
        }
    }
    closedir(d);

    if (mx) {
        s_file_seq = mx;
        printf("[sd ] 卡上已有 %lu 张，新图从 %04lu 开始\n",
               (unsigned long)mx, (unsigned long)(mx + 1));
    }
}

/* JPEG 完整性判定：头 FF D8 / 尾 FF D9 */
static bool jpeg_ok(camera_fb_t *fb)
{
    return fb->format == PIXFORMAT_JPEG && fb->len > 4 &&
           fb->buf[0] == 0xFF && fb->buf[1] == 0xD8 &&
           fb->buf[fb->len - 2] == 0xFF && fb->buf[fb->len - 1] == 0xD9;
}

/* 把记住的开关写回传感器。esp_camera_reconfigure() 会把寄存器恢复到默认值，
 * 所以改分辨率后必须重放，否则 mirror/flip 静默丢失。 */
static void replay_sensor_opts(void)
{
    sensor_t *s = drv_camera_sensor();
    if (!s) {
        return;
    }
    s->set_hmirror(s, s_mirror ? 1 : 0);
    s->set_vflip(s, s_flip ? 1 : 0);
    s->set_colorbar(s, s_bar ? 1 : 0);
    s->set_quality(s, (int)drv_camera_cfg()->jpeg_quality);
}

esp_err_t app_camera_open(const drv_camera_cfg_t *cfg)
{
    drv_camera_cfg_t c;
    if (cfg) {
        c = *cfg;
    } else {
        drv_camera_default_cfg(&c);
    }

    esp_err_t err = drv_camera_open(&c);
    if (err != ESP_OK) {
        return err;
    }
    replay_sensor_opts();
    return ESP_OK;
}

esp_err_t app_camera_close(void)
{
    return drv_camera_close();
}

esp_err_t app_camera_capture(int count, int gap_ms, bool save_to_sd,
                             char *path_out, size_t path_sz)
{
    if (!drv_camera_ready()) {
        printf("[cam] 摄像头未就绪（cam on 打开）\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (count < 1) {
        count = 1;
    }
    if (save_to_sd && !drv_sdcard_mounted()) {
        printf("[cam] SD 未挂载，本次只抓帧不落盘（sd on 可挂载）\n");
        save_to_sd = false;
    }
    if (save_to_sd) {
        seed_file_seq_from_card();
    }

    const drv_camera_cfg_t *cfg = drv_camera_cfg();
    uint32_t ok_before = s_st.ok;
    int64_t  t_all     = esp_timer_get_time();
    /* 帧间隔用帧自带的时间戳（esp_camera 打的是该帧第一个 DMA buffer 的采集时刻），
     * 这才是传感器真实节奏；"等帧耗时"只反映队列里有没有现成的帧
     * （grab_mode=LATEST + fb=2 时通常直接命中，等帧耗时≈0）。 */
    int64_t  prev_ts   = 0;
    int64_t  itv_sum   = 0;
    uint32_t itv_n     = 0;

    for (int i = 0; i < count; i++) {
        int64_t t0 = esp_timer_get_time();
        camera_fb_t *fb = drv_camera_capture();
        int64_t t1 = esp_timer_get_time();

        if (!fb) {
            s_st.fail++;
            printf("[cam] %d/%d CAPTURE_FAIL wait %lldms\n",
                   i + 1, count, (long long)(t1 - t0) / 1000);
            continue;
        }

        int64_t ts = (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;
        if (prev_ts && ts > prev_ts && (ts - prev_ts) < 2000000) {
            itv_sum += (ts - prev_ts);
            itv_n++;
        }
        prev_ts = ts;

        bool complete = jpeg_ok(fb);
        s_st.ok++;
        s_st.cap_us_total += (t1 - t0);
        if (complete) {
            s_st.eoi_ok++;
        }
        if (s_st.bytes_min == 0 || fb->len < s_st.bytes_min) {
            s_st.bytes_min = fb->len;
        }
        if (fb->len > s_st.bytes_max) {
            s_st.bytes_max = fb->len;
        }
        s_st.bytes_total += fb->len;

        printf("[cam] %d/%d %ux%u %luB %lldms %s%s\n",
               i + 1, count, (unsigned)fb->width, (unsigned)fb->height,
               (unsigned long)fb->len, (long long)(t1 - t0) / 1000,
               complete ? "JPEG-OK" : "JPEG-BAD",
               fb->format == PIXFORMAT_JPEG ? "" : " NOT-JPEG");

        if (save_to_sd) {
            char path[96];
            snprintf(path, sizeof(path), "%s/cam_%s_%04lu.jpg",
                     DRV_SDCARD_IMAGE_DIR, drv_camera_framesize_str(cfg->framesize),
                     (unsigned long)++s_file_seq);

            int64_t s0 = esp_timer_get_time();
            esp_err_t werr = drv_sdcard_write_file(path, fb->buf, fb->len);
            int64_t s1 = esp_timer_get_time();

            if (werr == ESP_OK) {
                s_st.saved++;
                s_st.save_us_total += (s1 - s0);
                printf("[sd ] saved %s %lldms\n", path, (long long)(s1 - s0) / 1000);
                if (path_out && path_sz) {
                    snprintf(path_out, path_sz, "%s", path);
                }
            } else {
                s_st.save_fail++;
                printf("[sd ] WRITE_FAIL %s: %s\n", path, esp_err_to_name(werr));
            }
        }

        drv_camera_release(fb);

        if (gap_ms > 0 && i + 1 < count) {
            vTaskDelay(pdMS_TO_TICKS(gap_ms));
        }
    }

    uint32_t this_ok = s_st.ok - ok_before;
    if (this_ok) {
        int64_t wait_avg = s_st.cap_us_total / (int64_t)s_st.ok;
        int64_t itv_avg  = itv_n ? itv_sum / (int64_t)itv_n : 0;
        if (itv_avg) {
            s_st.itv_us_last = itv_avg;
        }
        printf("[cam] run: %u frames wall %lldms | interval %lldms -> %.1f fps"
               " (帧时间戳) | queue_wait %lldms | bytes min/avg/max=%lu/%lu/%lu\n",
               (unsigned)this_ok,
               (long long)(esp_timer_get_time() - t_all) / 1000,
               (long long)itv_avg / 1000,
               itv_avg > 0 ? 1000000.0 / (double)itv_avg : 0.0,
               (long long)wait_avg / 1000,
               (unsigned long)s_st.bytes_min,
               (unsigned long)(s_st.bytes_total / s_st.ok),
               (unsigned long)s_st.bytes_max);
    }
    printf("[cam] total: ok=%lu fail=%lu eoi_ok=%lu saved=%lu save_fail=%lu\n",
           (unsigned long)s_st.ok, (unsigned long)s_st.fail,
           (unsigned long)s_st.eoi_ok, (unsigned long)s_st.saved,
           (unsigned long)s_st.save_fail);
    return this_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t app_camera_set_size(const char *name)
{
    framesize_t fs;
    if (!drv_camera_framesize_parse(name, &fs)) {
        printf("[cam] 未知分辨率 '%s'；可用: %s/%s/%s/%s/%s/%s/%s/%s/%s/%s/%s 或 640x480\n",
               name ? name : "",
               drv_camera_framesize_str(FRAMESIZE_QQVGA), drv_camera_framesize_str(FRAMESIZE_QVGA),
               drv_camera_framesize_str(FRAMESIZE_HVGA), drv_camera_framesize_str(FRAMESIZE_VGA),
               drv_camera_framesize_str(FRAMESIZE_SVGA), drv_camera_framesize_str(FRAMESIZE_XGA),
               drv_camera_framesize_str(FRAMESIZE_HD), drv_camera_framesize_str(FRAMESIZE_SXGA),
               drv_camera_framesize_str(FRAMESIZE_UXGA), drv_camera_framesize_str(FRAMESIZE_QXGA),
               drv_camera_framesize_str(FRAMESIZE_QSXGA));
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = drv_camera_set_framesize(fs);
    if (err != ESP_OK) {
        printf("[cam] size %s 失败: %s\n", name, esp_err_to_name(err));
        return err;
    }
    replay_sensor_opts();
    return ESP_OK;
}

esp_err_t app_camera_set_quality(int quality)
{
    esp_err_t err = drv_camera_set_quality(quality);
    printf("[cam] quality %d: %s\n", quality,
           err == ESP_OK ? "OK" : esp_err_to_name(err));
    return err;
}

esp_err_t app_camera_mirror(bool on)
{
    sensor_t *s = drv_camera_sensor();
    if (!s) {
        printf("[cam] 摄像头未就绪\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_mirror = on;
    int ret = s->set_hmirror(s, on ? 1 : 0);
    printf("[cam] hmirror=%s (%s)\n", on ? "on" : "off", ret == 0 ? "OK" : "sensor write fail");
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t app_camera_flip(bool on)
{
    sensor_t *s = drv_camera_sensor();
    if (!s) {
        printf("[cam] 摄像头未就绪\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_flip = on;
    int ret = s->set_vflip(s, on ? 1 : 0);
    printf("[cam] vflip=%s (%s)\n", on ? "on" : "off", ret == 0 ? "OK" : "sensor write fail");
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

/* 测试彩条：不依赖真实光路，用来单独验证 DVP/DMA 数据通路是否干净 */
esp_err_t app_camera_bar(bool on)
{
    sensor_t *s = drv_camera_sensor();
    if (!s) {
        printf("[cam] 摄像头未就绪\n");
        return ESP_ERR_INVALID_STATE;
    }
    s_bar = on;
    int ret = s->set_colorbar(s, on ? 1 : 0);
    printf("[cam] colorbar=%s (%s)\n", on ? "on" : "off", ret == 0 ? "OK" : "sensor write fail");
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

void app_camera_reset_stats(void)
{
    memset(&s_st, 0, sizeof(s_st));
    printf("[cam] stats cleared\n");
}

/* base64 帧外发（直接从帧缓冲编码，不额外申请内存；每行 ≤124 字符）。
 *
 * 协议标记必须**顶行、不带任何前缀**：
 *   SET=<调用号> SIZE=<W>x<H> FMT=<pixformat>
 *   B64-BEGIN seq=<全局帧号> len=<字节数> crc=<CRC32>
 *   <base64 …>
 *   B64-END seq=<全局帧号>
 * 为什么不能写成 "[cam] B64-END"：接收端按 "B64-END" 定位结束，前面的 "[cam] " 会落进
 * 码流区间，而 "cam" 三个字母恰好是合法 base64 字符，于是每帧凭空多出 3 个字符
 * （实测长度校验差 2 字节）。
 *
 * 每 16 行（≈2KB）主动 fflush + 让出一个 tick：主 console 是 USB-Serial/JTAG，没有流控，
 * 大帧连发时给 host 留出取数据的余量；这是防御性措施，真正的完整性由
 * len+CRC32 校验 + PC 侧重试保证。另外整个 dump 期间全系统日志静音（见 app_camera_dump），
 * 避免 stdout 出现第二个写者把码流交错打断。
 *
 * 序号约定：set = 本次 dump 调用号；seq = 全局帧号（s_seq），与落盘文件名用的
 * s_file_seq 分开计数，互不跳号。 */
static void dump_b64_frame(camera_fb_t *fb, uint32_t set, uint32_t seq)
{
    static const char *T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    uint32_t crc = esp_rom_crc32_le(0, fb->buf, (uint32_t)fb->len);
    printf("SET=%lu SIZE=%ux%u FMT=%d\n",
           (unsigned long)set, (unsigned)fb->width, (unsigned)fb->height,
           (int)fb->format);
    printf("B64-BEGIN seq=%lu len=%lu crc=%08lx\n",
           (unsigned long)seq, (unsigned long)fb->len, (unsigned long)crc);

    char   line[128];
    size_t ln = 0;
    size_t i  = 0;
    int    since_flush = 0;

    while (i < fb->len) {
        size_t rem = fb->len - i;
        uint32_t v = (uint32_t)fb->buf[i] << 16;
        if (rem >= 2) {
            v |= (uint32_t)fb->buf[i + 1] << 8;
        }
        if (rem >= 3) {
            v |= (uint32_t)fb->buf[i + 2];
        }

        line[ln++] = T[(v >> 18) & 0x3F];
        line[ln++] = T[(v >> 12) & 0x3F];
        line[ln++] = (rem >= 2) ? T[(v >> 6) & 0x3F] : '=';
        line[ln++] = (rem >= 3) ? T[v & 0x3F]         : '=';

        i += (rem >= 3) ? 3 : rem;

        if (ln + 4 >= sizeof(line)) {
            line[ln] = 0;
            printf("%s\n", line);
            ln = 0;
            if (++since_flush >= 16) {
                since_flush = 0;
                fflush(stdout);
                vTaskDelay(1);
            }
        }
    }
    if (ln) {
        line[ln] = 0;
        printf("%s\n", line);
    }

    fflush(stdout);
    printf("B64-END seq=%lu\n", (unsigned long)seq);
    fflush(stdout);
}

esp_err_t app_camera_dump(int count)
{
    if (!drv_camera_ready()) {
        printf("[cam] 摄像头未就绪\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (count < 1) {
        count = 1;
    }

    /* base64 期间把全系统日志静音：stdout 一旦有第二个写者，newlib 的缓冲会在
     * 任意字节边界交错，把某一行 base64 打断（实测 QSXGA 三次都少 8 个字符 = 6 字节）。
     * 静音后整段码流只有 console 任务在写，接收端按 len+CRC32 校验即可确认完整性。
     * 需要 CONFIG_LOG_DYNAMIC_LEVEL_CONTROL（本工程 sdkconfig 默认已开）。 */
    esp_log_level_set("*", ESP_LOG_NONE);

    const uint32_t set = ++s_dump_seq;
    for (int i = 0; i < count; i++) {
        camera_fb_t *fb = drv_camera_capture();
        if (!fb) {
            printf("[cam] dump %d: CAPTURE_FAIL\n", i + 1);
            esp_log_level_set("*", esp_log_get_default_level());
            return ESP_FAIL;
        }
        dump_b64_frame(fb, set, ++s_seq);
        drv_camera_release(fb);
    }

    esp_log_level_set("*", esp_log_get_default_level());
    return ESP_OK;
}

void app_camera_stat(void)
{
    printf("== camera ==\n");
    printf(" state    : %s\n", drv_camera_ready() ? "ready" : "NOT ready");

    sensor_t *s = drv_camera_sensor();
    if (s) {
        camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
        printf(" sensor   : %s PID=0x%02x VER=0x%02x MIDL=0x%02x MIDH=0x%02x addr=0x%02x\n",
               info ? info->name : "?", s->id.PID, s->id.VER,
               s->id.MIDL, s->id.MIDH, s->slv_addr);
        printf(" xclk     : %dHz  max_size=%s  psram_dma=%s\n", s->xclk_freq_hz,
               info ? drv_camera_framesize_str(info->max_size) : "?",
               esp_camera_get_psram_mode() ? "on" : "off");
        printf(" out fmt  : %s\n",
               s->pixformat == PIXFORMAT_JPEG   ? "JPEG(传感器内编码)" :
               s->pixformat == PIXFORMAT_RGB565 ? "RGB565" :
               s->pixformat == PIXFORMAT_YUV422 ? "YUV422" :
               s->pixformat == PIXFORMAT_GRAYSCALE ? "GRAYSCALE" : "other");
    }

    const drv_camera_cfg_t *cfg = drv_camera_cfg();
    printf(" framesize: %s (%ux%u) quality=%d fb=%u mirror=%s flip=%s bar=%s\n",
           drv_camera_framesize_str(cfg->framesize),
           resolution[cfg->framesize].width, resolution[cfg->framesize].height,
           cfg->jpeg_quality, (unsigned)cfg->fb_count,
           s_mirror ? "on" : "off", s_flip ? "on" : "off", s_bar ? "on" : "off");

    printf(" frames   : ok=%lu fail=%lu eoi_ok=%lu saved=%lu save_fail=%lu\n",
           (unsigned long)s_st.ok, (unsigned long)s_st.fail,
           (unsigned long)s_st.eoi_ok, (unsigned long)s_st.saved,
           (unsigned long)s_st.save_fail);
    printf(" counters : frame_seq=%lu card_file_seq=%lu dump_calls=%lu\n",
           (unsigned long)s_seq, (unsigned long)s_file_seq,
           (unsigned long)s_dump_seq);
    if (s_st.ok) {
        int64_t itv = s_st.itv_us_last;
        printf(" bytes    : min=%lu avg=%lu max=%lu total=%lu\n",
               (unsigned long)s_st.bytes_min,
               (unsigned long)(s_st.bytes_total / s_st.ok),
               (unsigned long)s_st.bytes_max,
               (unsigned long)s_st.bytes_total);
        printf(" timing   : queue_wait_avg=%lldms save_avg=%lldms",
               (long long)(s_st.cap_us_total / (int64_t)s_st.ok) / 1000,
               s_st.saved ? (long long)(s_st.save_us_total / (int64_t)s_st.saved) / 1000 : -1);
        if (itv) {
            printf(" interval=%lldms fps=%.1f (帧时间戳)",
                   (long long)itv / 1000, 1000000.0 / (double)itv);
        }
        printf("\n");
    }

    printf(" heap     : internal free=%luKB largest=%luKB | psram free=%luKB\n",
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    printf("== sdcard ==\n");
    if (drv_sdcard_mounted()) {
        uint64_t total = 0, freeb = 0;
        drv_sdcard_space(&total, &freeb);
        printf(" mounted  : %s  images->%s  total=%.1fMB free=%.1fMB\n",
               DRV_SDCARD_MOUNT_POINT, DRV_SDCARD_IMAGE_DIR,
               (double)(total / 1048576.0), (double)(freeb / 1048576.0));
    } else {
        printf(" mounted  : NO (sd on 重试)\n");
    }
}

void app_camera_info(void)
{
    sensor_t *s = drv_camera_sensor();
    if (!s) {
        printf("[cam] 摄像头未就绪\n");
        return;
    }
    camera_status_t *st = &s->status;   /* 驱动记录的当前状态（不是寄存器实时回读） */
    printf("== sensor status ==\n");
    printf(" quality=%d brightness=%d contrast=%d saturation=%d sharpness=%d\n",
           st->quality, st->brightness, st->contrast, st->saturation, st->sharpness);
    printf(" denoise=%d special_effect=%d wb_mode=%d awb=%d awb_gain=%d\n",
           st->denoise, st->special_effect, st->wb_mode, st->awb, st->awb_gain);
    printf(" aec=%d aec2=%d ae_level=%d aec_value=%d agc=%d agc_gain=%d gainceiling=%d\n",
           st->aec, st->aec2, st->ae_level, st->aec_value, st->agc, st->agc_gain,
           st->gainceiling);
    printf(" bpc=%d wpc=%d raw_gma=%d lenc=%d hmirror=%d vflip=%d dcw=%d colorbar=%d\n",
           st->bpc, st->wpc, st->raw_gma, st->lenc, st->hmirror, st->vflip,
           st->dcw, st->colorbar);
}
