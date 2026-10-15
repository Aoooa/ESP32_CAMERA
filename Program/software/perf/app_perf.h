#ifndef APP_PERF_H
#define APP_PERF_H

/* app_perf —— CPU 空闲余量与调度迟延测量。
 *
 * 不使用 FreeRTOS 运行时间统计（`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` 需为每个 tick
 * 挂统计源）。本模块只读 `esp_cpu_get_cycle_count()`，在固定 CPU 周期窗口内计数：
 *
 *   · 余量：探针以优先级 1（低于 UVC 任务 4/5、高于 idle）自旋，所得迭代数正比于该核剩余 CPU；
 *   · 基准：`app_main` 最早期（视频/相机/CDC 均未初始化）标定满速迭代数，故结果为绝对空闲率；
 *   · 迟延：探针切到 10ms 周期任务，测 `vTaskDelayUntil` 的实际迟延。
 *
 * 探针钉 core 1（`cam_task` 钉 core 0），测量范围为该核。
 */
void app_perf_init(void);

/* 启动早期调用一次，标定满速基准。 */
void app_perf_calibrate(void);

/* 测量并打印结果（console 命令 `perf`）。不改变任何系统状态：
 * 要对比视频开关，先 `uvc off` 再跑一次，两次空闲率之差即视频流占用。 */
void app_perf_report(void);

#endif /* APP_PERF_H */
