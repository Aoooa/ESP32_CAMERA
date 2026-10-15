#ifndef APP_PERF_H
#define APP_PERF_H

/* app_perf —— 轻量 CPU 余量 / 调度迟延探针。
 *
 * 设计约束（用户要求）：**不使用 FreeRTOS 运行时间统计**
 * （`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` / TRACE_FACILITY 要给每个 tick 挂统计源，
 * 稳态开销和内存都不小）。
 *
 * 做法：只用 `esp_cpu_get_cycle_count()`（读寄存器，FORCE_INLINE），探针任务平时休眠，
 * 仅在执行 `perf` 命令时按需苏醒约 2 秒 —— 稳态开销为零。
 *
 *   · 余量：探针以优先级 1（高于 idle 0、低于全部业务任务）自旋一个固定 **CPU 周期**窗口，
 *     它拿到的迭代次数正比于"系统还剩多少 CPU"。业务任务随时抢占它，所以测量不拖慢业务。
 *   · 视频占用：关掉视频流再测一次 → 关流时探针几乎吃掉全部余量，于是
 *     `1 - 开流迭代/关流迭代` 就是视频流真正占用的 CPU 比例（自带标定，无需外部基准）。
 *   · 干扰：探针切到 10ms 周期任务，量 `vTaskDelayUntil` 的实际迟延 max/avg。
 */
void app_perf_init(void);

/* 启动早期调用一次（在任何摄像头/USB/CDC 初始化之前）：标定"满速基准"，
 * 之后 perf 报的"空闲率"就是绝对值。 */
void app_perf_calibrate(void);

/* 跑一次测量并打印结果（console 命令 `perf` 调用）。
 * **不改动任何系统状态**：要对比"视频开/关"，先 `uvc off` 再跑一次，两次空闲率之差即占用。 */
void app_perf_report(void);

#endif /* APP_PERF_H */
