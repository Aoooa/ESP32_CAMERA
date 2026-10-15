#ifndef FRAME_BUS_H
#define FRAME_BUS_H

/* frame_bus —— 帧泵（唯一取帧者）向多个消费者分发最新帧。
 * 4 个 PSRAM 槽位；发布优先用空闲槽，无空闲时覆盖最旧的未被引用槽，全被引用则丢帧。
 * acquire 只返回比上次更新的最新帧，消费者用完必须 release。 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#include "esp_err.h"

#define FRAME_BUS_SLOTS 4

/* 分配槽位缓冲（PSRAM）。max_len = 单帧最大字节数。 */
esp_err_t frame_bus_init(size_t max_len);

/* 帧泵上下文：发布一帧。返回槽号(>=0)，-1 = 全部被引用、本帧丢弃。 */
int frame_bus_publish(const uint8_t *data, size_t len, int width, int height,
                      struct timeval ts);

/* 消费者上下文：取一帧比 *last_seq 更新的最新帧，命中时 refs++ 并回填元数据。
 * 无新帧返回 -1。 */
int frame_bus_acquire(uint32_t *last_seq, const uint8_t **buf, size_t *len,
                      int *width, int *height, struct timeval *ts);

void frame_bus_release(int slot);

/* 统计：发布帧数 / 因无空闲槽丢弃 / 覆盖旧帧 */
void frame_bus_stats(uint32_t *published, uint32_t *dropped, uint32_t *overwritten);

#endif /* FRAME_BUS_H */
