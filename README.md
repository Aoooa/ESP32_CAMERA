# Camera_test_tool —— ESP32-S3 OV5640 摄像头驱动与图像获取测试工程

同一块板子（Waveshare **ESP32-S3-Touch-LCD-2**）上的独立固件，与
`../Embedded_debug_tool`（LCD/触摸/串口桥/示波器那套）**互不影响**：本工程只做
「摄像头驱动 + 图像获取 + 落 SD 卡 + 回传 PC 验证」，不带 UI、不带网络。

## 为什么要单独开一个工程（引脚冲突）

板载 OV5640 走 **DVP 并口**，官方例程给出的引脚里包含了 `Embedded_debug_tool`
已经在用的串口引脚，同一份固件里两边不能共存：

| 摄像头信号 | GPIO | 在 Embedded_debug_tool 里的用途 |
|---|---|---|
| D9 (Y9) | IO2 | UART1 TX（XFR 高速口） |
| HREF | IO4 | UART1 RX / BAT_ADC |
| SIOC (SCL) | IO16 | UART2 TX |
| PWDN | IO17 | UART2 RX |
| D0 (Y2) | IO12 | — |
| D1 (Y3) | IO13 | — |
| D2 (Y4) | IO15 | — |
| D3 (Y5) | IO11 | — |
| D4 (Y6) | IO14 | — |
| D5 (Y7) | IO10 | — |
| D6 (Y8) | IO7 | — |
| D7 (Y9→IO2) | IO2 | 同第一行 |
| VSYNC | IO6 | — |
| PCLK | IO9 | — |
| XCLK | IO8 | — |
| SIOD (SDA) | IO21 | — |

所以本工程**不初始化 LCD / 触摸 / UART1 / UART2 / WiFi**，把 IO2/4/6-17/21 全留给摄像头。
SD 卡用 SPI2（MOSI=IO38 / SCLK=IO39 / MISO=IO40 / CS=IO41），与 DVP 不冲突，可以和摄像头同时工作。

> 注：`CLAUDE.md` 里 GPIO 表写的 SD = IO33/34/35/36 是过时信息。以官方
> `01_sd_card_test` 例程和 `Embedded_debug_tool/sdcard/drv_sdcard.c`（实机可用）为准。

引脚表的两处独立来源（互相印证，非推测）：
* `示例程序/ESP32-S3-Touch-LCD-2-Demo/ESP-IDF/05_lvgl_camera/main/main.c`
* `示例程序/ESP32-S3-Touch-LCD-2-Demo/Arduino/examples/10_camera_web_server/10_camera_web_server.ino`

## 完整 DVP 引脚（本工程 `camera/drv_camera.h`）

```
PWDN=17   RESET=-1(软复位)   XCLK=8    SIOD=21(SDA)   SIOC=16(SCL)
D0=12  D1=13  D2=15  D3=11  D4=14  D5=10  D6=7  D7=2
VSYNC=6  HREF=4  PCLK=9      XCLK 频率 = 20MHz
```

## 图像获取路径

`esp32-camera` 组件 + `PIXFORMAT_JPEG`：**JPEG 由 OV5640 片内编码器完成**
（`driver/sensor.c` 里 OV5640 `support_jpeg=true`），DVP 总线上直接跑压缩码流，
ESP32-S3 侧只做 LCD_CAM 采集 + DMA 到 PSRAM，不消耗 CPU 做编码。

取到的 `camera_fb_t` 有两条出路：

1. **落 SD 卡**：`/sdcard/cam/cam_<分辨率>_<序号>.jpg`（FatFs，SPI2）
2. **回传 PC**：`dump` 命令把 JPEG 以 base64 从调试 console（**COM14，原生 USB-Serial/JTAG**）
   外发，PC 侧脚本还原成 `.jpg`，直接看图验证 —— 不需要拆卡、不需要额外串口芯片

两条都带完整性校验：固件检查 JPEG 头尾 `FFD8 … FFD9`，`dump` 另附长度 + CRC32，
PC 脚本两者都对上才落盘。

## 构建

```bash
cmd //c "D:/Task/embedded_debugger_tool/Program/Camera_test_tool/scripts/idf.bat" build
```

## 烧录（COM14）

```bash
cmd //c "D:/Task/embedded_debugger_tool/Program/Camera_test_tool/scripts/idf.bat" -p COM14 flash
```

或直连 esptool（monitor 占口时用这条）：

```bash
/c/Espressif/tools/python/v5.5.3/venv/Scripts/python.exe -m esptool \
    --chip esp32s3 -p COM14 -b 460800 --before default_reset --after hard_reset \
    write_flash "@flash_args"
```

（`flash_args` 在 `build/` 下，路径相对 `build/` 解析。）

## 调试 console

主 console = 原生 USB-Serial/JTAG（PC 侧 COM14），单行命令 + 文本回显：

```bash
cmd //c "D:/Task/embedded_debugger_tool/Program/Camera_test_tool/scripts/idf.bat" -p COM14 monitor
# 退出前 Ctrl+]，否则 COM14 被占，下次烧录失败
```

脚本化（不做 DTR/RTS 翻转，不会复位板子）：

```bash
python scripts/cam_cmd.py --port COM14 "stat"
python scripts/cam_cmd.py --port COM14 --wait 10 "shot 3"
```

命令一览（`help`）：

| 命令 | 作用 |
|---|---|
| `stat` | 传感器型号/PID、当前分辨率与质量、累计帧统计、内部 RAM/PSRAM、SD 剩余空间 |
| `info` | `sensor->status` 各开关当前值（AWB/AEC/AGC/lenc/…） |
| `cam on/off` | 打开/关闭摄像头（off 会释放 LCD_CAM + DMA + 帧缓冲） |
| `shot [n]` | 抓 n 张并写 `/sdcard/cam/cam_<分辨率>_<序号>.jpg`（序号跨重启续号，不覆盖旧图） |
| `burst [n] [gap_ms]` | 连拍不落盘，只看帧率与字节数（用来分离「摄像头瓶颈」和「写卡瓶颈」） |
| `dump [n]` | 抓 n 帧并以 base64 从 COM14 外发 |
| `size <分辨率>` | `qvga/hvga/vga/svga/xga/hd/sxga/uxga/qxga/qsxga` 或 `640x480` |
| `quality <0-63>` | JPEG 量化档（越小越清晰） |
| `mirror/flip on/off` | 传感器水平镜像 / 垂直翻转 |
| `bar on/off` | 传感器**彩条**：不依赖镜头和光线，单独验证 DVP/DMA 数据通路 |
| `ls [目录]` | 列 SD 目录，确认图片真的落卡了 |
| `sd on/off` | 挂载 / 卸载 SD（卡接触不良时可重试而不用重启） |
| `clear` | 清零累计统计 |
| `reboot` | 重启 |

### 把图像取回 PC 看

```bash
python scripts/cam_dump.py --port COM14 -o out/cam.jpg
python scripts/cam_dump.py --port COM14 --size hd -o out/hd.jpg
python scripts/cam_dump.py --port COM14 --bar -o out/bar.jpg      # 传感器彩条，验数据通路
python scripts/cam_dump.py --port COM14 --frames 3 --outdir out
```

脚本在**一次串口会话**里先发可选的 `size/quality/bar`，再发 `dump`，按顶行标记切出每帧
base64 区间（`SET=… / B64-BEGIN seq=… len=… crc=… / … / B64-END seq=…`），解码后核对
**长度 + CRC32** 才落盘，并打印 JPEG 头尾（`ffd8…ffd9`）；不过校验会自动重抓（`--retries`，默认 3）。

固件侧两条相关约束（都是实测踩出来的）：

* `dump` 期间 `esp_log_level_set("*", ESP_LOG_NONE)` 全系统静音 —— stdout 一旦有第二个写者，
  newlib 的缓冲会在任意字节边界交错，把码流打断。
* 标记行必须**顶行、不带 `[cam] ` 这类前缀**：`cam` 三个字母本身是合法 base64 字符，
  带前缀会让每帧凭空多出 3 个字符（长度校验差 2 字节）。

> 主 console 是 USB-Serial/JTAG，**没有流控**：大帧（QSXGA 实测 ≈123KB → base64 ≈165KB）
> 传输要几十秒，固件每 ~2KB 主动 `fflush` + 让出一个 tick，PC 侧再靠校验+重试兜底。
> 要成批大图请优先 `shot` 落 SD 卡。

## 组件（离线内置）

`components/` 里直接放了两个官方组件的源码，**构建不需要联网**（本机 python 出网被防火墙挡，
组件管理器下载会失败）：

| 组件 | 版本 | 说明 |
|---|---|---|
| `esp32-camera` | 2.1.8 | 摄像头驱动（OV5640 在列）。删掉了它的 `idf_component.yml`，并把 `esp_jpeg` 写进 `CMakeLists.txt` 的 `REQUIRES`（`esp_camera.h` 末尾 include `img_converters.h`，后者要 `jpeg_decoder.h`，所以必须是 PUBLIC） |
| `esp_jpeg` | 1.3.1 | JPEG 解码（`esp32-camera` 的 `jpg2rgb565` 等接口依赖）；本工程不解码，仅为满足编译依赖 |

## 目录结构

```
Camera_test_tool/
├── CMakeLists.txt  partitions.csv  sdkconfig.defaults  AGENTS.md  README.md
├── main/            main.c：初始化顺序 + console 任务（命令解析）
├── camera/          drv_camera.*：esp32-camera 板级封装（引脚/开关/帧缓冲）
│                    app_camera.*：取图、落盘、base64 外发、统计
├── sdcard/          drv_sdcard.*：SPI2 总线 + SD 挂载 + 文件读写
├── components/      esp32-camera 2.1.8 + esp_jpeg 1.3.1（内置，离线）
└── scripts/         idf.bat / cam_cmd.py / cam_dump.py
```

依赖方向与 `Embedded_debug_tool` 一致：`drv_` → ESP-IDF API；`app_` → `drv_`；
`main` → `app_`/`drv_`。无 `platform/`、无 LVGL。

## 验证记录

见 `docs/camera_test_report.md`（真机结果：传感器识别、帧率、SD 落卡、PC 回传）。

## 已知边界 / 后续可选项

* **没有 LCD 实时预览**：这是刻意的。SPI2 与 SD 共总线，DVP 采集 + SPI2 刷屏 + 写卡同时跑会互相抢
  带宽，而且显示缓冲要内部 DMA RAM。第一版把「能取到图、能把图取回 PC」这条最短路径打通；
  要屏显再单独加。
* **`PIXFORMAT_RGB565` 未启用**：需要 `CONFIG_CAMERA_CONVERTER_ENABLED`（LCD_CAM BT601 转换）
  或 CPU 软编码，本工程走传感器硬 JPEG。
* **OV5640 自动对焦（`CAMERA_AF_SUPPORT`）未启用**：默认关；模块若是 AF 镜头需另开。
* **`dump` 走 COM14 的 USB-Serial/JTAG**：大分辨率（UXGA/QXGA）码流几十到上百 KB，
  base64 后回传要几十秒，属预期；要快就用 `shot` 落卡。
