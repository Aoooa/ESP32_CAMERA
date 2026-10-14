# OV5640 摄像头测试报告 — Camera_test_tool

日期：2026-10-14 ｜ 板子：Waveshare ESP32-S3-Touch-LCD-2 ｜ 烧录/调试口：**COM14**（板载原生 USB-Serial/JTAG）

## 结论

| 项目 | 结果 | 证据 |
|---|---|---|
| 摄像头驱动（OV5640） | ✅ 通过 | 开机即识别 `Detected OV5640 camera`，`PID=0x5640 addr=0x3c` |
| 图像获取（取帧） | ✅ 通过 | 多轮累计 120+ 帧，最后一轮 `ok=32 fail=0 eoi_ok=32`，每帧 JPEG 头尾 `FFD8…FFD9` 完整 |
| 多分辨率 | ✅ 通过 | QVGA/VGA/HD/UXGA/QSXGA(2560×1920) 全部实测取到图 |
| SD 卡落盘 | ✅ 通过 | `shot` 存 `/sdcard/cam/cam_<分辨率>_<序号>.jpg`，`ls` 读回文件名+字节数一致；续号跨重启不覆盖 |
| 图像回传 PC 验证 | ✅ 通过 | `dump` → base64 → `len+CRC32` 校验 → Pillow 独立解码出真实场景照片 |
| DVP 数据通路正确性 | ✅ 通过 | 传感器彩条模式下取回的图是 8 条纯色且 **R/G/B 不串位** |
| 帧率 | ✅ 实测 | QVGA/VGA 22.2fps、HD 12.7fps、UXGA 4.5fps、QSXGA 单帧 423ms |
| 资源 | ✅ 有余量 | 固件 437KB / 内部 RAM 剩 319KB（最大块 240KB）/ PSRAM 剩 8.0MB |

## 最终一轮回归（序号修正后的固件，22:29）

```
[sd ] 卡上已有 9 张，新图从 0010 开始          # 首次落盘前扫卡续号，重启不覆盖旧图
[cam] 1/2 640x480 10250B 0ms JPEG-OK   [sd ] saved /sdcard/cam/cam_vga_0010.jpg 56ms
[cam] 2/2 640x480 10239B 0ms JPEG-OK   [sd ] saved /sdcard/cam/cam_vga_0011.jpg 52ms
[cam] 30/30 640x480 10415B 44ms JPEG-OK
[cam] run: 30 frames wall 1269ms | interval 45ms -> 22.2 fps (帧时间戳) | queue_wait 39ms
dump vga / qsxga / bar  ->  三次都 exit=0，全部第一次就通过 len+CRC32
counters : frame_seq=3 card_file_seq=11 dump_calls=3   # dump 不再占用卡上文件序号
frames   : ok=32 fail=0 eoi_ok=32 saved=2 save_fail=0
```

这一轮的环境光比第一轮暗得多，回传图的像素均值因此很低（`q_vga.jpg` mean≈8、
`q_qsxga.jpg` mean≈17，主色 `(5,5,5)`）；**同一轮抓的彩条图完全正常**
（`q_bar.jpg` mean≈129，主色仍是 (191,0,221)/(254,254,254)/(255,255,97)/(83,251,216) 等纯色）。
彩条不经过光路 → 低均值是镜头进光不足（22fps 下最长曝光约 45ms，暗光本来吃不饱），
不是驱动或数据通路的问题。这也正是加 `bar` 命令的意义：**把“光学/光照”和“数据通路”分开判**。

## 构建环境

* ESP-IDF **v5.5.3**（`D:\esp\v5.5.3\esp-idf`），target `esp32s3`
* 组件（内置在 `components/`，离线构建）：`esp32-camera` **2.1.8** + `esp_jpeg` **1.3.1**
* 固件：`Camera_test_tool.bin` = 0x6c410 ≈ **437 KB**（app 分区 4 MB，占 11%）
* `sdkconfig.defaults`：16MB Flash / 240MHz / PSRAM 八线 80MHz / FATFS 长文件名+exFAT+UTF-8 /
  console = USB-Serial/JTAG

## 硬件与引脚

摄像头走 **DVP 并口 + SCCB(I2C) 配置**，引脚与官方例程一致（两处独立来源互相印证）：
`示例程序/…/ESP-IDF/05_lvgl_camera/main/main.c`、`示例程序/…/Arduino/examples/10_camera_web_server/10_camera_web_server.ino`

```
PWDN=17  RESET=-1(软复位)  XCLK=8  SIOD=21  SIOC=16
D0=12 D1=13 D2=15 D3=11 D4=14 D5=10 D6=7 D7=2
VSYNC=6 HREF=4 PCLK=9     XCLK=20MHz
```

SD 卡（SPI 模式，SPI2）：`MOSI=38 SCLK=39 MISO=40 CS=41`
（`示例程序/…/ESP-IDF/01_sd_card_test` + `Embedded_debug_tool/sdcard/drv_sdcard.c` 一致；
`CLAUDE.md` 里写的 IO33/34/35/36 是过时信息，已在 README/AGENTS 标注。）

**为什么必须是独立工程**：`IO2(D7)`、`IO4(HREF)`、`IO16(SIOC)`、`IO17(PWDN)` 正是
`Embedded_debug_tool` 的 UART1（XFR 高速口）与 UART2 引脚 —— 同一份固件里摄像头和那两路
UART 不能共存。本工程因此不初始化 LCD/触摸/UART1/UART2/WiFi，把引脚全部让给 DVP。
SD 卡与 DVP 无冲突，可以边拍边写卡。

## 传感器识别（开机日志原文）

```
I (733) drv_camera: init: JPEG 640x480 quality=12 fb=2 psram xclk=20000000Hz sccb=21/16
I (734) s3 ll_cam: DMA Channel=1
I (734) cam_hal: cam init ok
I (734) sccb-ng: pin_sda 21 pin_scl 16
I (734) sccb-ng: sccb_i2c_port=1
I (766) ov3640: Mismatch PID=0x5640
I (768) ov3660: Mismatch PID=0x5640
I (769) camera: Camera PID=0x5640 VER=0x00 MIDL=0x00 MIDH=0x00
I (769) camera: Detected OV5640 camera
I (769) camera: Detected camera at address=0x3c
I (1455) cam_hal: PSRAM DMA mode disabled
I (1455) cam_hal: buffer_size: 16384, half_buffer_size: 1024, node_buffer_size: 1024, node_cnt: 16, total_cnt: 60
I (1455) cam_hal: Allocating 61440 Byte frame buffer in PSRAM
I (1455) cam_hal: Allocating 61440 Byte frame buffer in PSRAM
I (1456) cam_hal: cam config ok
I (1468) ov5640: Set PLL: bypass: 0, multiplier: 180, sys_div: 4, pre_div: 2, root_2x: 0, pclk_root_div: 2, pclk_manual: 1, pclk_div: 4
I (1468) ov5640: Calculated XVCLK: 20000000 Hz, REFIN: 10000000 Hz, VCO: 1800000000 Hz, PLL_CLK: 180000000 Hz, SYSCLK: 45000000 Hz, PCLK: 11250000 Hz
I (1488) drv_camera: sensor: OV5640 PID=0x5640 VER=0x00 addr=0x3c xclk=20000000Hz
```

SD 卡同时挂载成功（同一份开机日志）：

```
Name: SD32G   Type: SDHC   Speed: 20.00 MHz (limit: 20.00 MHz)   Size: 30436MB
I (732) drv_sdcard: mounted: total=30424.5MB free=30377.3MB
```

> `I (685) sdspi_transaction: cmd=5, R1 response: command not supported` 是正常现象：
> CMD5(SD 专用) 对非 SDIO 卡本来就会被拒，属于卡的识别流程，不是错误。

## 取帧性能（JPEG 由 OV5640 片内编码，`PIXFORMAT_JPEG`）

帧率按**帧自带时间戳**计算（`esp_camera_fb_get()` 的队列等待另列）：

| 分辨率 | 像素 | 帧间隔 | fps | 单帧 JPEG 大小 | 备注 |
|---|---|---|---|---|---|
| QVGA | 320×240 | 45 ms | 22.2 | ~3.6 KB | `burst 20`，20/20 完整 |
| VGA | 640×480 | 45 ms | 22.2 | ~10.0 KB | `burst 30`，30/30 完整 |
| HD | 1280×720 | 78 ms | 12.7 | ~23.1 KB | `burst 20`，20/20 完整 |
| UXGA | 1600×1200 | 223 ms | 4.5 | ~49.5 KB | `shot 2` 落卡成功 |
| QSXGA | 2560×1920 | — | — | 123 404 B | 传感器最大档，抓帧 423 ms、写卡 449 ms |

原始日志片段：

```
[cam] 30/30 640x480 9991B 44ms JPEG-OK
[cam] run: 30 frames wall 1282ms | interval 45ms -> 22.2 fps (帧时间戳) | queue_wait 42ms | bytes min/avg/max=9943/9977/10011
[cam] total: ok=30 fail=0 eoi_ok=30 saved=0 save_fail=0

[cam] 20/20 1280x720 23065B 78ms JPEG-OK
[cam] run: 20 frames wall 1420ms | interval 78ms -> 12.7 fps (帧时间戳) | queue_wait 53ms

[cam] 1/1 2560x1920 123404B 423ms JPEG-OK
[sd ] saved /sdcard/cam/cam_qsxga_0007.jpg 449ms
```

VGA/QVGA 同为 45ms 间隔说明该档受 OV5640 输出节奏（PCLK 11.25MHz / 寄存器时序）限制，
不是 ESP32 侧瓶颈；写卡一次 45–91 ms（SPI2 @20MHz，10KB 级 JPEG）。

## SD 卡落盘验证

```
[cam] 1/5 640x480 9964B 0ms JPEG-OK     [sd ] saved /sdcard/cam/cam_vga_0001.jpg 91ms
[cam] 2/5 640x480 10002B 0ms JPEG-OK    [sd ] saved /sdcard/cam/cam_vga_0002.jpg 49ms
[cam] 3/5 640x480 9943B 17ms JPEG-OK    [sd ] saved /sdcard/cam/cam_vga_0003.jpg 84ms
[cam] 4/5 640x480 10011B 0ms JPEG-OK    [sd ] saved /sdcard/cam/cam_vga_0004.jpg 45ms
[cam] 5/5 640x480 10000B 3ms JPEG-OK    [sd ] saved /sdcard/cam/cam_vga_0005.jpg 48ms

[sd ] 列目录：
  FILE       9964 cam_vga_0001.jpg
  FILE      10002 cam_vga_0002.jpg
  FILE       9943 cam_vga_0003.jpg
  FILE      10011 cam_vga_0004.jpg
  FILE      10000 cam_vga_0005.jpg
[sd ] /sdcard/cam: 5 项, 文件共 49920 字节
```

`ls` 读回的大小与抓帧时报告的字节数逐张一致 → 文件真的落卡了（不是只写了缓存）。

## 回传 PC 的图像验证

`dump` 把帧缓冲直接 base64 打到 COM14，`scripts/cam_dump.py` 收流后用
**长度 + CRC32 双重校验**才落盘，再用 Pillow 独立解码验证像素（`tools/check_jpeg.py`）。

最后一版固件（标记协议修正后）**一次通过、无需重试**：

```
[ OK ] seq=1 -> out/f_vga.jpg     9966B  jpeg=ffd8..ffd9
[ OK ] seq=2 -> out/f_qsxga.jpg 123630B  jpeg=ffd8..ffd9        # 2560x1920 最大档
[ OK ] seq=3 -> out/multi\cam_320x240_0003.jpg  3635B  jpeg=ffd8..ffd9
[ OK ] seq=4 -> out/multi\cam_320x240_0004.jpg  3645B  jpeg=ffd8..ffd9
[ OK ] seq=5 -> out/multi\cam_320x240_0005.jpg  3621B  jpeg=ffd8..ffd9
[ OK ] seq=6 -> out/f_bar.jpg     (彩条)  jpeg=ffd8..ffd9
```

Pillow 独立解码 + 像素统计：

```
out/f_vga.jpg:      JPEG ( 640, 480) mean=(117.6,124.3,117.9) stddev=(46.1,46.7,47.7) unique_colors=7494  edge_mean=2.57
out/f_qsxga.jpg:    JPEG (2560,1920) mean=(121.5,124.1,119.6) stddev=(44.5,46.3,47.0) unique_colors=24033 edge_mean=1.09
out/multi/…0003:    JPEG ( 320, 240) mean=(117.0,123.7,117.5) stddev=(45.9,46.6,47.8) unique_colors=4835  edge_mean=4.40
out/f_bar.jpg:      JPEG ( 320, 240) mean=(128.8,127.9,128.4) stddev=(120.8,126.6,112.8) unique_colors=710 edge_mean=6.16
```

判读依据：各分辨率都能被标准 JPEG 解码器解出、尺寸与设置一致、三通道均值接近
（说明没有 R/B 通道错位）、`stddev≈45` 且有上万种颜色（说明是真实场景，不是黑屏/纯色/噪声）。

### 彩条对照（单独验证 DVP 数据通路）

`bar on`（OV5640 内部彩条，完全不依赖镜头与光线）后取一张回传：

```
out/f_bar.jpg: 中间一行主色 = (255,0,255) (254,254,254) (19,255,229) (240,0,28) …
```

= 洋红/白/青/绿/红/黑/蓝/黄 8 条纯色，且红是 `(240,0,28)`、绿是 `(3,255,0)`、蓝是 `(0,0,244)`
—— 通道没有串位、8 位数据线没有错位（D0-D7 顺序与 SCCB 配置都正确）。

## 资源占用（`stat` 实测）

```
 state    : ready
 sensor   : OV5640 PID=0x5640 VER=0x00 MIDL=0x00 MIDH=0x00 addr=0x3c
 xclk     : 20000000Hz  max_size=qsxga  psram_dma=off
 out fmt  : JPEG(传感器内编码)
 framesize: uxga (1600x1200) quality=12 fb=2 mirror=on flip=on bar=off
 frames   : ok=88 fail=0 eoi_ok=88 saved=8 save_fail=0
 bytes    : min=3576 avg=18184 max=123404 total=1600212
 timing   : queue_wait_avg=74ms save_avg=144ms interval=223ms fps=4.5 (帧时间戳)
 heap     : internal free=318KB largest=240KB | psram free=7413KB
== sdcard ==
 mounted  : /sdcard  images->/sdcard/cam  total=30424.5MB free=30376.9MB
```

内部 RAM 还剩 318KB（最大连续块 240KB），PSRAM 还剩 7.4MB —— 因为本工程没有 LCD 显示缓冲，
余量很足；要再加 LCD 预览也不会因为 RAM 受限。

## 过程中踩到/修正的点

1. **帧率算法**：最初用「等帧耗时」算 fps，实测 `cap_avg=0ms`（双帧缓冲里早就有现成帧，
   `esp_camera_fb_get()` 立刻命中），fps 因此完全失真。改成用帧自带时间戳算间隔，
   并把「队列等待」单独列出。
2. **base64 回传最初不可靠，两处协议/实现问题**（都靠 len+CRC32 当场抓到，而不是靠“图看起来还行”）：
   * *接收端按行过滤会丢字符*：旧解析要求“整行都是 base64”，而一次 `read(4096)` 的边界会把
     一行 124 字符的码流切成两半，长度 <4 的半行被整块丢掉 → 实测稳定少几字节。
     改成**按 `B64-BEGIN`/`B64-END` 标记切区间**，区间内只保留 base64 字符，天然抗分包抗断行。
   * *标记行带前缀会多字符*（这条有原始码流为证）：`B64-END` 那行原来写成 `[cam] B64-END …`，
     接收端定位到 `B64-END` 就切尾，于是前面的 `[cam] ` 落进码流，而 `cam` 三个字母**本身就是合法
     base64** → 每帧凭空多 3 字符（`raw_capture.py` 实测：区间 13576 字节里 base64 字符
     13355，比期望多 3；帧长是 3 的倍数时解码就多 2 字节而校验失败，带 `=` 填充时恰好被
     base64 解码器忽略，所以只有部分分辨率失败 —— 这正是它看起来“偶发”的原因）。
     改成**标记行顶行、无前缀**，接收端再把区间截到 END 行所在行之前做双保险。
   * 另外：`dump` 期间 `esp_log_level_set("*", ESP_LOG_NONE)` 全系统静音，
     避免 stdout 出现第二个写者把码流交错打断（newlib 缓冲在任意字节边界刷新，无法保证整行原子）。
   * 修正后：VGA / QSXGA(123KB) / 连续 3 帧 / 彩条，全部**第一次就通过 len+CRC32**。
   结论：**校验（长度 + CRC32）必须留在协议里**，`--retries` 作为兜底保留。
3. **`esp_vfs_fat.h` 不再带 `sdmmc_cmd.h`**：`sdmmc_card_print_info` 隐式声明报错，需显式 include。
4. **`-Werror` 三处**：注释里出现 `/*.ino`（`/*` within comment）、未使用的 `TAG`、
   `snprintf` 拼路径触发 `format-truncation`（用 `%.100s/%.255s` 限定宽度解决）。
5. **SD 总线归属**：`Embedded_debug_tool` 的 SD 只挂设备、SPI2 由 LCD 初始化；本工程没有 LCD，
   必须自己 `spi_bus_initialize()`，照抄会失败（已写进 AGENTS.md）。
6. **控制台中文编码**：固件输出的是 UTF-8（实测原始字节 `\xe4\xbc\xa0…`），
   PowerShell 控制台默认 GBK 会显示成乱码，但脚本按 UTF-8 落文件即正常；
   `cam_cmd.py --log <file>` 专门用来绕开 PowerShell 重定向（它会写成 UTF-16）。

## 已知限制 / 下一步

* **`dump` 的吞吐**：console 口（USB-Serial/JTAG）没有流控，实测 VGA(10KB) 秒级、
  QSXGA(123KB → base64 165KB) 约 30–40s。大图请优先 `shot` 落 SD 卡。
  要稳定高速取图应走 XFR（UART1@921600），但那条路的 IO2/IO4 与摄像头 DVP 冲突，本工程不能用。
* **未做 LCD 实时预览**（刻意）：SPI2 与 SD 共总线、刷屏要内部 DMA RAM，第一版先把
  「取到图 + 取回 PC」打通。要做预览可加 `esp_lcd_st7789` + 内部 RAM 分带暂存，
  或把本工程并入 UI 层。
* **`PIXFORMAT_RGB565` 未启用**：需要 `CONFIG_CAMERA_CONVERTER_ENABLED`（LCD_CAM BT601 转换）；
  本工程用传感器硬 JPEG。
* **OV5640 自动对焦（`CAMERA_AF_SUPPORT`）未启用**。
* 未测：`quality` 极端值、`flip/mirror` 组合下的图像方向、长时间连拍稳定性（>10min）、
  断电重挂卡的行为。
