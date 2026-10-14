# AGENTS.md — Camera_test_tool

ESP32-S3 摄像头测试工程（ESP-IDF v5.5.3）。同板独立于 `../Embedded_debug_tool`，
**故意不带 LCD/触摸/UART 桥/UI**：摄像头 DVP 引脚与那些外设冲突。

- 目的、引脚表、构建/烧录/验证：`README.md`
- 摄像头封装与引脚冲突说明：`camera/drv_camera.h`
- SD（SPI2）挂载说明：`sdcard/drv_sdcard.h`

## 分层（沿用 Embedded_debug_tool 规范）

```
main/                 组装入口 + console 任务（命令解析）
camera/drv_camera.*   esp32-camera 板级封装（引脚表 / 开关 / 帧缓冲）
camera/app_camera.*   测试业务：取图、落盘、base64 外发、统计
sdcard/drv_sdcard.*   SD(SPI 模式)挂载与文件读写
components/           离线内置 esp32-camera 2.1.8 + esp_jpeg 1.3.1
scripts/              idf.bat 构建包装、cam_cmd.py / cam_dump.py PC 侧调试
```

依赖方向：`app_camera → drv_camera + drv_sdcard`；`main → app_* + drv_sdcard`。
无 `platform/`、无 LVGL、无 WiFi。

## 构建

```bash
cmd //c "D:/Task/embedded_debugger_tool/Program/Camera_test_tool/scripts/idf.bat" build
```

## 烧录 / 调试（COM14 = 原生 USB-Serial/JTAG）

```bash
cmd //c ".../scripts/idf.bat" -p COM14 flash
cmd //c ".../scripts/idf.bat" -p COM14 monitor          # 退出务必 Ctrl+]
# monitor 残留会占住 COM14，清理：
#   Get-CimInstance Win32_Process -Filter "Name='python.exe'" |
#     Where-Object { $_.CommandLine -match 'monitor' } | Stop-Process
```

## 关键约束（改动前必读）

1. **PSRAM 必须开**（`sdkconfig.defaults` 里八线 80MHz）。帧缓冲在 PSRAM，关掉直接
   `esp_camera_init` 失败。
2. **本工程不能用 `../Embedded_debug_tool/sdcard/drv_sdcard.c` 的做法**：那边 SD 只挂设备、
   SPI2 总线由 LCD 初始化；这边没有 LCD，总线由 `drv_sdcard_init()` 自己
   `spi_bus_initialize()`。照抄会 `ESP_ERR_INVALID_STATE`。
3. **`CLAUDE.md` 的 GPIO 表里 SD 引脚（IO33/34/35/36）是过时信息**；实机与厂商例程都是
   MOSI=38 / SCLK=39 / MISO=40 / CS=41。以代码为准。
4. **改分辨率 = `esp_camera_reconfigure()`（deinit+init）**，传感器寄存器回到默认，
   `mirror/flip/bar/quality` 会丢；`app_camera.c` 用 `replay_sensor_opts()` 重放，
   新增同类开关时要一起加进去。
5. **输出格式走 `PIXFORMAT_JPEG`**（OV5640 内部编码）。`PIXFORMAT_RGB565` 在本工程
   需要 LCD_CAM 转换器 + CPU 编码（`CONFIG_CAMERA_CONVERTER_ENABLED`），未启用。
6. `sdkconfig.defaults` 改过要 `fullclean` 再 `build`；`fullclean` 后必须先 `build` 再 `flash`。
7. console 是**单行命令 + 文本回显**，脚本（`scripts/cam_cmd.py`）**不能翻转 DTR/RTS**，
   否则每次连接都复位板子。PowerShell 的 `1>` 重定向会把输出写成 UTF-16，抓 console 输出
   用 `cam_cmd.py --log <file>`（固件输出是 UTF-8）。
8. **`dump` 的外发标记协议**：`SET=… / B64-BEGIN … / B64-END …` 必须**顶行、不带 `[cam] ` 前缀**
   —— `cam` 三个字母本身是合法 base64，带前缀会让每帧凭空多 3 字符、长度校验失败。
   dump 期间还要 `esp_log_level_set("*", ESP_LOG_NONE)` 全系统静音，否则别的任务的日志会把
   码流按任意字节边界交错打断。完整性一律靠 `len + CRC32` 判定，不要靠“图看着还行”。
9. **落盘序号会撞名**：`cam_<分辨率>_<序号>.jpg` 的序号只在内存里，重启归零会覆盖上一轮。
   首次落盘前 `seed_file_seq_from_card()` 扫目录把序号推到已有最大值之后；
   改文件名格式时要同步改这个解析（取最后一个 `_` 之后的数字）。
