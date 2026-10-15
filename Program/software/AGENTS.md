# AGENTS.md — Camera_test_tool

ESP32-S3 OV5640 摄像头固件（ESP-IDF v5.5.3）。含**本地 LCD 显示**（自实现显示层，不用 LVGL）；不含触摸 / UART1 / UART2 / WiFi。
视频出口为 **UVC 标准 USB 摄像头 + CDC-ACM 串口复合设备**：一根 USB 线同时提供摄像头与命令/日志口；本机 LCD 显示**同一路视频流**。

## 索引

| 路径 | 内容 |
|---|---|
| `main/main.c` | 初始化顺序（nvs → SD → LCD → 帧泵 → UVC）、console 命令解析、两段式下载模式入口 |
| `video/app_video.h` | **与传输无关的视频帧源**：帧泵（唯一取帧者）、分辨率请求、开关、质量、统计 |
| `video/app_video.c` | 帧源实现（只与 drv_camera 交互）+ 帧泵任务（取帧 → 发布到帧总线） |
| `video/frame_bus.h` | **单生产者多消费者帧槽位**：4 槽 PSRAM，最新帧语义，引用计数 |
| `video/frame_bus.c` | 槽位实现（发布/获取/归还 + 统计） |
| `lcd/drv_lcd.h` | ST7789 面板：竖屏 240x320、SPI2 80MHz、MX 镜像、背光、互斥锁 |
| `lcd/drv_lcd.c` | 面板初始化 + `drv_lcd_draw_strip()`（唯一绘制入口，线程安全） |
| `lcd/app_lcd.h` | **LCD 消费者适配层**：帧总线 → 解码 → 贴屏；测试图案/基准 |
| `lcd/app_lcd.c` | ROM tjpgd 解码 + 条带式贴屏 + 统计 + `lcd fill/test/bench` |
| `bsp/bus_spi2.h` | SPI2 共享总线（SD 与 LCD 共用 IO38/39/40，引用计数） |
| `bsp/bus_spi2.c` | 总线初始化/释放（只初始化一次，最后一个用户释放） |
| `usb_uvc/app_usb_uvc.h` | UVC 传输适配层：启动、CDC 串口（命令/日志）、`APP_USB_UVC_FEED_IDLE_MS` |
| `usb_uvc/app_usb_uvc.c` | 4 个 UVC 回调（帧取自 frame_bus）+ 空闲保护 + CDC/stdout 重定向 |
| `perf/app_perf.h` | CPU 余量/调度迟延探针接口 |
| `perf/app_perf.c` | 探针实现（`perf` 命令） |
| `camera/drv_camera.h` | 板级引脚、DVP 引脚冲突、格式与线程模型 |
| `camera/app_camera.c` | 取图、落盘、base64 外发、统计、寄存器开关重放 |
| `sdcard/drv_sdcard.c` | SD 挂载/卸载（SPI2 经 bus_spi2 共享）+ 文件读写 |
| `components/usb_device_uvc/` | Espressif UVC 组件 v1.3.1（离线 vendor，含两处修复） |
| `components/esp32-camera/` | esp32-camera 2.1.8（离线 vendor，含 PSRAM DMA EOI 修复） |
| `components/esp_jpeg/` | esp_jpeg（ROM tjpgd 解码器 + `tjpgd/` 源码） |
| `components/espressif__tinyusb/` | TinyUSB 0.19.0~3（UVC 依赖） |
| `scripts/uvc_view.py` | PC 侧实时显示（OpenCV，`CAP_MSMF`） |
| `scripts/idf.bat` | ESP-IDF 环境包装（本机唯一可用构建入口） |
| `scripts/cam_cmd.py` / `cam_dump.py` | 向 console 发命令 / 抓帧落 JPG（串口路径，非 UVC 模式） |
| `tools/raw_capture.py` / `check_jpeg.py` | 原始串口流抓取与离线完整性分析 / JPEG 结构与像素统计 |
| `docs/` | 早期摄像头验证记录 |
| `reference/ESP-IDF/` | 官方例程：`01_sd_card_test`、`05_lvgl_camera` |
| `reference/Arduino/` | 官方例程：`03_sd_card_test`、`09_lvgl_camera`、`10_camera_web_server` |
| `reference/ESP32-S3-Touch-LCD-2-SchDoc.pdf` | 板子原理图 |

分层：`main → video(app_video 帧泵) → frame_bus ← {usb_uvc, app_lcd}`；
`app_lcd → drv_lcd → bus_spi2 ← drv_sdcard`；`main → app_camera → drv_camera + drv_sdcard`；`main → perf`。

## USB 复合设备（UVC + CDC）

| 项 | 值 |
|---|---|
| 接口 | UVC 控制/流 0/1（class `0x0e`）+ CDC 控制/数据 2/3（`0x02` / `0x0a`），IAD 齐全 |
| 端点 | 视频 IN `0x81`；CDC 通知 `0x83`、数据 IN `0x84` / OUT `0x04` |
| 传输模式 | bulk（`CONFIG_UVC_MODE_BULK_CAM1=y`），视频端点包长 64 |
| 分辨率/帧率 | `320x240@30` `480x320@30` `640x480@60` `1280x720@15`（帧间隔由 `CONFIG_UVC_CAM1_FRAMERATE` 决定） |
| 格式 | MJPEG（OV5640 片内编码），`quality` 0-63 运行时生效 |
| CDC 串口 | 115200，波特率对 CDC 无约束 |

PC 侧查看：

```bash
python scripts/uvc_view.py                            # 实时显示（q/ESC 退出，s 存图）
python scripts/uvc_view.py --no-window --seconds 8    # 帧率自检
```

OpenCV 后端必须用 `CAP_MSMF`；`CAP_DSHOW` 打不开本设备。设备同时是标准 UVC 设备，系统相机可直接打开。

## 本地显示（LCD）

| 项 | 值 |
|---|---|
| 面板 | ST7789 **240x320 竖屏**，SPI2 80MHz，`swap_xy(false)` + `mirror(true,false)` + `invert_color(true)` |
| 引脚 | SCLK=IO39 MOSI=IO38 MISO=IO40（与 SD 共用总线）、CS=IO45、DC=IO42、RST=-1、背光=IO1 |
| 显示缓冲 | 2 块 240x16 条带（**内部 DMA RAM**，各 7.5KB）ping-pong；另 1 块给测试图案 |
| 解码 | ROM TJpgDec（`rom/tjpgd.h`），自实现 out 回调输出 RGB565 大端 |
| 帧映射 | 解码后**水平居中裁剪到 240 宽 + 垂直居中**；QVGA → 240x240 居中 + 上下各 40 行黑边 |
| 帧率 | 纯显示 56.5 fps；含 QVGA 解码 19.8 fps（解码 48ms） |

## console 命令（经复合设备的 CDC）

`help` `stat` `info` `ls [dir]` `sd on|off` `clear` `reboot` `uvc on|off` `quality <0-63>` `perf` `boot`
`lcd` `lcd on|off` `lcd fill [r|g|b|w|k]` `lcd test` `lcd bench [n]`

- `uvc on|off`：开/关视频流。off 停止出帧并释放摄像头（DMA / 帧缓冲 / CPU）；on 按上次协商的分辨率重开。开关状态只由该命令改变。
- `lcd`：显示统计。`lcd on|off`：显示任务开关（off 不再解码，面板保持点亮）。
- `lcd fill [r|g|b|w|k]`：全屏纯色；无参依次红→绿→蓝→白→黑各 0.8s。
- `lcd test`：4 条横带（上→下 红/绿/蓝/白）+ SPI 带宽。`lcd bench [n]`：全屏动画刷 n 帧（默认 200）报帧率/带宽。
- 上述三个测试命令会暂停显示任务并保持图案，恢复用 `lcd on`。
- `quality <0-63>`：JPEG 质量，运行时生效；只改变每帧字节数，不改变帧率。
- `perf`：测空闲 CPU 余量与 10ms 周期任务迟延。对照方法：先 `uvc off` 再跑一次，两次空闲率之差即视频流占用。
- `boot`：主机主动请求进入 ROM 下载模式，**仅由该命令触发**。
- UVC 模式下属 UVC 任务独占相机，故拒绝：`cam` `shot` `burst` `dump` `size` `mirror` `flip` `bar`。

其余命令与分辨率名（`drv_camera.c` 的 `s_sizes[]`，或 `640x480` 直写）：
`96x96 qqvga 128x128 qcif hqvga 240x240 qvga 320x320 cif hvga vga svga xga hd sxga uxga fhd p_hd p_3mp qxga qhd wqxga p_fhd qsxga 5mp`

## 构建 / 烧录 / 调试

```bash
cmd //c "D:/Task/ESP32_CAMERA/Program/software/scripts/idf.bat" build
cmd //c ".../scripts/idf.bat" -p COM14 monitor      # 退出前 Ctrl+]
python scripts/cam_cmd.py --port COM14 "stat"
python scripts/cam_dump.py --port COM14 -o out/cam.jpg
```

- `scripts/idf.bat` 已导出 `ESP_ROM_ELF_DIR` 并 `cd` 到本工程根目录；绕过包装器直接调 `idf.py` 会失败。
- `sdkconfig.defaults` 改动需 `fullclean && build`；`fullclean` 后先 build 再 flash。
- 烧录：ROM 下载模式下在 `build/` 执行 `esptool ... write_flash "@flash_args"`，随后 `--after watchdog_reset` 启动应用。
- 主机侧流程：`boot` 后等约 3 秒再连接 esptool；进入下载模式的判据是出现 PID `0x0009` 的串口。

```powershell
# COM 口被 monitor 残留占用时
Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
  Where-Object { $_.CommandLine -match 'monitor' } | Stop-Process
```

## 仓库

| 项 | 值 |
|---|---|
| 仓库根 | `D:\Task\ESP32_CAMERA` |
| 工程根 | `D:\Task\ESP32_CAMERA\Program\software` |
| 远端 | `git@github.com:Aoooa/ESP32_CAMERA.git` |
| 分支 | `main`（稳定线）、`dev`（开发线） |
| committer | `ESP32-Dev <dev@local>` |

## 约束

1. **USB PHY 独占**：USB-OTG（UVC+CDC）与 USB-Serial/JTAG 只能二选一。启用 UVC 后 PC 上没有 USB-Serial/JTAG 串口，只有复合设备的 COM 口。
2. **`boot` 必须两段式**：命令侧只置 `RTC_NOINIT` 标志并 `esp_restart()`；`app_main` 最早期执行 `usb_dc_prepare_persist()` → `chip_usb_set_persist_flags(USBDC_PERSIST_ENA)` → `REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT)` → `esp_restart()`。顺序与 IDF 的 `esp_system/port/usb_console.c: esp_usb_console_before_restart` 一致。ROM 的 USB 函数假定 ROM 独占控制器，在 TinyUSB 已初始化后调用会死锁，故不能在命令上下文执行。
3. 板子曾进入下载模式时 `RTC_CNTL_FORCE_DOWNLOAD_BOOT` 位会保留；此时需 `esptool ... --after watchdog_reset read_mac` 启动应用，`--after hard_reset` 不清该位。
4. **CDC 的 VFS 写回调必须始终返回完整长度**：`esp_restart()` 会 flush 控制台，返回短计数会使 newlib 反复重试而卡死命令。
5. **空闲保护**：主机停止取帧超过 `APP_USB_UVC_FEED_IDLE_MS` 后 `on_fb_get` 返回 NULL，避免把传输/端点留给下一次会话一个坏状态。判据用组件的完成帧计数，不能用 `tud_video_n_streaming()`（bulk 模式下主机设过 alt=1 后恒为 true）。
6. **视频流开关归属**：`app_video_open()` 不修改 enabled，开关状态只由 `uvc on|off` 改变。
7. `components/usb_device_uvc` 的 `video_task`：`tx_busy` 只在 `tud_video_n_frame_xfer()` 返回 true 时置位，等完成回调带 200ms 超时。
8. `components/esp32-camera` PSRAM 分支：EOI 窄窗口搜索失败后回退全缓冲反向搜索（上游 v2.1.1~v2.1.8 缺此回退）。`CONFIG_CAMERA_PSRAM_DMA=y` 依赖该修复。
9. **帧源/传输分层**：帧获取、分辨率协商、开关、质量、统计在 `video/app_video`（帧泵是唯一取帧者）；消费者只通过 `frame_bus` 取帧，新增外部通信方式只增加 `usb_uvc` 形式的适配层。
10. **`perf` 不使用 FreeRTOS 运行时间统计**：只读 `esp_cpu_get_cycle_count()`；探针钉 core 1、优先级 1（低于 UVC 任务 4/5、高于 idle），在固定 CPU 周期窗口内自旋计数，并在 `app_main` 早期标定满速基准。测量范围是该核，`cam_task` 钉在 core 0。
11. `IO2/IO4/IO16/IO17` 是摄像头 DVP 引脚，与 Embedded_debug_tool 的 UART1/UART2 冲突 → 本工程不得启用那两路 UART。LCD 已启用（见 19-26）；触摸（I2C 47/48）与 UART1/2 未启用。
12. PSRAM 必须开（帧缓冲在 PSRAM）；`PIXFORMAT_RGB565` 需 `CONFIG_CAMERA_CONVERTER_ENABLED`，未启用。
13. SPI2 总线由 `bsp/bus_spi2` 按引用计数统一管：SD 与 LCD 各自 acquire/release，**只有最后一个用户释放时才 `spi_bus_free()`**，谁也不能自己 `spi_bus_initialize()`。
14. 改分辨率走 `esp_camera_reconfigure()`（deinit+init），传感器寄存器回默认 → 新增开关须加入 `app_camera.c: replay_sensor_opts()`。
15. `dump` 标记行 `SET=… / B64-BEGIN … / B64-END …` 必须顶行、无前缀；dump 期间 `esp_log_level_set("*", ESP_LOG_NONE)`；完整性只认 len + CRC32。
16. 落盘序号为内存态、重启归零 → 首次落盘前 `seed_file_seq_from_card()` 扫目录续号。
17. 脚本连 console 不得翻转 DTR/RTS（会复位板子）；抓输出用 `cam_cmd.py --log <file>`。UVC 模式下 console 是 USB CDC，`tud_cdc_n_connected()` 依赖 DTR=1；`cam_cmd.py` 默认 `dtr=False`，在该模式下收不到命令。
18. 摄像头非线程安全：只有帧泵任务调 `app_video_get()`；UVC/LCD 都不直接碰 drv_camera。分辨率请求走 `app_video_request_size()`（帧泵上下文生效）。
19. **LCD 与 SD 共用 SPI2**：IO38/39/40 是同一组引脚，CS 各自独立（SD=41、LCD=45）。总线 `max_transfer_sz` 取 16KB，能装下一条 240x16 条带（7.5KB）。
20. **esp_lcd 的 SPI panel IO 不是线程安全的**（`trans_pool`/`num_trans_inflight` 是共享状态）：所有绘制必须经 `drv_lcd_draw_strip()`（内部互斥锁），不能在多个任务里直接调 `esp_lcd_panel_draw_bitmap()`。
21. **`jd_decomp(jd, out, scale)` 的 `scale` 是 log2 除数**（0=1/1、1=1/2、2=1/4、3=1/8），不是除数本身（`tjpgd.c`: `jd->scale = scale; rx >>= jd->scale; x >>= jd->scale`）；不要传 1/2/4/8。
22. **TJpgDec 的 `JDEC.device` 只有一个字段、输入与输出回调共用** → 必须传同一个上下文结构（`lcd_jpeg_ctx_t`）。
23. **TJpgDec 的 out 回调是"每个 MCU 调一次"**，`rect` 是输出图里的绝对矩形（`tjpgd.c: mcu_output`）→ 按 `rect->top/left` 绝对定位；裁剪窗口两端都要夹到 `[0, ncols]`。
24. ST7789 大端字节序（RAMCTRL 0xf0）：strip 缓冲每像素**高字节在前**；面板 `invert_color(true)`；摄像头画面左右相反，用 `mirror(true,false)` 的 MX 位镜像。
25. 摄像头帧与面板比例不同（320x240 → 240x320）：按 `lcd/app_lcd.c` 的方式水平居中裁剪到 240 宽、垂直居中；QVGA 得 240x240 + 上下黑边，黑边只在几何变化或恢复显示时清一次。
26. LCD 任务钉 core 0、优先级 3（低于 UVC 的 4/5）。换分辨率经 `esp_camera_reconfigure()` 重开相机（~150ms），显示端几何随之变化。

## 引脚

DVP（`camera/drv_camera.h`）：`PWDN=17 RESET=-1 XCLK=8 SIOD=21 SIOC=16
D0..D7=12,13,15,11,14,10,7,2 VSYNC=6 HREF=4 PCLK=9`，XCLK 20MHz。
SD（`sdcard/drv_sdcard.h`）：`MOSI=38 SCLK=39 MISO=40 CS=41`（SPI2）。
LCD（`lcd/drv_lcd.h`）：`SCLK=39 MOSI=38 MISO=40`（与 SD 同一组 SPI2 引脚）、`CS=45 DC=42 RST=-1 背光=1`。
未使用的板级外设：触摸 CST816S（I2C0 `SCL=47 SDA=48`）、UART1（`IO2/IO4`）、UART2（`IO16/IO17`）——前两路与摄像头 DVP 冲突。
