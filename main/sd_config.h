/*
 * sd_config.h — SD interface mode and pin configuration.
 *
 * Edit this file to choose between SPI, SDMMC 1-bit, and SDMMC 4-bit modes,
 * then set the GPIO numbers and clock frequency for your board.
 *
 * Only the pins for the selected mode need to be correct.
 * Unused mode sections are ignored at compile time.
 */

#pragma once

/* ── Mode selector ───────────────────────────────────────────────────────────
 *
 *   SD_MODE_SPI      — SD over SPI (sdspi_host driver).
 *                      Works on any GPIO, lowest speed, most compatible.
 *
 *   SD_MODE_SDMMC_1B — Native SDMMC 1-bit (CMD + CLK + D0 only).
 *                      Higher speed than SPI. Safe for cards that don't
 *                      support 4-bit bus.
 *
 *   SD_MODE_SDMMC_4B — Native SDMMC 4-bit (CMD + CLK + D0–D3).
 *                      Fastest option; requires 4 data lines.
 *
 * Change the value on the line below, then rebuild.
 */

#define SD_MODE_SPI      1
#define SD_MODE_SDMMC_1B 2
#define SD_MODE_SDMMC_4B 3

#define SD_MODE  SD_MODE_SPI      /* ← change this line */

/* ── SPI mode configuration (SD_MODE == SD_MODE_SPI) ────────────────────── */
/*
 * Connect the SD card to a free SPI bus.  SPI3_HOST (HSPI) is recommended
 * to avoid conflicts with a display on SPI2_HOST.
 *
 * SD_SPI_FREQ: start conservative (4 MHz).  Increase to 20–25 MHz once the
 * board is proven stable.  The driver negotiates 400 kHz for card init
 * automatically regardless of this setting.
 */
#define SD_SPI_CS    38          /* SPI chip-select (any GPIO)              */
#define SD_SPI_SCK   41          /* SPI clock                               */
#define SD_SPI_MISO  42          /* SPI MISO                                */
#define SD_SPI_MOSI  39          /* SPI MOSI                                */
#define SD_SPI_HOST  SPI3_HOST   /* SPI peripheral: SPI2_HOST or SPI3_HOST  */
#define SD_SPI_FREQ  4000000     /* bus clock Hz (4 MHz — safe for cables)  */

/* ── SDMMC native mode configuration (SD_MODE_SDMMC_1B / SD_MODE_SDMMC_4B) */
/*
 * The ESP32-S3 SDMMC controller routes signals through the GPIO matrix, so
 * any GPIO can be used.  Pull-ups (10 kΩ) are required on CMD, D0 (and D1–D3
 * for 4-bit) — the ESP32-S3 does not have internal pull-ups strong enough for
 * SDMMC at high speed.
 *
 * SD_MMC_FREQ: 20 MHz is a safe starting point.  Max is 40 MHz (HS) or
 * 80 MHz (DDR), but that requires very short traces and proper termination.
 */
#define SD_MMC_CLK   14          /* SDMMC CLK                               */
#define SD_MMC_CMD   15          /* SDMMC CMD (needs external pull-up)      */
#define SD_MMC_D0    2           /* SDMMC D0  (needs external pull-up)      */
#define SD_MMC_D1    4           /* SDMMC D1  (4-bit only, pull-up needed)  */
#define SD_MMC_D2    12          /* SDMMC D2  (4-bit only, pull-up needed)  */
#define SD_MMC_D3    13          /* SDMMC D3  (4-bit only, pull-up needed)  */
#define SD_MMC_FREQ  20000000    /* bus clock Hz (20 MHz default)           */
