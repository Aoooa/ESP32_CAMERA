set(MODULE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

idf_component_register(
    SRCS
        "main.c"
        "${MODULE_DIR}/camera/drv_camera.c"
        "${MODULE_DIR}/camera/app_camera.c"
        "${MODULE_DIR}/sdcard/drv_sdcard.c"

    INCLUDE_DIRS
        "."
        "${MODULE_DIR}/camera"
        "${MODULE_DIR}/sdcard"

    PRIV_REQUIRES
        esp32-camera
        driver esp_driver_spi esp_driver_sdspi sdmmc fatfs vfs
        nvs_flash heap esp_timer esp_rom log
)
