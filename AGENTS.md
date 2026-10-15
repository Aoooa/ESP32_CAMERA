# AGENTS.md — Camera_test_tool

ESP32-S3 OV5640 摄像头固件（ESP-IDF v5.5.3）。不含 LCD / 触摸 / UART1 / UART2 / WiFi / LVGL。
视频出口为 **UVC 标准 USB 摄像头 + CDC-ACM 串口复合设备**：一根 USB 线同时提供摄像头与命令/日志口。
本工程为**独立仓库**，与 `D:\Task\embedded_debugger_tool`（LVGL 调试工具）无关联。

## 索引

| 路径 | 内容 |
|---|---|
| `main/main.c` | 初始化顺序（nvs → SD → camera → con 任务）、console 命令解析、两段式下载模式入口 |
| `video/app_video.h` | **与传输无关的视频帧源**：open/get/release、开关、质量、统计 |
| `video/app_video.c` | 帧源实现（只与 drv_camera 交互，单任务串行访问） |
| `usb_uvc/app_usb_uvc.h` | UVC 传输适配层：启动、CDC 串口（命令/日志）、`APP_USB_UVC_FEED_IDLE_MS` |
| `usb_uvc/app_usb_uvc.c` | 4 个 UVC 回调（帧取自 app_video）+ 空闲保护 + CDC/stdout 重定向 |
| `perf/app_perf.h` | CPU 余量/调度迟延探针接口 |
| `perf/app_perf.c` | 探针实现（`perf` 命令） |
| `camera/drv_camera.h` | 板级引脚、DVP 引脚冲突、格式与线程模型 |
| `camera/app_camera.c` | 取图、落盘、base64 外发、统计、寄存器开关重放 |
| `sdcard/drv_sdcard.c` | SD 挂载/卸载 + 文件读写 |
| `components/usb_device_uvc/` | Espressif UVC 组件 v1.3.1（离线 vendor，含两处修复） |
| `components/esp32-camera/` | esp32-camera 2.1.8（离线 vendor，含 PSRAM DMA EOI 修复） |
| `components/espressif__tinyusb/` | TinyUSB 0.19.0~3（UVC 依赖） |
| `scripts/uvc_view.py` | PC 侧实时显示（OpenCV，`CAP_MSMF`） |
| `scripts/idf.bat` | ESP-IDF 环境包装（本机唯一可用构建入口） |
| `scripts/cam_cmd.py` / `cam_dump.py` | 向 console 发命令 / 抓帧落 JPG（串口路径，非 UVC 模式） |
| `tools/raw_capture.py` / `check_jpeg.py` | 原始串口流抓取与离线完整性分析 / JPEG 结构与像素统计 |
| `docs/` | 早期摄像头验证记录 |
| `reference/ESP-IDF/` | 官方例程：`01_sd_card_test`、`05_lvgl_camera` |
| `reference/Arduino/` | 官方例程：`03_sd_card_test`、`09_lvgl_camera`、`10_camera_web_server` |
| `reference/ESP32-S3-Touch-LCD-2-SchDoc.pdf` | 板子原理图 |

分层：`main → video ← usb_uvc`；`main → app_camera → drv_camera + drv_sdcard`；`main → perf`。无 platform/。

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

## console 命令（经复合设备的 CDC）

`help` `stat` `info` `ls [dir]` `sd on|off` `clear` `reboot` `uvc on|off` `quality <0-63>` `perf` `boot`

- `uvc on|off`：开/关视频流。off 停止出帧并释放摄像头（DMA / 帧缓冲 / CPU）；on 按上次协商的分辨率重开。开关状态只由该命令改变。
- `quality <0-63>`：JPEG 质量，运行时生效；只改变每帧字节数，不改变帧率。
- `perf`：测空闲 CPU 余量与 10ms 周期任务迟延。对照方法：先 `uvc off` 再跑一次，两次空闲率之差即视频流占用。
- `boot`：主机主动请求进入 ROM 下载模式，**仅由该命令触发**。
- UVC 模式下属 UVC 任务独占相机，故拒绝：`cam` `shot` `burst` `dump` `size` `mirror` `flip` `bar`。

其余命令与分辨率名（`drv_camera.c` 的 `s_sizes[]`，或 `640x480` 直写）：
`96x96 qqvga 128x128 qcif hqvga 240x240 qvga 320x320 cif hvga vga svga xga hd sxga uxga fhd p_hd p_3mp qxga qhd wqxga p_fhd qsxga 5mp`

## 构建 / 烧录 / 调试

```bash
cmd //c "D:/Task/ESP32_CAMERA/scripts/idf.bat" build
cmd //c ".../scripts/idf.bat" -p COM14 monitor      # 退出前 Ctrl+]
python scripts/cam_cmd.py --port COM14 "stat"
python scripts/cam_dump.py --port COM14 -o out/cam.jpg
```

- `scripts/idf.bat` 已导出 `ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011`，并 `cd` 到本工程根目录；绕过该包装器直接调 `idf.py` 会因 PATH 与该变量缺失失败。
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
| 仓库根 | `D:\Task\ESP32_CAMERA`（独立仓库） |
| 远端 | `git@github.com:Aoooa/ESP32_CAMERA.git` |
| 分支 | `main`（稳定线）、`dev`（开发线） |
| 提交信息 | Conventional Commits：`<type>(<scope>): <subject>` |
| committer | `ESP32-Dev <dev@local>`（`git -c user.name=... -c user.email=... commit`） |

`.gitignore` 排除：`build/`、`sdkconfig`、`sdkconfig.old`、`sdkconfig.bak_console`、
`dependencies.lock`、`managed_components/`、`out/`、`components/espressif__tinyusb/test/fuzz/**/*.zip`。

## 约束

1. **USB PHY 独占**：USB-OTG（UVC+CDC）与 USB-Serial/JTAG 只能二选一。启用 UVC 后 PC 上没有 USB-Serial/JTAG 串口，只有复合设备的 COM 口。
2. **`boot` 必须两段式**：命令侧只置 `RTC_NOINIT` 标志并 `esp_restart()`；`app_main` 最早期执行 `usb_dc_prepare_persist()` → `chip_usb_set_persist_flags(USBDC_PERSIST_ENA)` → `REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT)` → `esp_restart()`。顺序与 IDF 的 `esp_system/port/usb_console.c: esp_usb_console_before_restart` 一致。ROM 的 USB 函数假定 ROM 独占控制器，在 TinyUSB 已初始化后调用会死锁，故不能在命令上下文执行。
3. 板子曾进入下载模式时 `RTC_CNTL_FORCE_DOWNLOAD_BOOT` 位会保留；此时需 `esptool ... --after watchdog_reset read_mac` 启动应用，`--after hard_reset` 不清该位。
4. **CDC 的 VFS 写回调必须始终返回完整长度**：`esp_restart()` 会 flush 控制台，返回短计数会使 newlib 反复重试而卡死命令。
5. **空闲保护**：主机停止取帧超过 `APP_USB_UVC_FEED_IDLE_MS` 后 `on_fb_get` 返回 NULL，避免把传输/端点留给下一次会话一个坏状态。判据用组件的完成帧计数，不能用 `tud_video_n_streaming()`（bulk 模式下主机设过 alt=1 后恒为 true）。
6. **视频流开关归属**：`app_video_open()` 不修改 enabled，开关状态只由 `uvc on|off` 改变。
7. `components/usb_device_uvc` 的 `video_task`：`tx_busy` 只在 `tud_video_n_frame_xfer()` 返回 true 时置位，等完成回调带 200ms 超时。
8. `components/esp32-camera` PSRAM 分支：EOI 窄窗口搜索失败后回退全缓冲反向搜索（上游 v2.1.1~v2.1.8 缺此回退）。`CONFIG_CAMERA_PSRAM_DMA=y` 依赖该修复。
9. **帧源/传输分层**：帧获取、分辨率协商、开关、质量、统计在 `video/app_video`；接入新的外部通信方式只增加 `usb_uvc` 形式的适配层。
10. **`perf` 不使用 FreeRTOS 运行时间统计**：只读 `esp_cpu_get_cycle_count()`；探针钉 core 1、优先级 1（低于 UVC 任务 4/5、高于 idle），在固定 CPU 周期窗口内自旋计数，并在 `app_main` 早期标定满速基准。测量范围是该核，`cam_task` 钉在 core 0。
11. `IO2/IO4/IO16/IO17` 与 Embedded_debug_tool 的 UART1/UART2 冲突 → 本工程不得启用那两路 UART 与 LCD。
12. PSRAM 必须开（帧缓冲在 PSRAM）；`PIXFORMAT_RGB565` 需 `CONFIG_CAMERA_CONVERTER_ENABLED`，未启用。
13. SD 的 SPI2 总线由 `drv_sdcard_init()` 自行 `spi_bus_initialize()`。
14. 改分辨率走 `esp_camera_reconfigure()`（deinit+init），传感器寄存器回默认 → 新增开关须加入 `app_camera.c: replay_sensor_opts()`。
15. `dump` 标记行 `SET=… / B64-BEGIN … / B64-END …` 必须顶行、无前缀；dump 期间 `esp_log_level_set("*", ESP_LOG_NONE)`；完整性只认 len + CRC32。
16. 落盘序号为内存态、重启归零 → 首次落盘前 `seed_file_seq_from_card()` 扫目录续号。
17. 脚本连 console 不得翻转 DTR/RTS（会复位板子）；抓输出用 `cam_cmd.py --log <file>`。
18. 摄像头/SD/console 非线程安全：接口只在 console 任务串行调用；UVC 模式下相机归 UVC 任务独占。

## 引脚

DVP（`camera/drv_camera.h`）：`PWDN=17 RESET=-1 XCLK=8 SIOD=21 SIOC=16
D0..D7=12,13,15,11,14,10,7,2 VSYNC=6 HREF=4 PCLK=9`，XCLK 20MHz。
SD（`sdcard/drv_sdcard.h`）：`MOSI=38 SCLK=39 MISO=40 CS=41`（SPI2）。
