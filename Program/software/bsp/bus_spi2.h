#ifndef BUS_SPI2_H
#define BUS_SPI2_H

/* bus_spi2 —— SPI2 总线（SD 与 LCD 共用 IO38/39/40，CS 各自独立：
 * SD=IO41、LCD=IO45）。 */

#include "esp_err.h"

#define BUS_SPI2_PIN_MOSI 38
#define BUS_SPI2_PIN_SCLK 39
#define BUS_SPI2_PIN_MISO 40

/* 单次 SPI 事务上限：覆盖一条 240x16 条带（7.5KB） */
#define BUS_SPI2_MAX_TRANSFER_SZ (16 * 1024)

/* 首次调用初始化 SPI2，之后只加引用计数；归零时 spi_bus_free()。 */
esp_err_t bus_spi2_acquire(void);
void bus_spi2_release(void);

#endif /* BUS_SPI2_H */
