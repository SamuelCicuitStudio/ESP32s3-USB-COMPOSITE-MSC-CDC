idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        tinyusb
        nvs_flash
        driver
        esp_driver_sdspi
        sdmmc
        log
        heap
        usb
)
