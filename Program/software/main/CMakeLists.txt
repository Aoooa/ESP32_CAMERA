set(MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

idf_component_register(
    SRCS
        "main.c"
        "${MODULE_DIR}/camera/drv_camera.c"
        "${MODULE_DIR}/camera/app_camera.c"
        "${MODULE_DIR}/sdcard/drv_sdcard.c"
        "${MODULE_DIR}/bsp/bus_spi2.c"
        "${MODULE_DIR}/lcd/drv_lcd.c"
        "${MODULE_DIR}/lcd/app_lcd.c"
        "${MODULE_DIR}/usb_uvc/app_usb_uvc.c"
        "${MODULE_DIR}/video/app_video.c"
        "${MODULE_DIR}/video/frame_bus.c"
        "${MODULE_DIR}/perf/app_perf.c"

    INCLUDE_DIRS
        "."
        "${MODULE_DIR}/camera"
        "${MODULE_DIR}/sdcard"
        "${MODULE_DIR}/bsp"
        "${MODULE_DIR}/lcd"
        "${MODULE_DIR}/usb_uvc"
        "${MODULE_DIR}/video"
        "${MODULE_DIR}/perf"

    PRIV_REQUIRES
        esp32-camera
        usb_device_uvc
        espressif__tinyusb
        driver esp_driver_spi esp_driver_sdspi sdmmc fatfs vfs
        esp_lcd
        nvs_flash heap esp_timer esp_rom log
)
