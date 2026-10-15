#ifndef APP_VIDEO_H
#define APP_VIDEO_H

/* app_video —— 与传输无关的视频帧源。
 *
 * 这里只跟 esp32-camera（drv_camera）打交道；**UVC 只是第一个"消费者"**。
 * 以后加别的外部通信方式（以太网 / WiFi / 其他 USB 类 / 自定义协议）时，
 * 复用同一套接口即可，不必再碰摄像头细节：
 *
 *     消费者(传输层)                     帧源(本模块)
 *     ─────────────────                ─────────────
 *     解析协议里的 宽/高   ──open()──▶  按分辨率打开/重配摄像头
 *     要一帧              ──get()───▶  抓一帧 JPEG（失败返回 NULL）
 *     用完归还            ──release()─▶ 归还帧缓冲
 *     自己的开关/统计      ──set_enabled()/get_stats()
 *
 * ⚠️ 线程模型：drv_camera 非线程安全，**所有接口必须在同一个任务里调用**
 *    （UVC 模式下就是 usb_device_uvc 的 UVC 任务）。
 *
 * ⚠️ 帧的大小上限是**消费者的事**（各传输的缓冲大小不同），所以本模块只校验
 *    "是不是 JPEG / 长度是否为 0"，不设固定上限。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_camera.h"

/* 按给定分辨率打开/重配摄像头（宽高由调用方从自己的协议里解析）。 */
esp_err_t app_video_open(int width, int height);

/* 摄像头是否已就绪。 */
bool app_video_ready(void);

/* 取一帧 JPEG；失败返回 NULL（消费者应跳过这一帧）。用完必须 release。 */
camera_fb_t *app_video_get(void);

/* 归还帧缓冲（传 NULL 安全）。 */
void app_video_release(camera_fb_t *fb);

/* 开关视频流。
 * off：不再出帧，并在**下一次 app_video_get()** 里真正关闭摄像头
 *      （不在命令上下文里碰 drv_camera，保证单线程访问）。
 * on ：按上次成功协商的分辨率自动重开。 */
void app_video_set_enabled(bool on);
bool app_video_enabled(void);

/* 确定性统计（不采样）：累计交给消费者的帧数与字节数。 */
void app_video_get_stats(uint32_t *frames, uint32_t *bytes);

/* JPEG 质量（0-63，越小越清晰、字节越多）。运行时可调：
 * 这里只记录"待生效请求"，真正下发在下一次 app_video_get() 于帧源任务里执行
 * （drv_camera 非线程安全，不能让命令上下文直接碰）。 */
void app_video_set_quality(int q);
int  app_video_get_quality(void);

/* 宽高 → framesize_t；不支持返回 false（协议侧做参数校验用）。 */
bool app_video_framesize_from_wh(int w, int h, framesize_t *out);

#endif /* APP_VIDEO_H */
