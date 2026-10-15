/* bus_spi2.c —— SPI2 共享总线（SD + LCD）。见 bus_spi2.h。 */

#include "bus_spi2.h"

#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "bus_spi2";

static int s_refs;

esp_err_t bus_spi2_acquire(void)
{
    if (s_refs > 0) {
        s_refs++;
        return ESP_OK;
    }

    spi_bus_config_t bus = {
        .mosi_io_num     = BUS_SPI2_PIN_MOSI,
        .miso_io_num     = BUS_SPI2_PIN_MISO,
        .sclk_io_num     = BUS_SPI2_PIN_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BUS_SPI2_MAX_TRANSFER_SZ,
    };

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize(SPI2) 失败：%s", esp_err_to_name(err));
        return err;
    }

    s_refs = 1;
    ESP_LOGI(TAG, "SPI2 就绪：MOSI=%d SCLK=%d MISO=%d max_transfer=%d",
             BUS_SPI2_PIN_MOSI, BUS_SPI2_PIN_SCLK, BUS_SPI2_PIN_MISO,
             BUS_SPI2_MAX_TRANSFER_SZ);
    return ESP_OK;
}

void bus_spi2_release(void)
{
    if (s_refs <= 0) {
        return;
    }
    if (--s_refs == 0) {
        spi_bus_free(SPI2_HOST);
        ESP_LOGI(TAG, "SPI2 已释放");
    }
}
