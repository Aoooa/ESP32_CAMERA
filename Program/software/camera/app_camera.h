#ifndef APP_CAMERA_H
#define APP_CAMERA_H

/* app_camera —— 摄像头测试业务层（与 UI 解耦：本工程无 LVGL，输出走串口 console）。
 *
 * 职责：
 *   1) 打开/重配摄像头（drv_camera），并把 hmirror/vflip 等寄存器状态记住，
 *      在改分辨率触发 reconfigure 后自动重放（esp_camera_reconfigure 会把传感器
 *      寄存器恢复到默认值，不重放就会出现"改了分辨率画面又镜像回去"）。
 *   2) 取图：单帧/连拍（统计帧率、字节数、失败次数），JPEG 直接落 SD 卡。
 *   3) 外发：把一帧 JPEG 以 base64 分块打到 console 口，PC 侧脚本还原成 .jpg
 *      —— 这样"图像获取"是否真的成功可以在 PC 上看图验证，而不是只看日志。
 *   4) 状态：传感器型号/PID、当前参数、累计统计、内存与 SD 剩余空间。
 *
 * 线程模型：全部接口须在**同一个任务**里串行调用（本工程 = main.c 的 console 任务）。
 * 依赖：drv_camera（摄像头）+ drv_sdcard（存储）。不含 platform/、不含 LVGL。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "drv_camera.h"

/* 打开摄像头。cfg=NULL 用 drv_camera_default_cfg()。 */
esp_err_t app_camera_open(const drv_camera_cfg_t *cfg);

/* 关闭摄像头（esp_camera_deinit），释放 LCD_CAM/DMA/帧缓冲 */
esp_err_t app_camera_close(void);

/* 抓 count 张并（save_to_sd 为真时）写 /sdcard/cam/shot_%04lu.jpg。
 * path_out 非空时回传**最后**一张的完整路径（仅 count==1 时有意义）。
 * 逐张打印 "shot 0001 640x480 21435B 123.4ms"，结束打印汇总（含 fps）。
 * 返回 ESP_OK = 至少一张成功。 */
esp_err_t app_camera_capture(int count, int gap_ms, bool save_to_sd,
                             char *path_out, size_t path_sz);

/* 改分辨率（内部会重放 mirror/flip）。name 见 drv_camera_framesize_parse。 */
esp_err_t app_camera_set_size(const char *name);

/* 改 JPEG 质量 0-63 */
esp_err_t app_camera_set_quality(int quality);

/* 镜像/翻转/彩条开关（彩条不依赖真实光路，用来单独验证 DVP 数据通路） */
esp_err_t app_camera_mirror(bool on);
esp_err_t app_camera_flip(bool on);
esp_err_t app_camera_bar(bool on);

/* 打印传感器型号/PID/当前参数 + 累计统计 + 内存 + SD 空间 */
void app_camera_stat(void);

/* 打印摄像头寄存器级状态（sensor->status 全量，排查白屏/黑屏/不曝光用） */
void app_camera_info(void);

/* 抓 count 帧，以 base64 外发到 console 口（每帧一段 BEGIN/END 包裹） */
esp_err_t app_camera_dump(int count);

/* 累计统计清零 */
void app_camera_reset_stats(void);

#endif /* APP_CAMERA_H */
