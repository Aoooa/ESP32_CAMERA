#ifndef APP_VIDEO_H
#define APP_VIDEO_H

/* app_video —— 与传输无关的视频帧源。
 *
 * 只与 esp32-camera（drv_camera）交互；UVC 是当前唯一消费者，接入其他通信方式时
 * 复用本接口，只增加对应适配层：
 *
 *     消费者(传输层)                     帧源
 *     ─────────────────                ─────────────
 *     协议里的 宽/高      ──open()──▶  按分辨率打开/重配摄像头
 *     取一帧              ──get()───▶  抓一帧 JPEG（失败返回 NULL）
 *     归还                ──release()─▶ 归还帧缓冲
 *     开关 / 统计          ──set_enabled() / get_stats()
 *
 * 线程模型：drv_camera 非线程安全，所有接口须在同一个任务里调用（UVC 模式下即 UVC 任务）。
 * 帧大小上限属于各消费者（缓冲大小不同），本模块只校验"是否 JPEG / 长度是否为 0"。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

/* 按给定分辨率打开/重配摄像头。 */
esp_err_t app_video_open(int width, int height);

/* 摄像头是否已就绪。 */
bool app_video_ready(void);

/* 取一帧 JPEG；失败返回 NULL。用完必须 release。 */
camera_fb_t *app_video_get(void);

/* 归还帧缓冲（传 NULL 安全）。 */
void app_video_release(camera_fb_t *fb);

/* 开关视频流。off：不再出帧，并在下一次 app_video_get() 中关闭摄像头；
 * on：按上次协商的分辨率自动重开。开关状态只由本接口改变。 */
void app_video_set_enabled(bool on);
bool app_video_enabled(void);

/* 累计交给消费者的帧数与字节数。 */
void app_video_get_stats(uint32_t *frames, uint32_t *bytes);

/* JPEG 质量（0-63，越小越清晰、字节越多）。设置在下一次 app_video_get() 中生效。 */
void app_video_set_quality(int q);
int  app_video_get_quality(void);

/* 宽高 → framesize_t；不支持返回 false。 */
bool app_video_framesize_from_wh(int w, int h, framesize_t *out);

#endif /* APP_VIDEO_H */
