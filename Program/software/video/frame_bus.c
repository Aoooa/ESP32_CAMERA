/* frame_bus.c —— 帧槽位。见 frame_bus.h。 */

#include <stdlib.h>
#include <string.h>

#include "frame_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "frame_bus";

typedef struct {
    uint8_t  *buf;
    size_t    cap;
    size_t    len;
    int       width;
    int       height;
    uint32_t  seq;        /* 0 = 空槽 */
    int       refs;       /* 被消费者引用次数 */
    struct timeval ts;
} frame_slot_t;

static frame_slot_t     s_slot[FRAME_BUS_SLOTS];
static SemaphoreHandle_t s_lock;
static uint32_t          s_seq;
static volatile uint32_t s_published;
static volatile uint32_t s_dropped;
static volatile uint32_t s_overwritten;

esp_err_t frame_bus_init(size_t max_len)
{
    if (s_lock) {
        return ESP_OK;
    }
    if (max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < FRAME_BUS_SLOTS; i++) {
        s_slot[i].buf = heap_caps_malloc(max_len, MALLOC_CAP_SPIRAM);
        if (!s_slot[i].buf) {
            ESP_LOGE(TAG, "槽位 %d 分配 %u 字节失败", i, (unsigned)max_len);
            for (int j = 0; j < i; j++) {
                free(s_slot[j].buf);
                s_slot[j].buf = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        s_slot[i].cap = max_len;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "%d 槽 x %u 字节（PSRAM）", FRAME_BUS_SLOTS, (unsigned)max_len);
    return ESP_OK;
}

int frame_bus_publish(const uint8_t *data, size_t len, int width, int height,
                      struct timeval ts)
{
    if (!s_lock || !data || len == 0) {
        return -1;
    }
    if (len > s_slot[0].cap) {
        ESP_LOGW(TAG, "帧 %u 字节 > 槽位 %u，丢弃", (unsigned)len, (unsigned)s_slot[0].cap);
        s_dropped++;
        return -1;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* 空槽优先，其次最旧的未被引用槽，全被引用则丢帧 */
    int pick = -1;
    for (int i = 0; i < FRAME_BUS_SLOTS; i++) {
        if (s_slot[i].seq == 0 && s_slot[i].refs == 0) {
            pick = i;
            break;
        }
    }
    bool overwrite = false;
    if (pick < 0) {
        for (int i = 0; i < FRAME_BUS_SLOTS; i++) {
            if (s_slot[i].refs != 0) {
                continue;
            }
            if (pick < 0 || s_slot[i].seq < s_slot[pick].seq) {
                pick = i;
            }
        }
        overwrite = (pick >= 0);
    }
    if (pick < 0) {
        s_dropped++;
        xSemaphoreGive(s_lock);
        return -1;
    }

    memcpy(s_slot[pick].buf, data, len);
    s_slot[pick].len    = len;
    s_slot[pick].width  = width;
    s_slot[pick].height = height;
    s_slot[pick].ts     = ts;
    s_slot[pick].seq    = ++s_seq;
    s_published++;
    if (overwrite) {
        s_overwritten++;
    }

    xSemaphoreGive(s_lock);
    return pick;
}

int frame_bus_acquire(uint32_t *last_seq, const uint8_t **buf, size_t *len,
                      int *width, int *height, struct timeval *ts)
{
    if (!s_lock || !last_seq) {
        return -1;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int pick = -1;
    for (int i = 0; i < FRAME_BUS_SLOTS; i++) {
        if (s_slot[i].seq == 0 || s_slot[i].seq == *last_seq) {
            continue;
        }
        if (pick < 0 || s_slot[i].seq > s_slot[pick].seq) {
            pick = i;
        }
    }

    int ret = -1;
    if (pick >= 0) {
        s_slot[pick].refs++;
        *last_seq = s_slot[pick].seq;
        if (buf)    *buf    = s_slot[pick].buf;
        if (len)    *len    = s_slot[pick].len;
        if (width)  *width  = s_slot[pick].width;
        if (height) *height = s_slot[pick].height;
        if (ts)     *ts     = s_slot[pick].ts;
        ret = pick;
    }

    xSemaphoreGive(s_lock);
    return ret;
}

void frame_bus_release(int slot)
{
    if (!s_lock || slot < 0 || slot >= FRAME_BUS_SLOTS) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_slot[slot].refs > 0) {
        s_slot[slot].refs--;
    }
    xSemaphoreGive(s_lock);
}

void frame_bus_stats(uint32_t *published, uint32_t *dropped, uint32_t *overwritten)
{
    if (published)   *published   = s_published;
    if (dropped)     *dropped     = s_dropped;
    if (overwritten) *overwritten = s_overwritten;
}
