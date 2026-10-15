/* drv_sdcard.c —— SD 卡 SPI 模式挂载（SPI2 与 LCD 共用，见 bsp/bus_spi2.h） */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "drv_sdcard.h"
#include "bus_spi2.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"          /* sdmmc_card_print_info / sdmmc_host_t */
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"

static const char *TAG = "drv_sdcard";

#define SD_PIN_CS    41
#define SD_SPI_HOST  SPI2_HOST

static sdmmc_card_t *s_card;      /* 非 NULL = 已挂载 */

esp_err_t drv_sdcard_init(void)
{
    if (s_card) {
        return ESP_OK;               /* 幂等 */
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    /* SPI2 与 LCD 共用：初始化/释放走引用计数 */
    esp_err_t err = bus_spi2_acquire();
    if (err != ESP_OK) {
        return err;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs  = SD_PIN_CS;
    slot.host_id  = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* 绝不格式化：卡上是用户数据 */
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    ESP_LOGI(TAG, "mounting SD on SPI%d MOSI=%d SCLK=%d MISO=%d CS=%d -> %s",
             host.slot + 1, BUS_SPI2_PIN_MOSI, BUS_SPI2_PIN_SCLK, BUS_SPI2_PIN_MISO,
             SD_PIN_CS, DRV_SDCARD_MOUNT_POINT);

    err = esp_vfs_fat_sdspi_mount(DRV_SDCARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        /* 挂载失败时 esp_vfs_fat_sdspi_mount 内部已回滚总线上的设备 */
        s_card = NULL;
        bus_spi2_release();
        if (err == ESP_FAIL) {
            ESP_LOGE(TAG, "mount failed: 文件系统不是 FAT/exFAT（或已损坏）");
        } else {
            ESP_LOGE(TAG, "card init failed: %s（卡是否插好？）", esp_err_to_name(err));
        }
        return err;
    }

    sdmmc_card_print_info(stdout, s_card);

    /* 图片目录（已存在则忽略） */
    if (mkdir(DRV_SDCARD_IMAGE_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: errno=%d", DRV_SDCARD_IMAGE_DIR, errno);
    }

    uint64_t total = 0, freeb = 0;
    if (drv_sdcard_space(&total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "mounted: total=%.1fMB free=%.1fMB",
                 total / 1048576.0, freeb / 1048576.0);
    }
    return ESP_OK;
}

void drv_sdcard_deinit(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(DRV_SDCARD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
    bus_spi2_release();   /* LCD 还在用时不会真正释放 */
}

bool drv_sdcard_mounted(void)
{
    return s_card != NULL;
}

esp_err_t drv_sdcard_write_file(const char *path, const void *data, size_t len)
{
    if (!path || !data || !s_card) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s for write failed: errno=%d", path, errno);
        return ESP_FAIL;
    }
    size_t wrote = fwrite(data, 1, len, f);
    fflush(f);
    if (fclose(f) != 0 || wrote != len) {
        ESP_LOGE(TAG, "write %s failed: %u/%u bytes, errno=%d",
                 path, (unsigned)wrote, (unsigned)len, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t drv_sdcard_read_file(const char *path, void *buf, size_t cap, size_t *out_len)
{
    if (!path || !buf || !s_card) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "stat %s failed: errno=%d", path, errno);
        return ESP_ERR_NOT_FOUND;
    }
    if ((size_t)st.st_size > cap) {
        ESP_LOGE(TAG, "%s is %ld bytes > buffer %u", path, (long)st.st_size, (unsigned)cap);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t got = fread(buf, 1, st.st_size, f);
    fclose(f);

    if (out_len) {
        *out_len = got;
    }
    return (got == (size_t)st.st_size) ? ESP_OK : ESP_FAIL;
}

esp_err_t drv_sdcard_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_vfs_fat_info(DRV_SDCARD_MOUNT_POINT, total_bytes, free_bytes);
}
