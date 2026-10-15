#ifndef APP_USB_UVC_H
#define APP_USB_UVC_H

/* app_usb_uvc —— UVC 传输适配层。
 *
 * 帧来自 video/app_video（与传输无关的帧源），本模块只做 UVC 特有的事 +
 * 复合设备里那个 CDC 串口（命令 / 日志）。细节见 app_usb_uvc.c 顶部注释。
 *
 * PC 侧用 OpenCV 直接看：python scripts/uvc_view.py（后端只认 CAP_MSMF），
 * 或者直接用系统相机（它是标准 UVC 设备）。 */

#include <stdbool.h>

#include "esp_err.h"

/* "没人在取帧"的判定阈值（ms）：UVC 的完成帧计数超过这么久没动，就返回 NULL
 * 不再送帧。实测"流开着但没人读"会把传输/端点留给下一次会话一个坏状态
 * （第二次打开摄像头永久 0 帧、只有复位才能好）。
 * 要远大于主机正常的帧间隔（30fps ≈ 33ms）。 */
#define APP_USB_UVC_FEED_IDLE_MS 2000

/* 启动 UVC 设备（分配传输缓冲 + uvc_device_config + uvc_device_init）。
 * 重复调用返回 ESP_ERR_INVALID_STATE。 */
esp_err_t app_usb_uvc_start(void);

/* 主机是否正在取流。 */
bool app_usb_uvc_host_active(void);

/* ---- CDC-ACM（复合设备里的那个串口，与 UVC 共存）---- */

/* 从 CDC 读一个字节，没有数据返回 -1（非阻塞）。 */
int app_usb_uvc_cdc_getc(void);

/* 把 ESP_LOG 输出改到 CDC（UART0 上就没有日志了）。CDC 没连上就丢弃，不阻塞。 */
void app_usb_uvc_log_to_cdc(void);

/* 把 CDC 注册成 VFS 设备并把 stdout 重定向过去：现有 printf/con_handle 的输出
 * 自动改走 USB CDC，不用改控制台代码。 */
void app_usb_uvc_console_to_cdc(void);

#endif /* APP_USB_UVC_H */
