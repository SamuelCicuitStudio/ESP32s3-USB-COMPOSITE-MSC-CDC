/*
 * tusb_config.h — TinyUSB configuration for CDC + MSC composite device.
 *
 * CFG_TUSB_MCU, CFG_TUSB_OS, BOARD_TUD_RHPORT, and BOARD_TUD_MAX_SPEED are
 * injected by CMake target_compile_definitions so they do not need to be
 * defined here.
 */

#pragma once

#define CFG_TUSB_OS_INC_PATH freertos/   /* IDF FreeRTOS header prefix */
#define CFG_TUSB_DEBUG       0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN   __attribute__((aligned(4)))

/* Device-only, full-speed */
#define CFG_TUD_ENABLED        1
#define CFG_TUH_ENABLED        0
#define CFG_TUD_MAX_SPEED      OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE 64

/* Enabled classes */
#define CFG_TUD_CDC    1
#define CFG_TUD_MSC    1
#define CFG_TUD_HID    0
#define CFG_TUD_MIDI   0
#define CFG_TUD_VENDOR 0

/* CDC buffers */
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256

/* MSC bulk endpoint buffer (one sector) */
#define CFG_TUD_MSC_EP_BUFSIZE 512
