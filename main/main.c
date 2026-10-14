/* main.c —— OV5640 摄像头测试工程入口
 *
 * 工程目的：在同一片 Waveshare ESP32-S3-Touch-LCD-2 上单独验证**摄像头驱动 + 图像获取**
 *          （Embedded_debug_tool 因 UART1=IO2/IO4、UART2=IO16/IO17、LCD=IO40 与摄像头
 *            DVP 引脚冲突，无法在同一固件里跑摄像头，故独立成工程）。
 *
 * 启动顺序：nvs → SD 挂载（失败不致命）→ 摄像头打开（失败不致命）→ console 任务。
 * 不初始化：LCD / 触摸 / UART1 / UART2 / WiFi —— 全部让位给摄像头。
 *
 * 交互：主 console = 原生 USB-Serial/JTAG（板上 USB 口，PC 侧如 COM14），
 *      与 Embedded_debug_tool 的约定一致：单行命令 + 文本回显，可被脚本驱动。
 *      命令见 help。
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_camera.h"
#include "drv_camera.h"
#include "drv_sdcard.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

static const char *TAG = "main";

/* "on"/"1" → true */
static bool parse_on(const char *s)
{
    return s && (!strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true"));
}

static void con_help(void)
{
    printf("commands:\n");
    printf("  help                     本帮助\n");
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

static void con_handle(char *line)
{
    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (!cmd) {
        return;
    }
    char *a1 = strtok_r(NULL, " \t", &save);
    char *a2 = strtok_r(NULL, " \t", &save);

    if (!strcmp(cmd, "help")) {
        con_help();
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
        app_camera_set_quality(a1 ? atoi(a1) : -1);
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

    err = app_camera_open(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "摄像头打开失败：%s（help / stat / cam on 排查）",
                 esp_err_to_name(err));
    }

    xTaskCreate(con_task, "con", 8192, NULL, 4, NULL);

    printf("\n=== Ready ===  help 查看全部命令；shot 抓一张存卡；dump 把图外发到 PC\n");
    fflush(stdout);
}
