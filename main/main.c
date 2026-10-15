/* main.c —— OV5640 摄像头测试工程入口
 *
 * 工程目的：在同一片 Waveshare ESP32-S3-Touch-LCD-2 上单独验证**摄像头驱动 + 图像获取**
 *          （Embedded_debug_tool 因 UART1=IO2/IO4、UART2=IO16/IO17、LCD=IO40 与摄像头
 *            DVP 引脚冲突，无法在同一固件里跑摄像头，故独立成工程）。
 *
 * 启动顺序：nvs → SD 挂载（失败不致命）→ UVC 启动 / 摄像头打开（失败不致命）。
 * 不初始化：LCD / 触摸 / UART1 / UART2 / WiFi —— 全部让位给摄像头。
 *
 * 交互：两种模式**编译期互斥**（ESP32-S3 的 USB-Serial/JTAG 与 USB-OTG 共用内部 PHY）：
 *   APP_WITH_USB_UVC = 1（默认）：USB 口做 UVC 摄像头 → PC 侧 scripts/uvc_view.py 看实时画面。
 *      无 console（日志走 UART0），摄像头由 UVC 任务独占（drv_camera 非线程安全）。
 *   APP_WITH_USB_UVC = 0：USB 口做 USB-Serial/JTAG console → 原有单行命令测试模式，无视频。
 *      切回该模式还需把 sdkconfig.defaults 的 console 改回 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y。
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_camera.h"
#include "app_usb_uvc.h"
#include "app_video.h"
#include "app_perf.h"
#include "drv_camera.h"
#include "drv_sdcard.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "nvs_flash.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp32s3/rom/usb/usb_persist.h"          /* USBDC_PERSIST_ENA */
#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"  /* chip_usb_set_persist_flags */
#include "esp32s3/rom/usb/usb_dc.h"               /* usb_dc_prepare_persist */

static const char *TAG = "main";

#define APP_WITH_USB_UVC 1

/* 控制台（con_help/con_handle/con_task）两种模式都编译：
 * UVC 模式下它挂在复合设备的 CDC 串口上（见 app_usb_uvc_*），
 * 摄像头类命令由 con_handle 里的 guard 拒绝，避免和 UVC 任务抢摄像头。 */

/* "on"/"1" → true */
static bool parse_on(const char *s)
{
    return s && (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true"));
}

static void con_help(void)
{
    printf("commands:\n");
    printf("  help                     本帮助\n");
    printf("  uvc on|off               开/关视频流（off 释放摄像头 DMA/帧缓冲）\n");
    printf("  perf                     测 CPU 余量与调度迟延（含视频开/关对比）\n");
    printf("  boot                     主机主动进 ROM 下载模式（之后可直接 esptool 烧录）\n");
    printf("  stat                     传感器/参数/累计统计/内存/SD 空间\n");
    printf("  info                     sensor->status 各开关当前值\n");
    printf("  cam on|off               打开/关闭摄像头（off 释放 LCD_CAM+DMA+帧缓冲）\n");
    printf("  shot [n]                 抓 n 张并写 /sdcard/cam/*.jpg（默认 1）\n");
    printf("  burst [n] [gap_ms]       连拍不写卡，只看帧率/字节（默认 30 张）\n");
    printf("  dump [n]                 把 n 帧 JPEG 以 base64 从 console 口外发\n");
    printf("  size <分辨率>            qvga|hvga|vga|svga|xga|hd|sxga|uxga|qxga|qsxga|640x480\n");
    printf("  quality <0-63>           JPEG 质量（越小越清晰）\n");
    printf("  mirror on|off            水平镜像\n");
    printf("  flip on|off              垂直翻转\n");
    printf("  bar on|off               传感器彩条（不依赖光路，专测 DVP 数据通路）\n");
    printf("  ls [目录]                列目录（默认 %s）\n", DRV_SDCARD_IMAGE_DIR);
    printf("  sd on|off                挂载/卸载 SD 卡\n");
    printf("  clear                    清零累计统计\n");
    printf("  reboot                   重启\n");
}

/* 列目录：验证图是不是真的落到卡上了 */
static void con_ls(const char *dir)
{
    if (!drv_sdcard_mounted()) {
        printf("[sd ] 未挂载，无法列目录（sd on）\n");
        return;
    }

    const char *path = (dir && *dir) ? dir : DRV_SDCARD_IMAGE_DIR;
    DIR *d = opendir(path);
    if (!d) {
        printf("[sd ] opendir %s 失败\n", path);
        return;
    }

    int n = 0;
    uint64_t bytes = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        /* 精度限制让 gcc 能算出 snprintf 的上界（-Werror=format-truncation），
         * 同时避免超长文件名把 st 读成越界 */
        char full[400];
        snprintf(full, sizeof(full), "%.100s/%.255s", path, e->d_name);

        struct stat st = { 0 };
        long sz = (stat(full, &st) == 0) ? (long)st.st_size : -1;
        if (sz > 0) {
            bytes += (uint64_t)sz;
        }
        printf("  %s %10ld %s\n", (S_ISDIR(st.st_mode) ? "DIR " : "FILE"),
               sz, e->d_name);
        n++;
    }
    closedir(d);
    printf("[sd ] %s: %d 项, 文件共 %llu 字节\n", path, n,
           (unsigned long long)bytes);
}

/* 两段式进入 ROM 下载模式。
 * ① `boot` 命令：只置本标志 + esp_restart()；
 * ② `app_main` 最早期（UVC/CDC 尚未初始化）：置 USB 持久化标志与下载引导位后重启。
 * 拆分原因：ROM 的 USB 函数假定 ROM 独占 USB 控制器，在 TinyUSB 已初始化后调用会死锁。
 * RTC_NOINIT 内存跨重启保留，用 magic 防止未初始化值误触发。 */
#define REBOOT_DL_MAGIC 0x424f4f54u   /* 'BOOT' */
RTC_NOINIT_ATTR static uint32_t g_reboot_to_dl;

static void con_handle(char *line)
{    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd) {
        return;
    }
    char *a1 = strtok_r(NULL, " \t", &save);
    char *a2 = strtok_r(NULL, " \t", &save);

#if APP_WITH_USB_UVC
    /* UVC 模式下摄像头由 UVC 任务独占（drv_camera 非线程安全），这些命令会跟它抢，
     * 直接拒绝；其余命令（help/stat/info/ls/sd/clear/reboot/uvc/perf/boot/quality）照常可用。
     * `quality` 不在此列：它走 app_video 的"待生效请求"，由帧源任务下发，是线程安全的。 */
    static const char *cam_cmds[] = { "cam", "shot", "burst", "dump",
                                     "size", "mirror", "flip", "bar" };
    for (int i = 0; i < (int)(sizeof(cam_cmds) / sizeof(cam_cmds[0])); i++) {
        if (!strcmp(cmd, cam_cmds[i])) {
            printf("[con] '%s' 在 UVC 模式下不可用：摄像头归 UVC 任务独占\n", cmd);
            return;
        }
    }
#endif

    if (!strcmp(cmd, "help")) {
        con_help();
    } else if (!strcmp(cmd, "uvc")) {
        bool on = !a1 || parse_on(a1);
        app_video_set_enabled(on);   /* 直接控制"视频源"，与传输无关 */
        printf("[uvc] 视频流 %s\n", on ? "开" : "关");
    } else if (!strcmp(cmd, "boot")) {
        /* 两段式第一段：置标志 + 重启；下载模式的切换在 app_main 最早期执行。 */
        g_reboot_to_dl = REBOOT_DL_MAGIC;
        esp_restart();
    } else if (!strcmp(cmd, "perf")) {
        app_perf_report();      /* 轻量 CPU 余量/迟延测量，详见 perf/app_perf.h */
    } else if (!strcmp(cmd, "stat")) {
        app_camera_stat();
    } else if (!strcmp(cmd, "info")) {
        app_camera_info();
    } else if (!strcmp(cmd, "cam")) {
        if (!a1 || parse_on(a1)) {
            printf("[cam] open: %s\n", esp_err_to_name(app_camera_open(NULL)));
        } else {
            printf("[cam] close: %s\n", esp_err_to_name(app_camera_close()));
        }
    } else if (!strcmp(cmd, "shot")) {
        int n = a1 ? atoi(a1) : 1;
        char path[96] = { 0 };
        app_camera_capture(n, 0, true, path, sizeof(path));
    } else if (!strcmp(cmd, "burst")) {
        int n = a1 ? atoi(a1) : 30;
        int gap = a2 ? atoi(a2) : 0;
        app_camera_capture(n, gap, false, NULL, 0);
    } else if (!strcmp(cmd, "dump")) {
        app_camera_dump(a1 ? atoi(a1) : 1);
    } else if (!strcmp(cmd, "size")) {
        app_camera_set_size(a1);
    } else if (!strcmp(cmd, "quality")) {
        int q = a1 ? atoi(a1) : -1;
        if (q < 0 || q > 63) {
            printf("[uvc] quality 现值 = %d（用法 quality <0-63>，越小越清晰、字节越多）\n",
                   app_video_get_quality());
        } else {
            app_video_set_quality(q);   /* 走帧源的"待生效请求"，不跨任务碰 drv_camera */
            printf("[uvc] quality -> %d（下一帧生效）\n", q);
        }
    } else if (!strcmp(cmd, "mirror")) {
        app_camera_mirror(parse_on(a1));
    } else if (!strcmp(cmd, "flip")) {
        app_camera_flip(parse_on(a1));
    } else if (!strcmp(cmd, "bar")) {
        app_camera_bar(parse_on(a1));
    } else if (!strcmp(cmd, "ls")) {
        con_ls(a1);
    } else if (!strcmp(cmd, "sd")) {
        if (!a1 || parse_on(a1)) {
            printf("[sd ] mount: %s\n", esp_err_to_name(drv_sdcard_init()));
        } else {
            drv_sdcard_deinit();
            printf("[sd ] unmounted\n");
        }
    } else if (!strcmp(cmd, "clear")) {
        app_camera_reset_stats();
    } else if (!strcmp(cmd, "reboot")) {
        printf("[sys] reboot\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else {
        printf("未知命令 '%s'（help）\n", cmd);
    }
}

/* console 任务：单行命令读取（不做 DTR/RTS 翻转，脚本可直接驱动） */
static void con_task(void *arg)
{
    (void)arg;
    char buf[128];
    int  n = 0;

    printf("cam> ");
    fflush(stdout);
    while (1) {
        int c = getchar();
        if (c < 0) {
            c = app_usb_uvc_cdc_getc();   /* 复合设备里的 CDC 串口：同一根 USB 线也能发命令 */
        }
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n') {
            buf[n] = 0;
            n = 0;
            con_handle(buf);
            printf("cam> ");
            fflush(stdout);
        } else if (n < (int)sizeof(buf) - 1) {
            buf[n++] = (char)c;
        } else {
            n = 0;   /* 超长行直接丢弃，避免命令被截断执行 */
        }
    }
}

void app_main(void)
{
    /* 两段式第二段：必须在任何 USB/CDC 初始化之前执行。
     * 顺序与 IDF 的 esp_usb_console_before_restart 一致：先置 USB-Serial/JTAG 持久化
     * 标志，再置下载引导位。 */
    if (g_reboot_to_dl == REBOOT_DL_MAGIC) {
        g_reboot_to_dl = 0;
        usb_dc_prepare_persist();
        chip_usb_set_persist_flags(USBDC_PERSIST_ENA);
        REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
        esp_restart();
    }

    /* 尽早建探针并标定"满速基准"：此刻视频/相机/CDC 都还没起来，基准才干净。
     * 之后 `perf` 报的"空闲率"就是绝对值（见 perf/app_perf.c）。 */
    app_perf_init();
    app_perf_calibrate();

    printf("\n===== Camera_test_tool =====\n");
    printf(" board : Waveshare ESP32-S3-Touch-LCD-2 (ESP32-S3, 8MB octal PSRAM)\n");
    printf(" camera: OV5640 DVP  XCLK=8 SIOC=16 SIOD=21 D0-7=%d,%d,%d,%d,%d,%d,%d,%d\n",
           DRV_CAMERA_PIN_D0, DRV_CAMERA_PIN_D1, DRV_CAMERA_PIN_D2, DRV_CAMERA_PIN_D3,
           DRV_CAMERA_PIN_D4, DRV_CAMERA_PIN_D5, DRV_CAMERA_PIN_D6, DRV_CAMERA_PIN_D7);
    printf("         VSYNC=%d HREF=%d PCLK=%d PWDN=%d RESET=%d\n",
           DRV_CAMERA_PIN_VSYNC, DRV_CAMERA_PIN_HREF, DRV_CAMERA_PIN_PCLK,
           DRV_CAMERA_PIN_PWDN, DRV_CAMERA_PIN_RESET);
    printf(" sdcard: SPI2 MOSI=38 SCLK=39 MISO=40 CS=41 -> %s\n", DRV_SDCARD_MOUNT_POINT);
    printf(" 不初始化 LCD/触摸/UART1/UART2（与摄像头 DVP 引脚冲突，见 drv_camera.h）\n");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* SD 先起：摄像头失败时也要能落盘/查目录；反之存储是排查依赖 */
    err = drv_sdcard_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD 挂载失败：%s（shot 只抓帧不落盘；sd on 可重试）",
                 esp_err_to_name(err));
    }

#if APP_WITH_USB_UVC
    /* UVC 模式下摄像头由 UVC 任务独占（drv_camera 非线程安全，见 drv_camera.h），
     * 故这里不预开摄像头、也不起 console 任务 —— 由 UVC 的 start_cb 按主机要求的分辨率打开。 */
    err = app_usb_uvc_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB UVC 启动失败：%s", esp_err_to_name(err));
    }
    /* 控制台输出与日志改走复合设备里的 CDC 串口（同一根 USB 线），并起 console 任务收命令 */
    app_usb_uvc_console_to_cdc();
    app_usb_uvc_log_to_cdc();
    xTaskCreate(con_task, "con", 8192, NULL, 4, NULL);
    printf("\n=== Ready ===  摄像头：python scripts/uvc_view.py；命令：同一根线的 COM 口\n");
#else
    err = app_camera_open(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头打开失败：%s（help / stat / cam on 排查）",
                 esp_err_to_name(err));
    }

    xTaskCreate(con_task, "con", 8192, NULL, 4, NULL);

    printf("\n=== Ready ===  help 查看全部命令；shot 抓一张存卡；dump 把图外发到 PC\n");
#endif
    fflush(stdout);
}
