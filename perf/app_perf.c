/* app_perf —— 见 app_perf.h。不依赖 FreeRTOS 运行时间统计，稳态零开销。 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "app_perf.h"
#include "app_video.h"

#define PERF_SPIN_PRIO    1      /* 高于 idle(0)，低于所有业务任务（con=4 等） */
#define PERF_JITTER_PRIO  6
#define PERF_TASK_STACK   3072

/* 测量窗口用 **CPU 周期**而不是时间：探针被抢占时计数器照样走，窗口口径才一致。 */
#define PERF_WINDOW_CYC   240000000u     /* ≈1s @240MHz */
#define PERF_JITTER_MS    10
#define PERF_JITTER_N     100            /* 10ms × 100 ≈ 1s */

#define PERF_PH_IDLE   0
#define PERF_PH_SPIN   1
#define PERF_PH_JITTER 2
#define PERF_PH_DONE   3

static volatile int      s_phase;
static volatile uint32_t s_spin_iters;
static volatile uint32_t s_baseline_iters;   /* 启动早期标定的"满速基准" */
static volatile uint32_t s_jit_max_cyc;
static volatile uint32_t s_jit_avg_cyc;
static volatile uint32_t s_cyc_per_ms;

static void perf_task(void *arg)
{
    (void)arg;

    for (;;) {
        int ph = s_phase;

        if (ph == PERF_PH_SPIN) {
            vTaskPrioritySet(NULL, PERF_SPIN_PRIO);
            uint32_t n = 0;
            uint32_t t0 = esp_cpu_get_cycle_count();
            while ((uint32_t)(esp_cpu_get_cycle_count() - t0) < PERF_WINDOW_CYC) {
                n++;
            }
            s_spin_iters = n;
            s_phase = PERF_PH_DONE;

        } else if (ph == PERF_PH_JITTER) {
            vTaskPrioritySet(NULL, PERF_JITTER_PRIO);

            /* 先自测 cycles/ms（拿周期计数器和 esp_timer 对一遍），不依赖任何主频 API */
            uint32_t c0 = esp_cpu_get_cycle_count();
            int64_t  u0 = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(20));
            uint32_t c1 = esp_cpu_get_cycle_count();
            int64_t  u1 = esp_timer_get_time();
            uint32_t cyc_per_ms = (uint32_t)(((uint64_t)(c1 - c0) * 1000) / (uint64_t)(u1 - u0));
            if (!cyc_per_ms) {
                cyc_per_ms = 240000;    /* 兜底：240MHz */
            }
            s_cyc_per_ms = cyc_per_ms;

            TickType_t last = xTaskGetTickCount();
            uint32_t expect = esp_cpu_get_cycle_count() + cyc_per_ms * PERF_JITTER_MS;
            uint32_t maxc = 0;
            uint64_t sum = 0;
            for (int i = 0; i < PERF_JITTER_N; i++) {
                vTaskDelayUntil(&last, pdMS_TO_TICKS(PERF_JITTER_MS));
                uint32_t late = (uint32_t)(esp_cpu_get_cycle_count() - expect);
                if ((int32_t)late < 0) {
                    late = 0;
                }
                if (late > maxc) {
                    maxc = late;
                }
                sum += late;
                expect += cyc_per_ms * PERF_JITTER_MS;
            }
            s_jit_max_cyc = maxc;
            s_jit_avg_cyc = (uint32_t)(sum / PERF_JITTER_N);
            s_phase = PERF_PH_DONE;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_perf_init(void)
{
    /* 钉 core 1：cam_task 钉在 core 0（CONFIG_CAMERA_CORE0=y），探针不与其竞争。 */
    xTaskCreatePinnedToCore(perf_task, "perf", PERF_TASK_STACK, NULL,
                            PERF_SPIN_PRIO, NULL, 1);
}

/* 触发一个阶段并等它跑完（最多等 8s） */
static void perf_run(int phase)
{
    s_phase = phase;
    for (int i = 0; i < 800 && s_phase != PERF_PH_DONE; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* 启动早期标定满速基准：此时视频/相机/CDC 均未初始化，探针可获得约全部 CPU。
 * 之后每次 perf 用同一窗口测量，比值即绝对空闲率。 */
void app_perf_calibrate(void)
{
    perf_run(PERF_PH_SPIN);
    s_baseline_iters = s_spin_iters;
}

void app_perf_report(void)
{
    uint32_t f0 = 0, b0 = 0, f1 = 0, b1 = 0;
    int64_t  t0 = esp_timer_get_time();

    app_video_get_stats(&f0, &b0);
    perf_run(PERF_PH_SPIN);                  /* 只测当前状态，不改动任何东西 */
    uint32_t iters = s_spin_iters;
    int64_t  t_spin_end = esp_timer_get_time();
    perf_run(PERF_PH_JITTER);
    app_video_get_stats(&f1, &b1);

    int64_t  dt_us = esp_timer_get_time() - t0;
    uint32_t cpm   = s_cyc_per_ms ? s_cyc_per_ms : 240000;
    uint32_t df    = f1 - f0;
    uint32_t db    = b1 - b0;

    printf("[perf] 窗口 %u CPU 周期  主频≈%u MHz  自旋相位 %.0f ms  全程 %.1f s\n",
           (unsigned)PERF_WINDOW_CYC, (unsigned)(cpm / 1000),
           (double)(t_spin_end - t0) / 1000.0, (double)dt_us / 1e6);
    if (s_baseline_iters) {
        uint32_t pm = (uint32_t)(((uint64_t)iters * 1000) / s_baseline_iters);
        printf("[perf] 空闲余量 = %u (基准 %u) => 空闲约 %u.%u%%\n",
               (unsigned)iters, (unsigned)s_baseline_iters,
               (unsigned)(pm / 10), (unsigned)(pm % 10));
    } else {
        printf("[perf] 空闲余量 = %u (无基准)\n", (unsigned)iters);
    }
    printf("[perf] 视频输出(确定性计数) = %u 帧 / %u 字节 => %.1f fps, %.1f KB/s\n",
           (unsigned)df, (unsigned)db,
           dt_us ? (double)df * 1e6 / (double)dt_us : 0.0,
           dt_us ? (double)db * 1e6 / (double)dt_us / 1024.0 : 0.0);
    printf("[perf] 10ms 周期任务迟延: max = %u us, avg = %u us\n",
           (unsigned)(((uint64_t)s_jit_max_cyc * 1000) / cpm),
           (unsigned)(((uint64_t)s_jit_avg_cyc * 1000) / cpm));
    printf("[perf] 对照方法：先 uvc off，再跑一次 perf；两次空闲率之差 = 视频流占用\n");
}
