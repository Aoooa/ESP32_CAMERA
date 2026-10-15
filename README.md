# Camera_test_tool —— ESP32-S3 OV5640 摄像头固件

Waveshare **ESP32-S3-Touch-LCD-2** 上的独立固件，只做「摄像头驱动 + 图像获取 + 落 SD 卡 + USB 输出」，
不带 UI、不带网络。与 `../Embedded_debug_tool`（LCD/触摸/串口桥/示波器）互不影响。

## 视频输出：USB 复合设备（UVC 摄像头 + CDC 串口）

固件把 OV5640 的 MJPEG 当**标准 USB 摄像头**枚举，并复合一个 **CDC-ACM 串口**：
一根 USB 线同时得到摄像头和命令/日志口（PC 上表现为「一个摄像头 + 一个 COM 口」）。

```bash
python scripts/uvc_view.py                            # 实时显示（q/ESC 退出，s 存图）
python scripts/uvc_view.py --no-window --seconds 8    # 帧率自检
```

OpenCV 后端必须用 `CAP_MSMF`（`CAP_DSHOW` 打不开本设备）。设备同时是标准 UVC 设备，
系统自带相机程序可直接打开。

| 项 | 值 |
|---|---|
| 接口 | UVC 控制/流 0/1（class `0x0e`）+ CDC 控制/数据 2/3（`0x02`/`0x0a`） |
| 端点 | 视频 IN `0x81`；CDC 通知 `0x83`、数据 IN `0x84` / OUT `0x04` |
| 传输 | bulk，视频端点包长 64 |
| 分辨率/帧率 | `320x240@30` `480x320@30` `640x480@60` `1280x720@15` |
| 格式 | MJPEG（OV5640 片内编码），质量 0-63 运行时可调 |
| CDC 串口 | 115200（波特率对 CDC 无约束） |

## 图像获取路径

`esp32-camera` + `PIXFORMAT_JPEG`：JPEG 由 OV5640 片内编码器完成（`driver/sensor.c` 中
OV5640 `support_jpeg=true`），DVP 总线上直接跑压缩码流；ESP32-S3 侧只做 LCD_CAM 采集 + DMA，
PSRAM DMA 模式下帧直接落入 PSRAM 帧缓冲，不做整帧拷贝。

`camera_fb_t` 的出路：

1. **USB 输出**：`video/app_video` 提供帧，`usb_uvc/app_usb_uvc` 作为 UVC 适配层送出
2. **落 SD 卡**：`/sdcard/cam/cam_<分辨率>_<序号>.jpg`（FatFs，SPI2）
3. **串口回传**（非 UVC 模式）：`dump` 命令把 JPEG 以 base64 从 console 外发，PC 侧还原 `.jpg`

固件校验 JPEG 头尾 `FFD8 … FFD9`；`dump` 另附长度 + CRC32，PC 脚本两者都对上才落盘。

## console 命令

UVC 模式下 console 走复合设备的 CDC；串口路径（`CAMERA` 直连）用 `scripts/cam_cmd.py`。

| 命令 | 作用 |
|---|---|
| `help` | 命令一览 |
| `stat` | 传感器型号/PID、当前分辨率与质量、累计帧统计、内部 RAM/PSRAM、SD 剩余空间 |
| `info` | `sensor->status` 各开关当前值（AWB/AEC/AGC/lenc/…） |
| `uvc on\|off` | 开/关视频流；off 停止出帧并释放摄像头（DMA/帧缓冲/CPU），on 按上次分辨率重开 |
| `quality <0-63>` | JPEG 量化档，运行时生效（越小越清晰、字节越多） |
| `perf` | CPU 空闲余量与 10ms 周期任务迟延；先 `uvc off` 再跑一次，两次之差即视频流占用 |
| `boot` | 主机主动请求进入 ROM 下载模式，随后可用 esptool 烧录 |
| `ls [目录]` / `sd on\|off` | 列 SD 目录 / 挂载卸载 SD |
| `clear` / `reboot` | 清零累计统计 / 重启 |
| `cam` `shot` `burst` `dump` `size` `mirror` `flip` `bar` | 摄像头操作；**UVC 模式下属 UVC 任务独占相机，这些命令被拒绝** |

分辨率名（`camera/drv_camera.c` 的 `s_sizes[]`，或 `640x480` 直写）：
`96x96 qqvga 128x128 qcif hqvga 240x240 qvga 320x320 cif hvga vga svga xga hd sxga uxga fhd p_hd p_3mp qxga qhd wqxga p_fhd qsxga 5mp`

### 把图像取回 PC（串口路径）

```bash
python scripts/cam_dump.py --port COM14 -o out/cam.jpg
python scripts/cam_dump.py --port COM14 --size hd -o out/hd.jpg
python scripts/cam_dump.py --port COM14 --bar -o out/bar.jpg      # 传感器彩条，验数据通路
python scripts/cam_dump.py --port COM14 --frames 3 --outdir out
```

脚本在一次串口会话里先发可选的 `size/quality/bar`，再发 `dump`，按顶行标记切出每帧 base64
（`SET=… / B64-BEGIN seq=… len=… crc=… / … / B64-END …`），解码后核对长度 + CRC32 才落盘，
不过校验会自动重抓（`--retries`，默认 3）。

## 构建 / 烧录

```bash
cmd //c "D:/Task/ESP32_CAMERA/scripts/idf.bat" build
```

- 改 `sdkconfig` 触发重配置时先设 `ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011`。
- `sdkconfig.defaults` 改动需 `fullclean && build`。
- 烧录（ROM 下载模式下，`flash_args` 在 `build/`，路径相对 `build/` 解析）：

```bash
/C/Espressif/tools/python/v5.5.3/venv/Scripts/python.exe -m esptool \
    --chip esp32s3 -p COM14 -b 460800 --before default_reset --after hard_reset \
    write_flash "@flash_args"
```

- 进入 ROM 下载模式：`boot` 命令（主机侧，无需按键），或按住 BOOT + 点 RST。
- 启动应用：`esptool ... --after watchdog_reset read_mac`（`--after hard_reset` 不清
  `RTC_CNTL_FORCE_DOWNLOAD_BOOT` 位）。

## 引脚

DVP（`camera/drv_camera.h`）：

```
PWDN=17   RESET=-1(软复位)   XCLK=8    SIOD=21(SDA)   SIOC=16(SCL)
D0=12  D1=13  D2=15  D3=11  D4=14  D5=10  D6=7  D7=2
VSYNC=6  HREF=4  PCLK=9      XCLK 频率 = 20MHz
```

| 摄像头信号 | GPIO | 在 Embedded_debug_tool 里的用途 |
|---|---|---|
| D9 (Y9) | IO2 | UART1 TX（XFR 高速口） |
| HREF | IO4 | UART1 RX / BAT_ADC |
| SIOC (SCL) | IO16 | UART2 TX |
| PWDN | IO17 | UART2 RX |
| 其余 D0-D7 / VSYNC / PCLK / XCLK / SIOD | IO6-IO15、IO21 | — |

因此本工程**不初始化 LCD / 触摸 / UART1 / UART2 / WiFi**，把 IO2/4/6-17/21 全留给摄像头。
SD 卡走 SPI2（`MOSI=IO38 / SCLK=IO39 / MISO=IO40 / CS=IO41`），与 DVP 不冲突，可与摄像头同时工作。

## 组件（离线内置）

`components/` 内置官方组件源码，构建不需要联网：

| 组件 | 版本 | 说明 |
|---|---|---|
| `esp32-camera` | 2.1.8 | 摄像头驱动（含 OV5640）；已移除其 `idf_component.yml`，并把 `esp_jpeg` 写进 `REQUIRES`（`esp_camera.h` 末尾 include `img_converters.h`，故须 PUBLIC） |
| `esp_jpeg` | 1.3.1 | JPEG 解码（`jpg2rgb565` 等接口依赖）；本工程不解码，仅为满足编译依赖 |
| `usb_device_uvc` | 1.3.1 | Espressif UVC 设备组件（含 CDC 复合与本工程的两处修复） |
| `espressif__tinyusb` | 0.19.0~3 | UVC 依赖的 USB 协议栈 |
| `cmake_utilities` | 1.1.1 | 组件构建辅助 |

## 目录结构

```
Camera_test_tool/
├── CMakeLists.txt  partitions.csv  sdkconfig.defaults  AGENTS.md  README.md
├── main/            main.c：初始化顺序 + console 任务 + 两段式下载模式入口
├── video/           app_video.*：与传输无关的视频帧源（open/get/release/开关/质量/统计）
├── usb_uvc/         app_usb_uvc.*：UVC 传输适配层 + CDC 串口（命令/日志/stdout 重定向）
├── perf/            app_perf.*：CPU 余量与调度迟延探针
├── camera/          drv_camera.*（板级封装）、app_camera.*（取图/落盘/外发/统计）
├── sdcard/          drv_sdcard.*：SPI2 总线 + SD 挂载 + 文件读写
├── components/      离线内置组件（见上）
└── scripts/         idf.bat / uvc_view.py / cam_cmd.py / cam_dump.py
```

依赖方向：`drv_` → ESP-IDF API；`app_` → `drv_`；`video ← usb_uvc`；`main` → 各服务。无 `platform/`、无 LVGL。

## 约束与边界

- **USB PHY 独占**：USB-OTG（UVC+CDC）与 USB-Serial/JTAG 只能二选一；启用 UVC 时 PC 上没有
  USB-Serial/JTAG 串口，只有复合设备的 COM 口。
- **`boot` 为两段式**：命令侧只置 RTC 标志并重启；持久化与置下载位在 `app_main` 最早期执行
  （ROM 的 USB 函数假定独占控制器，在 TinyUSB 初始化后调用会死锁）。
- 摄像头/SD/console 非线程安全：接口只在 console 任务串行调用；UVC 模式下相机归 UVC 任务。
- **无 LCD 实时预览**：DVP 采集 + SPI2（SD/LCD 共用）同时跑会互相抢带宽，且显示缓冲需内部 DMA RAM。
- **`PIXFORMAT_RGB565` 未启用**：需 `CONFIG_CAMERA_CONVERTER_ENABLED`；本工程走传感器硬 JPEG。
- **OV5640 自动对焦未启用**（`CAMERA_AF_SUPPORT` 默认关）。
- **`dump` 走串口**：大分辨率码流几十到上百 KB，base64 回传需数十秒；要成批大图优先 `shot` 落 SD。
