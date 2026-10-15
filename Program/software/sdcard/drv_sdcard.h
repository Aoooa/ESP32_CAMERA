#ifndef DRV_SDCARD_H
#define DRV_SDCARD_H

/* drv_sdcard —— SD 卡（SPI 模式）挂载与写文件。
 *
 * 板级（Waveshare ESP32-S3-Touch-LCD-2，SD 槽与 LCD 共用 SPI2）：
 *   MOSI=IO38  SCLK=IO39  MISO=IO40  CS=IO41
 *   来源互相印证：示例程序/…/ESP-IDF/01_sd_card_test/main/sd_card_example_main.c
 *                 Embedded_debug_tool/sdcard/drv_sdcard.c（实机可用）
 *   ⚠️ CLAUDE.md 里那张 "SD_CS=IO36/IO33/34/35" 的表是过时信息，以代码/例程为准。
 *
 * 本工程不驱动 LCD，所以 SPI2 总线由本模块自己 spi_bus_initialize()
 * （Embedded_debug_tool 里总线归 LCD 初始化，SD 只挂设备，两边不能照抄）。
 *
 * 挂载点 /sdcard，图片目录 /sdcard/cam；文件系统 FAT12/16/32/exFAT（长文件名 + UTF-8）。
 *
 * 线程模型：非线程安全，须由同一个任务串行调用（本工程 = console 任务）。
 * 摄像头与 SD 卡物理上不冲突（DVP 用 IO2/4/6-17/21，SD 用 IO38-41），可并行工作。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define DRV_SDCARD_MOUNT_POINT "/sdcard"
#define DRV_SDCARD_IMAGE_DIR   DRV_SDCARD_MOUNT_POINT "/cam"

/* 初始化 SPI2 总线 + 挂载 SD 卡 + 建好图片目录。
 * 卡没插/接触不良会返回错误（不格式化，绝不动用户数据）。 */
esp_err_t drv_sdcard_init(void);

/* 卸载并释放总线 */
void drv_sdcard_deinit(void);

/* 是否已挂载 */
bool drv_sdcard_mounted(void);

/* 写整个文件（覆盖）。path 为绝对路径（如 "/sdcard/cam/shot_0001.jpg"）。 */
esp_err_t drv_sdcard_write_file(const char *path, const void *data, size_t len);

/* 读取文件内容到 buf（cap 为 buf 容量），*out_len 返回实际字节数。
 * 文件大于 cap 时返回 ESP_ERR_INVALID_SIZE。 */
esp_err_t drv_sdcard_read_file(const char *path, void *buf, size_t cap, size_t *out_len);

/* 卷容量/剩余空间（字节） */
esp_err_t drv_sdcard_space(uint64_t *total_bytes, uint64_t *free_bytes);

#endif /* DRV_SDCARD_H */
