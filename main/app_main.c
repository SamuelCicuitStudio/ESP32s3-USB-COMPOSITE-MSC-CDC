/*
 * app_main.c — USB CDC + MSC composite device on ESP32-S3
 *
 * Combines a USB Mass Storage device (SD card via SPI) with a USB CDC
 * serial port on a single USB connector. No extra hardware is needed
 * beyond the SD card wired to SPI.
 *
 * TinyUSB is compiled from source as a project component (components/tinyusb/).
 * The project has no dependency on esp_tinyusb or any other framework wrapper.
 * USB PHY initialisation uses the standard IDF esp_private/usb_phy.h API.
 *
 * Default pin assignment (SPI3 — change SD_* defines below):
 *   SD CS   → GPIO 38
 *   SD SCK  → GPIO 41
 *   SD MISO → GPIO 42
 *   SD MOSI → GPIO 39
 *
 * What you will see after flashing:
 *   - Windows detects "Geye USB Composite", "Geye CDC Console", and
 *     "Geye SD Card".
 *   - The COM port prints SD and MSC counters when you type "status".
 *   - The drive is fully readable, writable, and formattable.
 *
 * Build:  idf.py build
 * Flash:  idf.py -p COMx flash monitor
 */

#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"
#include "tusb.h"

static const char *TAG = "usb_cdc_msc";

/* ── Pin assignments — change these to match your board ──────────────────── */
#define SD_CS    38
#define SD_SCK   41
#define SD_MISO  42
#define SD_MOSI  39
#define SD_HOST  SPI3_HOST
#define SD_FREQ  4000000     /* 4 MHz — safe for long wires */
#define MSC_SECTOR_BUFFER_BYTES 4096U

/* ── SD card state ───────────────────────────────────────────────────────── */
static sdmmc_card_t       s_card;
static bool               s_card_ready = false;
static sdspi_dev_handle_t s_spi_dev    = -1;

/*
 * DMA bounce buffer — must be in internal SRAM, 4-byte aligned.
 * sdmmc_read/write_sectors() performs DMA transfers and will crash if the
 * buffer is in PSRAM or not aligned. heap_caps_aligned_alloc guarantees both.
 */
static uint8_t *s_dma_buf = NULL;

static volatile uint32_t s_msc_read_calls;
static volatile uint32_t s_msc_read_errors;
static volatile uint32_t s_msc_write_calls;
static volatile uint32_t s_msc_write_errors;
static volatile uint32_t s_msc_scsi_errors;
static volatile uint32_t s_msc_last_lba;
static volatile esp_err_t s_msc_last_error = ESP_OK;

static volatile bool s_cmd_status;
static volatile bool s_cmd_unknown;
static char s_cli_buf[32];
static uint8_t s_cli_len;

/* ── Boot log — captures startup messages for replay over CDC ────────────── */
#define BOOT_LOG_CAP 1024
static char   s_boot_log[BOOT_LOG_CAP];
static size_t s_boot_log_len = 0;

static void blog(const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", tmp);
    if (n > 0 && s_boot_log_len + (size_t)n + 3 < BOOT_LOG_CAP) {
        s_boot_log_len += (size_t)snprintf(s_boot_log + s_boot_log_len,
                                           BOOT_LOG_CAP - s_boot_log_len,
                                           "%s\r\n", tmp);
    }
}

/* ── SD initialisation ───────────────────────────────────────────────────── */
static esp_err_t sd_init(void)
{
    /* Give the card time to power up (spec: ≥1 ms after Vcc stable) */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Initialise SPI bus with DMA (SPI_DMA_CH_AUTO picks a free GDMA channel).
     * USB OTG uses its own internal DMA engine, so there is no conflict. */
    const spi_bus_config_t bus = {
        .mosi_io_num     = SD_MOSI,
        .miso_io_num     = SD_MISO,
        .sclk_io_num     = SD_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 8192,
        .flags           = SPICOMMON_BUSFLAG_MASTER,
    };
    esp_err_t ret = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        blog("SD: SPI bus init FAILED (%s)", esp_err_to_name(ret));
        return ret;
    }

    ret = sdspi_host_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        blog("SD: sdspi_host_init FAILED (%s)", esp_err_to_name(ret));
        return ret;
    }

    /* Attach SD card — no card-detect, write-protect, or interrupt pins */
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.host_id  = SD_HOST;
    dev.gpio_cs  = SD_CS;
    dev.gpio_cd  = SDSPI_SLOT_NO_CD;
    dev.gpio_wp  = SDSPI_SLOT_NO_WP;
    dev.gpio_int = SDSPI_SLOT_NO_INT;

    ret = sdspi_host_init_device(&dev, &s_spi_dev);
    if (ret != ESP_OK) {
        blog("SD: attach FAILED (%s)", esp_err_to_name(ret));
        return ret;
    }

    /* sdspi always starts negotiation at 400 kHz (SD spec requirement) then
     * ramps up to max_freq_khz once the card has identified itself. */
    sdmmc_host_t host   = SDSPI_HOST_DEFAULT();
    host.slot           = s_spi_dev;
    host.max_freq_khz   = SD_FREQ / 1000U;

    memset(&s_card, 0, sizeof(s_card));
    ret = sdmmc_card_init(&host, &s_card);
    if (ret != ESP_OK) {
        blog("SD: card init FAILED (%s) — check wiring", esp_err_to_name(ret));
        sdspi_host_remove_device(s_spi_dev);
        s_spi_dev = -1;
        return ret;
    }

    s_card_ready = true;
    return ESP_OK;
}

/* ── USB PHY initialisation ──────────────────────────────────────────────── */
/*
 * Brings up the internal USB OTG PHY and starts the TinyUSB device stack.
 *
 * otg_speed = USB_PHY_SPEED_UNDEFINED lets the hardware auto-select speed
 * (full-speed for the ESP32-S3 internal PHY).  Using USB_PHY_SPEED_FULL here
 * can force an unnecessary negotiation round-trip that adds ~8 s to the first
 * enumeration.
 *
 * tusb_init() is called with an explicit device role and full-speed so that
 * TinyUSB does not need to infer these from compile-time macros.
 */
static usb_phy_handle_t s_phy;

static void usb_phy_init(void)
{
    const usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_UNDEFINED,  /* let hardware pick (full-speed) */
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_cfg, &s_phy));

    const tusb_rhport_init_t dev_init = {
        .role  = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    ESP_ERROR_CHECK(tusb_init(BOARD_TUD_RHPORT, &dev_init) ? ESP_OK : ESP_FAIL);
}

/* ── USB descriptors ─────────────────────────────────────────────────────── */
/*
 * Composite CDC + MSC device.
 *
 * bDeviceClass = 0xEF / bDeviceSubClass = 0x02 / bDeviceProtocol = 0x01
 * is the "Miscellaneous Device" class with IAD (Interface Association
 * Descriptors). Windows requires this for composite USB devices so that it
 * can correctly split the CDC and MSC interfaces to different drivers.
 */
static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0xEF,
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,   /* Espressif test VID */
    .idProduct          = 0x4005,
    .bcdDevice          = 0x0101,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/*
 * Interface layout:
 *   0 — CDC control  (+ IAD)
 *   1 — CDC data
 *   2 — MSC
 *
 * Endpoints:
 *   0x81  CDC notification  (IN  interrupt)
 *   0x02  CDC data OUT      (OUT bulk)
 *   0x82  CDC data IN       (IN  bulk)
 *   0x03  MSC OUT           (OUT bulk)
 *   0x83  MSC IN            (IN  bulk)
 */
enum { ITF_CDC = 0, ITF_CDC_DATA = 1, ITF_MSC = 2, ITF_COUNT };

#define EP_CDC_NOTIF  0x81
#define EP_CDC_OUT    0x02
#define EP_CDC_IN     0x82
#define EP_MSC_OUT    0x03
#define EP_MSC_IN     0x83

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC,  4, EP_CDC_NOTIF, 8,
                       EP_CDC_OUT, EP_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_MSC,  5, EP_MSC_OUT, EP_MSC_IN, 64),
};

static const char *s_string_desc[] = {
    (char[]){0x09, 0x04},   /* 0 — Language: English (US) */
    "Samuel Circuit Studio", /* 1 — Manufacturer */
    "Geye USB Composite",    /* 2 — Product */
    "000002",                /* 3 — Serial number */
    "Geye CDC Console",      /* 4 — CDC interface */
    "Geye SD Card",          /* 5 — MSC interface */
};
#define STRING_COUNT (sizeof(s_string_desc) / sizeof(s_string_desc[0]))

/* ── TinyUSB device descriptor callbacks ─────────────────────────────────── */
/*
 * TinyUSB calls these to fetch the descriptors it sends to the host.
 * They must be defined by the application when using TinyUSB directly
 * (i.e. without a framework wrapper like esp_tinyusb).
 */
const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_device_desc;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_config_desc;
}

static uint16_t s_desc_str[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        /* Language descriptor */
        memcpy(&s_desc_str[1], s_string_desc[0], 2);
        chr_count = 1;
    } else {
        if (index >= STRING_COUNT) return NULL;
        const char *str = s_string_desc[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) {
            s_desc_str[1 + i] = str[i];
        }
    }
    /* First word: length (bytes) + descriptor type */
    s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_desc_str;
}

/* ── TinyUSB event callbacks ─────────────────────────────────────────────── */
void tud_mount_cb(void)     { }
void tud_umount_cb(void)    { }
void tud_suspend_cb(bool r) { (void)r; }
void tud_resume_cb(void)    { }

/* ── CDC callbacks ───────────────────────────────────────────────────────── */
void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
    uint8_t ch;
    while (tud_cdc_available()) {
        if (tud_cdc_read(&ch, 1) == 0) break;
        if (ch == '\r' || ch == '\n') {
            s_cli_buf[s_cli_len] = '\0';
            if (strcmp(s_cli_buf, "status") == 0) {
                s_cmd_status = true;
            } else if (s_cli_len > 0) {
                s_cmd_unknown = true;
            }
            s_cli_len = 0;
        } else if (s_cli_len < (uint8_t)(sizeof(s_cli_buf) - 1)) {
            s_cli_buf[s_cli_len++] = (char)ch;
        }
    }
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)dtr;
    (void)rts;
}

/* ── MSC callbacks ───────────────────────────────────────────────────────── */
/*
 * Because we compile TinyUSB from source (components/tinyusb/) there are no
 * competing stub definitions — these are the ONLY definitions in the build.
 * No --wrap linker flags or __wrap_ prefixes are needed.
 */

/*
 * GET MAX LUN: return the highest LUN index supported.
 * For a single-LUN device (one SD card), this must be 0.
 * Returning 1 tells the host two LUNs exist; it then probes LUN 1,
 * gets ILLEGAL_REQUEST, and Windows reports the drive as write-protected.
 */
uint8_t tud_msc_get_maxlun_cb(void) { return 0; }

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

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (!s_card_ready || s_dma_buf == NULL) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun,
                           uint32_t *block_count,
                           uint16_t *block_size)
{
    (void)lun;
    if (s_card_ready) {
        *block_size  = (uint16_t)s_card.csd.sector_size;
        *block_count = (uint32_t)s_card.csd.capacity;
    } else {
        *block_size  = 512;
        *block_count = 0;
    }
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;   /* SD card is always writable */
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                             bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba,
                            uint32_t offset, void *buffer,
                            uint32_t bufsize)
{
    (void)lun;
    s_msc_read_calls++;
    if (!s_card_ready || s_dma_buf == NULL) {
        s_msc_read_errors++;
        s_msc_last_lba = lba;
        s_msc_last_error = ESP_ERR_INVALID_STATE;
        return -1;
    }

    const uint32_t sector_size = (uint32_t)s_card.csd.sector_size;
    if (sector_size == 0U ||
        sector_size > MSC_SECTOR_BUFFER_BYTES ||
        offset >= sector_size ||
        buffer == NULL) {
        s_msc_read_errors++;
        s_msc_last_lba = lba;
        s_msc_last_error = ESP_ERR_INVALID_ARG;
        return -1;
    }

    uint8_t *dst = (uint8_t *)buffer;
    uint32_t remaining = bufsize;
    uint32_t current_lba = lba;
    uint32_t current_offset = offset;
    while (remaining > 0U) {
        const uint32_t room = sector_size - current_offset;
        const uint32_t chunk = remaining < room ? remaining : room;
        esp_err_t rc = sdmmc_read_sectors(&s_card, s_dma_buf, current_lba, 1);
        if (rc != ESP_OK) {
            s_msc_read_errors++;
            s_msc_last_lba = current_lba;
            s_msc_last_error = rc;
            return -1;
        }
        memcpy(dst, s_dma_buf + current_offset, chunk);
        dst += chunk;
        remaining -= chunk;
        current_lba++;
        current_offset = 0U;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba,
                             uint32_t offset, uint8_t *buffer,
                             uint32_t bufsize)
{
    (void)lun;
    s_msc_write_calls++;
    if (!s_card_ready || s_dma_buf == NULL) {
        s_msc_write_errors++;
        s_msc_last_lba = lba;
        s_msc_last_error = ESP_ERR_INVALID_STATE;
        return -1;
    }

    const uint32_t sector_size = (uint32_t)s_card.csd.sector_size;
    if (sector_size == 0U ||
        sector_size > MSC_SECTOR_BUFFER_BYTES ||
        offset >= sector_size ||
        buffer == NULL) {
        s_msc_write_errors++;
        s_msc_last_lba = lba;
        s_msc_last_error = ESP_ERR_INVALID_ARG;
        return -1;
    }

    const uint8_t *src = buffer;
    uint32_t remaining = bufsize;
    uint32_t current_lba = lba;
    uint32_t current_offset = offset;
    while (remaining > 0U) {
        const uint32_t room = sector_size - current_offset;
        const uint32_t chunk = remaining < room ? remaining : room;
        if (current_offset == 0U && chunk == sector_size) {
            memcpy(s_dma_buf, src, sector_size);
        } else {
            esp_err_t rc = sdmmc_read_sectors(&s_card, s_dma_buf, current_lba, 1);
            if (rc != ESP_OK) {
                s_msc_write_errors++;
                s_msc_last_lba = current_lba;
                s_msc_last_error = rc;
                return -1;
            }
            memcpy(s_dma_buf + current_offset, src, chunk);
        }

        esp_err_t rc = sdmmc_write_sectors(&s_card, s_dma_buf, current_lba, 1);
        if (rc != ESP_OK) {
            s_msc_write_errors++;
            s_msc_last_lba = current_lba;
            s_msc_last_error = rc;
            return -1;
        }
        src += chunk;
        remaining -= chunk;
        current_lba++;
        current_offset = 0U;
    }
    return (int32_t)bufsize;
}

void tud_msc_write10_complete_cb(uint8_t lun) { (void)lun; }

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t cmd[16],
                          void *buffer, uint16_t bufsize)
{
    switch (cmd[0]) {

    case 0x1A: { /* MODE_SENSE(6) — advertise WP=0 (writable medium) */
        uint8_t resp[4] = {
            3,  /* Mode Data Length = sizeof(resp)-1 */
            0,  /* Medium Type: default (direct-access) */
            0,  /* Device-Specific Parameter: bit7=WP → 0 = writable */
            0,  /* Block Descriptor Length: 0 (no descriptor) */
        };
        uint16_t len = (bufsize < sizeof(resp)) ? bufsize : (uint16_t)sizeof(resp);
        memcpy(buffer, resp, len);
        return (int32_t)len;
    }

    case 0x5A: { /* MODE_SENSE(10) — same, WP=0 */
        uint8_t resp[8] = {
            0, 6,  /* Mode Data Length (big-endian) = sizeof(resp)-2 */
            0,     /* Medium Type */
            0,     /* Device-Specific Parameter: WP=0 */
            0,     /* Long LBA = 0 */
            0,     /* Reserved */
            0, 0,  /* Block Descriptor Length = 0 */
        };
        uint16_t len = (bufsize < sizeof(resp)) ? bufsize : (uint16_t)sizeof(resp);
        memcpy(buffer, resp, len);
        return (int32_t)len;
    }

    case 0x15: /* MODE_SELECT(6) */
    case 0x55: /* MODE_SELECT(10) */
    case 0x1E: /* PREVENT_ALLOW_MEDIUM_REMOVAL */
    case 0x2F: /* VERIFY(10) */
    case 0x35: /* SYNCHRONIZE_CACHE(10) */
    case 0x91: /* SYNCHRONIZE_CACHE(16) */
    case 0x04: /* FORMAT_UNIT */
        (void)buffer;
        (void)bufsize;
        return 0;

    default:
        s_msc_scsi_errors++;
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

/* ── USB task ────────────────────────────────────────────────────────────── */
static void usb_task(void *arg)
{
    (void)arg;
    while (1) {
        tud_task();
    }
}

/* ── CDC helpers — on-demand status only ────────────────────────────────── */
static void cdc_write(const char *s)
{
    tud_cdc_write(s, (uint32_t)strlen(s));
    tud_cdc_write_flush();
}

static void cdc_send_status(void)
{
    cdc_write("\r\n=== Boot log ===\r\n");
    const char *p = s_boot_log;
    size_t rem = s_boot_log_len;
    while (rem > 0) {
        uint32_t chunk = rem > 64 ? 64 : (uint32_t)rem;
        tud_cdc_write(p, chunk);
        tud_cdc_write_flush();
        vTaskDelay(pdMS_TO_TICKS(5));
        p += chunk;
        rem -= chunk;
    }

    cdc_write("=== SD status ===\r\n");
    char line[96];
    if (!s_card_ready) {
        cdc_write("SD: NOT READY\r\n");
    } else {
        snprintf(line, sizeof(line), "Name   : %s\r\n", s_card.cid.name);
        cdc_write(line);
        snprintf(line, sizeof(line), "Type   : %s\r\n",
                 (s_card.ocr & (1UL << 30)) ? "SDHC/SDXC" : "SDSC");
        cdc_write(line);
        snprintf(line, sizeof(line), "Speed  : %d kHz\r\n",
                 s_card.real_freq_khz);
        cdc_write(line);
        snprintf(line, sizeof(line),
                 "Sectors: %" PRIu32 " x %" PRIu32 " B\r\n",
                 (uint32_t)s_card.csd.capacity,
                 (uint32_t)s_card.csd.sector_size);
        cdc_write(line);
        uint64_t mb = ((uint64_t)s_card.csd.capacity *
                       s_card.csd.sector_size) / (1024ULL * 1024ULL);
        snprintf(line, sizeof(line), "Size   : %" PRIu64 " MB\r\n", mb);
        cdc_write(line);
        snprintf(line, sizeof(line),
                 "MSC    : R=%" PRIu32 "/%" PRIu32 " W=%" PRIu32 "/%" PRIu32 "\r\n",
                 (uint32_t)s_msc_read_calls,
                 (uint32_t)s_msc_read_errors,
                 (uint32_t)s_msc_write_calls,
                 (uint32_t)s_msc_write_errors);
        cdc_write(line);
        snprintf(line, sizeof(line),
                 "LastErr: %s LBA=%" PRIu32 " SCSI=%" PRIu32 "\r\n",
                 esp_err_to_name((esp_err_t)s_msc_last_error),
                 (uint32_t)s_msc_last_lba,
                 (uint32_t)s_msc_scsi_errors);
        cdc_write(line);
    }
    cdc_write("=================\r\n");
}

static void cdc_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!tud_mounted() || !tud_cdc_connected()) continue;
        if (s_cmd_status) {
            s_cmd_status = false;
            cdc_send_status();
        }
        if (s_cmd_unknown) {
            s_cmd_unknown = false;
            cdc_write("Unknown command. Type: status\r\n");
        }
    }
}

/* ── app_main ────────────────────────────────────────────────────────────── */
void app_main(void)
{
    blog("=== USB CDC + MSC (SD card) ===");

    /* NVS — required by some IDF internals */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    blog("NVS: OK");

    /* DMA bounce buffer — must be internal SRAM with 4-byte alignment.
     * sdmmc_read/write_sectors() performs DMA transfers; if the buffer is
     * in PSRAM or misaligned the transfer will silently corrupt data or fault.
     * heap_caps_aligned_alloc guarantees the required placement. */
    s_dma_buf = heap_caps_aligned_alloc(4, MSC_SECTOR_BUFFER_BYTES,
                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma_buf) {
        blog("DMA buf: ALLOC FAILED — halting");
        for (;;) vTaskDelay(1000);
    }
    blog("DMA buf: %p (%u B, internal SRAM)",
         (void *)s_dma_buf,
         (unsigned int)MSC_SECTOR_BUFFER_BYTES);

    blog("USB: starting TinyUSB...");
    usb_phy_init();
    blog("USB: READY as Geye USB Composite / Geye CDC Console");

    xTaskCreate(usb_task, "usb_tud", 4096, NULL, 5, NULL);
    xTaskCreate(cdc_task, "cdc_tx",  3072, NULL, 4, NULL);

    /* SD card — MSC reports no media until this succeeds. */
    blog("SD: SPI3  CS=%d SCK=%d MISO=%d MOSI=%d  %d Hz",
         SD_CS, SD_SCK, SD_MISO, SD_MOSI, SD_FREQ);
    ret = sd_init();
    if (ret == ESP_OK) {
        uint64_t mb = ((uint64_t)s_card.csd.capacity *
                       s_card.csd.sector_size) / (1024ULL * 1024ULL);
        blog("SD: READY  name=%s  %" PRIu32 " x %" PRIu32 " B = %" PRIu64 " MB",
             s_card.cid.name,
             (uint32_t)s_card.csd.capacity,
             (uint32_t)s_card.csd.sector_size,
             mb);
        blog("SD: type=%s  speed=%d kHz",
             (s_card.ocr & (1UL << 30)) ? "SDHC/SDXC" : "SDSC",
             s_card.real_freq_khz);
    } else {
        blog("SD: FAILED (%s) — MSC will report no media", esp_err_to_name(ret));
    }

    blog("Tasks started. Plug in USB cable.");
    blog("Type 'status' in CDC terminal to see SD and MSC counters.");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
