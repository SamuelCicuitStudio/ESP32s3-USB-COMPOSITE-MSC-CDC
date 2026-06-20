/*
 * TinyUSB configuration for the ESP32-S3 USB MSC-only SD-card example.
 */

#pragma once

#define CFG_TUSB_OS_INC_PATH freertos/
#define CFG_TUSB_DEBUG       0

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN   __attribute__((aligned(4)))

#define CFG_TUD_ENABLED        1
#define CFG_TUH_ENABLED        0
#define CFG_TUD_MAX_SPEED      OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC    0
#define CFG_TUD_MSC    1
#define CFG_TUD_HID    0
#define CFG_TUD_MIDI   0
#define CFG_TUD_VENDOR 0

/*
 * 4096 bytes = 8 sectors per USB transfer cycle.
 * Reduces MSC CBW/data/CSW round-trips by 8x vs the default 512-byte buffer.
 * Must match MSC_SECTOR_BUFFER_BYTES in app_main.c.
 */
#define CFG_TUD_MSC_EP_BUFSIZE 4096
