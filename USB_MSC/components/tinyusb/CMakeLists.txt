set(TUSB_SRC "${CMAKE_CURRENT_LIST_DIR}/src")

idf_component_register(
    SRCS
        "${TUSB_SRC}/tusb.c"
        "${TUSB_SRC}/common/tusb_fifo.c"
        "${TUSB_SRC}/device/usbd.c"
        "${TUSB_SRC}/class/msc/msc_device.c"
        "${TUSB_SRC}/portable/synopsys/dwc2/dcd_dwc2.c"
        "${TUSB_SRC}/portable/synopsys/dwc2/dwc2_common.c"
    INCLUDE_DIRS
        "${TUSB_SRC}"
        "${CMAKE_CURRENT_LIST_DIR}"
    REQUIRES
        freertos
        esp_hw_support
        usb
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CFG_TUSB_MCU=OPT_MCU_ESP32S3
    CFG_TUSB_OS=OPT_OS_FREERTOS
    BOARD_TUD_RHPORT=0
    BOARD_TUD_MAX_SPEED=OPT_MODE_FULL_SPEED
)
