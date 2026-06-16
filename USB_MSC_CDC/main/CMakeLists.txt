idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        tinyusb           # bundled TinyUSB — compiled from source in components/tinyusb/
        nvs_flash
        driver
        esp_driver_sdspi  # SPI mode (SD_MODE_SPI)
        esp_driver_sdmmc  # SDMMC native mode (SD_MODE_SDMMC_1B / SD_MODE_SDMMC_4B)
        sdmmc
        log
        heap
        usb               # esp_private/usb_phy.h (IDF USB PHY layer)
)
