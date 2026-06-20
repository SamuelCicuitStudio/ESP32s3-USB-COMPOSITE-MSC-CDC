/*
 * app_main.c - USB MSC-only SD card example for ESP32-S3.
 *
 * TinyUSB is bundled in components/tinyusb. All TinyUSB
 * callbacks are defined here as strong symbols — no --wrap, no production
 * extra USB component involved. This follows the same working MSC-only
 * pattern as the Arduino USBMSC reference.
 *
 * Console stays on UART0 GPIO43/44 through the FTDI adapter at 115200 baud.
 * Native USB exposes only the MSC disk.
 *
 * Startup order: SD first, then USB. The host never sees a NOT_READY state
 * on first mount because the card is probed before USB enumeration starts.
 *
 * LED indicator (active LOW): GPIO8=R, GPIO46=G, GPIO3=B
 *   Off        — no SD card / not ready
 *   Solid blue — SD ready, USB mounted, idle
 *   Green blink — write activity
 *   Blue blink  — read activity (no write)
 *   Solid red   — SD error
 */

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "tusb.h"

/* ── RGB LED pin assignment (active LOW) ─────────────────────────────────── */
#define LED_R   8
#define LED_G   46
#define LED_B   3

/* ── SD card pin assignment ──────────────────────────────────────────────── */
#define SD_SCLK  41
#define SD_MOSI  39
#define SD_MISO  42
#define SD_CS    38
#define SD_HOST  SPI3_HOST
#define SD_FREQ  20000000     /* 20 MHz */

/* ── USB identity (matches board_config.h production values) ─────────────── */
#define USB_VID        0xCAFEU
#define USB_PID        0x4007U
#define USB_BCD_DEV    0x0103U

#define MSC_SECTOR_BUFFER_BYTES 4096U
#define MSC_VOLUME_LABEL        "GEYE"

/* ── SD state ────────────────────────────────────────────────────────────── */
static sdmmc_card_t       s_card;
static bool               s_card_ready = false;
static bool               s_card_error = false;
static sdspi_dev_handle_t s_spi_dev    = -1;
static uint8_t           *s_dma_buf    = NULL;
static usb_phy_handle_t   s_phy;

/* ── Counters (sampled by led_task — zero overhead in MSC hot path) ──────── */
static volatile uint32_t  s_msc_read_calls;
static volatile uint32_t  s_msc_read_errors;
static volatile uint32_t  s_msc_write_calls;
static volatile uint32_t  s_msc_write_errors;

static char s_usb_serial[13];

/* ── LED helpers ─────────────────────────────────────────────────────────── */
static void led_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_R) | (1ULL << LED_G) | (1ULL << LED_B),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    /* All off initially (HIGH = off for active-low LEDs) */
    gpio_set_level(LED_R, 1);
    gpio_set_level(LED_G, 1);
    gpio_set_level(LED_B, 1);
}

/* r/g/b: true = LED on */
static inline void led_rgb(bool r, bool g, bool b)
{
    gpio_set_level(LED_R, r ? 0 : 1);
    gpio_set_level(LED_G, g ? 0 : 1);
    gpio_set_level(LED_B, b ? 0 : 1);
}

/* ── LED indicator task ──────────────────────────────────────────────────── */
/*
 * Runs at priority 1, samples read/write counters every 100 ms.
 * No mutex needed — counters are written by TinyUSB task, read here.
 * States:
 *   Off        : SD not ready
 *   Solid red  : SD error
 *   Green blink: write activity detected since last sample
 *   Blue blink : read activity only
 *   Solid blue : mounted and idle
 */
static void led_task(void *arg)
{
    (void)arg;
    uint32_t last_read  = 0;
    uint32_t last_write = 0;
    bool     blink_phase = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));

        uint32_t cur_read  = s_msc_read_calls;
        uint32_t cur_write = s_msc_write_calls;
        bool read_active   = (cur_read  != last_read);
        bool write_active  = (cur_write != last_write);
        last_read  = cur_read;
        last_write = cur_write;
        blink_phase = !blink_phase;

        if (s_card_error) {
            led_rgb(true, false, false);          /* solid red — error */
        } else if (!s_card_ready) {
            led_rgb(false, false, false);          /* off — no card */
        } else if (write_active) {
            led_rgb(false, blink_phase, false);    /* green blink — writing */
        } else if (read_active) {
            led_rgb(false, false, blink_phase);    /* blue blink — reading */
        } else {
            led_rgb(false, false, true);           /* solid blue — idle */
        }
    }
}

/* ── FAT volume label writer ─────────────────────────────────────────────── */
static void sd_set_volume_label(const char *label)
{
    uint8_t fmt[11];
    memset(fmt, ' ', sizeof(fmt));
    for (int i = 0; i < 11 && label[i] != '\0'; ++i) {
        char c = label[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
        fmt[i] = (uint8_t)c;
    }

    if (sdmmc_read_sectors(&s_card, s_dma_buf, 0, 1) != ESP_OK) return;

    uint32_t fat_lba = 0;
    if (s_dma_buf[0] != 0xEB && s_dma_buf[0] != 0xE9) {
        if (s_dma_buf[510] == 0x55 && s_dma_buf[511] == 0xAA) {
            uint32_t p = (uint32_t)s_dma_buf[454]        |
                         ((uint32_t)s_dma_buf[455] << 8)  |
                         ((uint32_t)s_dma_buf[456] << 16) |
                         ((uint32_t)s_dma_buf[457] << 24);
            if (p > 0U) {
                fat_lba = p;
                if (sdmmc_read_sectors(&s_card, s_dma_buf, fat_lba, 1) != ESP_OK) return;
            }
        }
    }

    if ((s_dma_buf[0] != 0xEB && s_dma_buf[0] != 0xE9) ||
        s_dma_buf[510] != 0x55 || s_dma_buf[511] != 0xAA) {
        return;
    }

    const bool is_fat32 = (memcmp(s_dma_buf + 82, "FAT32   ", 8) == 0);
    const uint16_t bps  = (uint16_t)(s_dma_buf[11] | ((uint16_t)s_dma_buf[12] << 8));
    const uint8_t  spc  = s_dma_buf[13];
    const uint16_t rsvd = (uint16_t)(s_dma_buf[14] | ((uint16_t)s_dma_buf[15] << 8));
    const uint8_t  nfat = s_dma_buf[16];

    if (bps != 512 || spc == 0 || nfat == 0) return;

    uint32_t root_lba, root_secs;
    if (is_fat32) {
        uint32_t fat32_sz = (uint32_t)s_dma_buf[36] | ((uint32_t)s_dma_buf[37] << 8) |
                            ((uint32_t)s_dma_buf[38] << 16) | ((uint32_t)s_dma_buf[39] << 24);
        uint32_t root_clus = (uint32_t)s_dma_buf[44] | ((uint32_t)s_dma_buf[45] << 8) |
                             ((uint32_t)s_dma_buf[46] << 16) | ((uint32_t)s_dma_buf[47] << 24);
        uint32_t first_data = fat_lba + rsvd + (nfat * fat32_sz);
        root_lba  = first_data + (root_clus - 2U) * spc;
        root_secs = spc;
    } else {
        uint16_t fat16_sz = (uint16_t)(s_dma_buf[22] | ((uint16_t)s_dma_buf[23] << 8));
        uint16_t root_ent = (uint16_t)(s_dma_buf[17] | ((uint16_t)s_dma_buf[18] << 8));
        root_lba  = fat_lba + rsvd + ((uint32_t)nfat * fat16_sz);
        root_secs = ((uint32_t)root_ent * 32U + 511U) / 512U;
        if (root_secs == 0U) root_secs = 1U;
    }

    /* Write BPB label */
    memcpy(s_dma_buf + (is_fat32 ? 71U : 43U), fmt, sizeof(fmt));
    sdmmc_write_sectors(&s_card, s_dma_buf, fat_lba, 1);

    /* Write root-directory ATTR_VOLUME_ID entry */
    bool     done    = false;
    uint32_t del_lba = 0, del_off = 0;

    for (uint32_t si = 0; si < root_secs && !done; ++si) {
        if (sdmmc_read_sectors(&s_card, s_dma_buf, root_lba + si, 1) != ESP_OK) break;
        bool dirty = false;
        for (uint32_t ei = 0; ei < 512U / 32U; ++ei) {
            uint8_t *dir  = s_dma_buf + ei * 32U;
            uint8_t  first = dir[0];
            uint8_t  attr  = dir[11];
            if (first == 0x00) {
                memset(dir, 0, 32); memcpy(dir, fmt, sizeof(fmt)); dir[11] = 0x08;
                dirty = done = true; break;
            }
            if (first == 0xE5) {
                if (!del_lba) { del_lba = root_lba + si; del_off = ei * 32U; }
                continue;
            }
            if ((attr & 0x0F) == 0x0F) continue;
            if (attr & 0x08) {
                memcpy(dir, fmt, sizeof(fmt)); dirty = done = true; break;
            }
        }
        if (dirty) sdmmc_write_sectors(&s_card, s_dma_buf, root_lba + si, 1);
    }
    if (!done && del_lba) {
        if (sdmmc_read_sectors(&s_card, s_dma_buf, del_lba, 1) == ESP_OK) {
            uint8_t *dir = s_dma_buf + del_off;
            memset(dir, 0, 32); memcpy(dir, fmt, sizeof(fmt)); dir[11] = 0x08;
            sdmmc_write_sectors(&s_card, s_dma_buf, del_lba, 1);
        }
    }
}

/* ── SD initialisation ───────────────────────────────────────────────────── */
static esp_err_t sd_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(100));

    const spi_bus_config_t bus = {
        .mosi_io_num     = SD_MOSI,
        .miso_io_num     = SD_MISO,
        .sclk_io_num     = SD_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 8192,
        .flags           = SPICOMMON_BUSFLAG_MASTER,
    };
    esp_err_t ret = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    ret = sdspi_host_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id  = SD_HOST;
    dev.gpio_cs  = SD_CS;
    dev.gpio_cd  = SDSPI_SLOT_NO_CD;
    dev.gpio_wp  = SDSPI_SLOT_NO_WP;
    dev.gpio_int = SDSPI_SLOT_NO_INT;

    ret = sdspi_host_init_device(&dev, &s_spi_dev);
    if (ret != ESP_OK) return ret;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot         = s_spi_dev;
    host.max_freq_khz = SD_FREQ / 1000U;

    memset(&s_card, 0, sizeof(s_card));
    ret = sdmmc_card_init(&host, &s_card);
    if (ret != ESP_OK) {
        sdspi_host_remove_device(s_spi_dev);
        s_spi_dev = -1;
        return ret;
    }

    /* Stamp volume label before exposing card to USB */
    sd_set_volume_label(MSC_VOLUME_LABEL);
    s_card_ready = true;
    return ESP_OK;
}

/* ── USB PHY + TinyUSB init ──────────────────────────────────────────────── */
static void usb_phy_and_tusb_init(void)
{
    const usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_UNDEFINED,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_cfg, &s_phy));

    const tusb_rhport_init_t rh = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    ESP_ERROR_CHECK(tusb_init(BOARD_TUD_RHPORT, &rh) ? ESP_OK : ESP_FAIL);
}

/* ── USB descriptors ─────────────────────────────────────────────────────── */
static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = USB_BCD_DEV,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

enum { ITF_MSC = 0, ITF_COUNT = 1 };

#define EP_MSC_OUT          0x01
#define EP_MSC_IN           0x81
#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 4, EP_MSC_OUT, EP_MSC_IN, 64),
};

static const char *const s_strings[] = {
    (const char[]){0x09, 0x04},  /* 0 — Language: English */
    "Samuel Circuit Studio",      /* 1 — Manufacturer */
    "Geye MSC SD",                /* 2 — Product */
    NULL,                         /* 3 — Serial (built at runtime) */
    "Geye SD Card",               /* 4 — MSC interface */
};

static const char *serial_string(void)
{
    if (s_usb_serial[0] != '\0') return s_usb_serial;
    static const char hex[] = "0123456789ABCDEF";
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        for (int i = 0; i < 6; i++) {
            s_usb_serial[i * 2]     = hex[mac[i] >> 4];
            s_usb_serial[i * 2 + 1] = hex[mac[i] & 0x0F];
        }
    } else {
        memcpy(s_usb_serial, "000000000000", 12);
    }
    s_usb_serial[12] = '\0';
    return s_usb_serial;
}

/* ── TinyUSB descriptor callbacks (direct strong symbols, no --wrap) ──────── */
const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_desc_device;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_config;
}

static uint16_t s_str_buf[32];
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t len = 0;
    if (index == 0) {
        memcpy(&s_str_buf[1], s_strings[0], 2);
        len = 1;
    } else {
        if (index >= sizeof(s_strings) / sizeof(s_strings[0])) return NULL;
        const char *str = (index == 3) ? serial_string() : s_strings[index];
        if (!str) return NULL;
        len = (uint8_t)strlen(str);
        if (len > 31) len = 31;
        for (uint8_t i = 0; i < len; i++)
            s_str_buf[1 + i] = (uint16_t)str[i];
    }
    s_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return s_str_buf;
}

/* ── TinyUSB event callbacks ─────────────────────────────────────────────── */
void tud_mount_cb(void)   { }
void tud_umount_cb(void)  { }
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void)  { }

/* ── MSC callbacks ───────────────────────────────────────────────────────── */
uint8_t tud_msc_get_maxlun_cb(void) { return 0; }

void tud_msc_inquiry_cb(uint8_t lun,
                        uint8_t vendor_id[8],
                        uint8_t product_id[16],
                        uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "GEYE    ", 8);
    memcpy(product_id,  "SD CARD         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

uint32_t tud_msc_inquiry2_cb(uint8_t lun,
                              scsi_inquiry_resp_t *resp,
                              uint32_t bufsize)
{
    (void)lun;
    if (!resp || bufsize < sizeof(*resp)) return 0;
    memcpy(resp->vendor_id,   "GEYE    ", 8);
    memcpy(resp->product_id,  "SD CARD         ", 16);
    memcpy(resp->product_rev, "1.0 ", 4);
    return sizeof(*resp);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (lun != 0 || !s_card_ready || !s_dma_buf) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun,
                          uint32_t *block_count,
                          uint16_t *block_size)
{
    if (lun == 0 && s_card_ready) {
        *block_size  = (uint16_t)s_card.csd.sector_size;
        *block_count = (uint32_t)s_card.csd.capacity;
    } else {
        *block_size  = 512;
        *block_count = 0;
    }
}

bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return true; }

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                            bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba,
                           uint32_t offset, void *buffer, uint32_t bufsize)
{
    s_msc_read_calls++;
    if (lun != 0 || !s_card_ready || !s_dma_buf || !buffer) {
        s_msc_read_errors++;
        return -1;
    }
    const uint32_t sector_size = (uint32_t)s_card.csd.sector_size;
    if (sector_size == 0U || sector_size > MSC_SECTOR_BUFFER_BYTES || offset >= sector_size) {
        s_msc_read_errors++;
        return -1;
    }
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t rem = bufsize;
    uint32_t cur_lba = lba;
    uint32_t cur_off = offset;
    while (rem > 0U) {
        uint32_t room  = sector_size - cur_off;
        uint32_t chunk = rem < room ? rem : room;
        if (sdmmc_read_sectors(&s_card, s_dma_buf, cur_lba, 1) != ESP_OK) {
            s_msc_read_errors++;
            return -1;
        }
        memcpy(dst, s_dma_buf + cur_off, chunk);
        dst += chunk;
        rem -= chunk;
        cur_lba++;
        cur_off = 0U;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba,
                            uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    s_msc_write_calls++;
    if (lun != 0 || !s_card_ready || !s_dma_buf || !buffer) {
        s_msc_write_errors++;
        return -1;
    }
    const uint32_t sector_size = (uint32_t)s_card.csd.sector_size;
    if (sector_size == 0U || sector_size > MSC_SECTOR_BUFFER_BYTES || offset >= sector_size) {
        s_msc_write_errors++;
        return -1;
    }
    const uint8_t *src = buffer;
    uint32_t rem = bufsize;
    uint32_t cur_lba = lba;
    uint32_t cur_off = offset;
    while (rem > 0U) {
        uint32_t room  = sector_size - cur_off;
        uint32_t chunk = rem < room ? rem : room;
        if (cur_off == 0U && chunk == sector_size) {
            memcpy(s_dma_buf, src, sector_size);
        } else {
            if (sdmmc_read_sectors(&s_card, s_dma_buf, cur_lba, 1) != ESP_OK) {
                s_msc_write_errors++;
                return -1;
            }
            memcpy(s_dma_buf + cur_off, src, chunk);
        }
        if (sdmmc_write_sectors(&s_card, s_dma_buf, cur_lba, 1) != ESP_OK) {
            s_msc_write_errors++;
            return -1;
        }
        src += chunk;
        rem -= chunk;
        cur_lba++;
        cur_off = 0U;
    }
    return (int32_t)bufsize;
}

void tud_msc_write10_complete_cb(uint8_t lun) { (void)lun; }

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t cmd[16],
                         void *buffer, uint16_t bufsize)
{
    if (!cmd) return -1;
    switch (cmd[0]) {
    case 0x1A: { /* MODE_SENSE(6) */
        const uint8_t r[4] = {3, 0, 0, 0};
        uint16_t len = bufsize < sizeof(r) ? bufsize : (uint16_t)sizeof(r);
        memcpy(buffer, r, len);
        return (int32_t)len;
    }
    case 0x5A: { /* MODE_SENSE(10) */
        const uint8_t r[8] = {0, 6, 0, 0, 0, 0, 0, 0};
        uint16_t len = bufsize < sizeof(r) ? bufsize : (uint16_t)sizeof(r);
        memcpy(buffer, r, len);
        return (int32_t)len;
    }
    case 0x15: case 0x55: case 0x1E:
    case 0x2F: case 0x35: case 0x91: case 0x04:
        (void)buffer; (void)bufsize;
        return 0;
    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

/* ── Tasks ───────────────────────────────────────────────────────────────── */
static void usb_task(void *arg)
{
    (void)arg;
    for (;;) tud_task();
}

/* ── app_main ────────────────────────────────────────────────────────────── */
void app_main(void)
{
    led_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* DMA bounce buffer — internal SRAM, 4-byte aligned, required by GDMA */
    s_dma_buf = heap_caps_aligned_alloc(4, MSC_SECTOR_BUFFER_BYTES,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma_buf) {
        s_card_error = true;
        for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* SD first — card ready before USB host ever polls MSC */
    ret = sd_init();
    if (ret != ESP_OK) {
        s_card_error = true;
    }

    /* Start LED indicator before USB enumerates */
    xTaskCreate(led_task, "led", 2048, NULL, 1, NULL);

    /* USB after SD — host sees a ready disk on first mount */
    usb_phy_and_tusb_init();
    xTaskCreate(usb_task, "usb_tud", 8192, NULL, 5, NULL);

    for (;;) vTaskDelay(pdMS_TO_TICKS(60000));
}
