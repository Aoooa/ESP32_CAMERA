#ifndef APP_VIDEO_H
#define APP_VIDEO_H

/* app_video —— 与传输无关的视频帧源：帧泵独占取帧并发布到 frame_bus，
 * 消费者（usb_uvc / app_lcd）各自取帧。分辨率请求由消费者投递，在帧泵上下文落到摄像头。 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

/* 帧总线单槽容量 */
#define APP_VIDEO_BUS_SLOT_LEN (128 * 1024)

/* 起帧泵任务（唯一取帧者）+ 初始化帧总线。need_frames 由 main 注入，
 * 返回 false 时帧泵不取帧。 */
void app_video_pump_start(bool (*need_frames)(void));

/* 消费者投递分辨率请求（线程安全）。false = 不支持该分辨率。 */
bool app_video_request_size(int width, int height);

/* 等分辨率请求被帧泵应用完成（毫秒超时）。 */
bool app_video_wait_size(int width, int height, int timeout_ms);

/* 按给定分辨率打开/重配摄像头。只能在帧泵上下文调用。 */
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
