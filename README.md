# USB CDC + MSC Composite Device — ESP32-S3

A complete, self-contained ESP-IDF 5.5.1 project that exposes an SD card as a
**USB Mass Storage device** and simultaneously provides a **USB CDC serial port**
on a single native ESP32-S3 USB connector.

The SD interface is configurable: choose between **SPI**, **SDMMC 1-bit**, or
**SDMMC 4-bit** by editing one line in `main/sd_config.h`. No other file needs
to change.

Validated on ESP32-S3 with ESP-IDF v5.5.1. Confirmed behavior: USB enumerates
quickly, CDC appears as `Geye CDC Console`, MSC appears as `Geye SD Card`, and
the SD card is fully readable, writable, and formattable from the host computer.

---

## Features

- Composite USB device: CDC serial port + MSC mass storage on one cable.
- Three SD interface modes selectable via a single `#define` in `sd_config.h`:
  - **SPI** — works on any GPIO, most compatible, lowest speed.
  - **SDMMC 1-bit** — native SDMMC peripheral, faster than SPI.
  - **SDMMC 4-bit** — fastest option; requires 4 data lines.
- Uses ESP32-S3 native USB OTG with TinyUSB compiled from source in this project.
- SD card served as a raw block device; no FAT mount on the ESP32 side.
  Windows, macOS, and Linux see the SD card's native filesystem.
- FAT volume label stamped as `GEYE` at boot — the drive shows the name
  immediately in Windows Explorer without formatting.
- CDC port replays the full boot log and live counters when you type `status`.
- USB starts before SD probing: CDC/MSC enumerate even if the card is slow or
  absent; MSC reports no media until the card is ready.
- DMA-safe sector buffer in internal SRAM with 4-byte alignment.
- Graceful degradation: no SD card → MSC reports SCSI NOT_READY; CDC still works.

---

## Quick-Start: Choosing the SD Interface

Open `main/sd_config.h` and change **one line**:

```c
#define SD_MODE  SD_MODE_SPI        // SPI (default)
// #define SD_MODE  SD_MODE_SDMMC_1B   // SDMMC 1-bit
// #define SD_MODE  SD_MODE_SDMMC_4B   // SDMMC 4-bit
```

Then set the GPIO numbers and clock frequency for your board in the same file
(see the sections below). Rebuild — done.

---

## Hardware Requirements

- **ESP32-S3** development board with the USB OTG D+/D− pins exposed.
- **SD card module** wired according to the mode you selected.
- **USB cable** from the ESP32-S3 **OTG port** to the host PC.
  This is the second USB connector (not the UART/programming port).
- **Flash size**: 4 MB or larger.
- **PSRAM**: not used by this project.

---

## Wiring

### SPI mode (`SD_MODE_SPI`) — default

Any SPI-capable GPIOs work. Defaults in `sd_config.h`:

| SD card pin | ESP32-S3 GPIO | `sd_config.h` define |
|-------------|---------------|----------------------|
| CS          | 38            | `SD_SPI_CS`          |
| SCK / CLK   | 41            | `SD_SPI_SCK`         |
| MISO / DO   | 42            | `SD_SPI_MISO`        |
| MOSI / DI   | 39            | `SD_SPI_MOSI`        |
| VCC         | 3.3 V         | —                    |
| GND         | GND           | —                    |

Default clock: **4 MHz** (`SD_SPI_FREQ`). Safe for long wires and breadboards.
Raise to `20000000` (20 MHz) for short traces.

No pull-ups required for SPI mode.

### SDMMC 1-bit mode (`SD_MODE_SDMMC_1B`)

| SD card pin | ESP32-S3 GPIO | `sd_config.h` define |
|-------------|---------------|----------------------|
| CLK         | 14            | `SD_MMC_CLK`         |
| CMD         | 15            | `SD_MMC_CMD`         |
| D0          | 2             | `SD_MMC_D0`          |
| VCC         | 3.3 V         | —                    |
| GND         | GND           | —                    |

**Pull-ups required:** 10 kΩ on CMD and D0. The ESP32-S3 does not have internal
pull-ups strong enough for SDMMC at speed.

### SDMMC 4-bit mode (`SD_MODE_SDMMC_4B`)

| SD card pin | ESP32-S3 GPIO | `sd_config.h` define |
|-------------|---------------|----------------------|
| CLK         | 14            | `SD_MMC_CLK`         |
| CMD         | 15            | `SD_MMC_CMD`         |
| D0          | 2             | `SD_MMC_D0`          |
| D1          | 4             | `SD_MMC_D1`          |
| D2          | 12            | `SD_MMC_D2`          |
| D3          | 13            | `SD_MMC_D3`          |
| VCC         | 3.3 V         | —                    |
| GND         | GND           | —                    |

**Pull-ups required:** 10 kΩ on CMD, D0, D1, D2, D3.

Default clock: **20 MHz** (`SD_MMC_FREQ`). Can be raised to 40 MHz on short
traces with good pull-ups.

---

## Configuration Reference (`main/sd_config.h`)

```c
/* Mode — change this one line */
#define SD_MODE  SD_MODE_SPI      // SD_MODE_SPI | SD_MODE_SDMMC_1B | SD_MODE_SDMMC_4B

/* SPI pins (used when SD_MODE == SD_MODE_SPI) */
#define SD_SPI_CS    38
#define SD_SPI_SCK   41
#define SD_SPI_MISO  42
#define SD_SPI_MOSI  39
#define SD_SPI_HOST  SPI3_HOST   // SPI2_HOST or SPI3_HOST
#define SD_SPI_FREQ  4000000     // Hz

/* SDMMC pins (used when SD_MODE == SD_MODE_SDMMC_1B or SD_MODE_SDMMC_4B) */
#define SD_MMC_CLK   14
#define SD_MMC_CMD   15
#define SD_MMC_D0    2
#define SD_MMC_D1    4    // 4-bit only
#define SD_MMC_D2    12   // 4-bit only
#define SD_MMC_D3    13   // 4-bit only
#define SD_MMC_FREQ  20000000    // Hz
```

All other project files are left unchanged when you switch modes.

---

## Build and Flash

### Prerequisites

- Espressif ESP-IDF v5.5.1 installed and `idf.py` on your PATH.
- Target: ESP32-S3.

### Steps

```bash
# 1. Enter the project directory
cd USB_CDC_MSC

# 2. Set target (only needed once)
idf.py set-target esp32s3

# 3. Build
idf.py build

# 4. Flash via the UART/programming port, then open the monitor
idf.py -p COM3 flash monitor
```

Replace `COM3` with the COM port of your board's **UART/programming** port
(not the OTG port). After flashing, plug a USB cable into the **OTG port** and
Windows will detect two new devices.

---

## Expected Output

### UART monitor

```
I (312) usb_cdc_msc: === USB CDC + MSC (SD card) ===
I (318) usb_cdc_msc: NVS: OK
I (322) usb_cdc_msc: DMA buf: 0x3fc9e000 (4096 B, internal SRAM)
I (328) usb_cdc_msc: USB: READY as Geye USB Composite / Geye CDC Console
I (329) usb_cdc_msc: SD: SPI  host=1  CS=38 SCK=41 MISO=42 MOSI=39  4000000 Hz
I (891) usb_cdc_msc: Volume label 'GEYE       ' written (FAT32 VBR@2048 root@8192)
I (892) usb_cdc_msc: SD: READY  name=SC16G  30801920 x 512 B = 14901 MB  label=GEYE
I (893) usb_cdc_msc: SD: type=SDHC/SDXC  speed=4000 kHz
I (901) usb_cdc_msc: Tasks started. Plug in USB cable.
```

### CDC port — type `status` to query

```
=== Boot log ===
=== USB CDC + MSC (SD card) ===
NVS: OK
...
=== SD status ===
Name   : SC16G
Type   : SDHC/SDXC
Speed  : 4000 kHz
Sectors: 30801920 x 512 B
Size   : 14901 MB
MSC    : R=42/0 W=18/0
LastErr: ESP_OK LBA=0 SCSI=0
=================
```

### Windows Explorer

The SD card appears as a removable drive named **GEYE**. You can read, write,
format, and safely eject it exactly like a USB flash drive.

---

## Technical Notes

### How the SD mode abstraction works

`main/sd_config.h` defines the three mode constants and a single `SD_MODE` macro.
`app_main.c` includes `sd_config.h` first and uses `#if SD_MODE == ...` guards to:

- Pull in either `driver/sdspi_host.h` + `driver/spi_master.h` (SPI mode) or
  `driver/sdmmc_host.h` (SDMMC modes).
- Compile `sdspi_dev_handle_t s_spi_dev` only in SPI mode.
- Choose the correct host and slot init path inside `sd_init()`.

`CMakeLists.txt` lists both `esp_driver_sdspi` and `esp_driver_sdmmc` in REQUIRES
so the linker has both drivers available regardless of the selected mode.

### Why USB starts before SD probing

TinyUSB is installed first so the CDC console is available immediately, even if
the SD card is slow or absent. While the card is not ready,
`tud_msc_test_unit_ready_cb()` returns false and sets SCSI NOT_READY. After
`sd_init()` succeeds, the MSC LUN reports the real sector count. If the host
cached the early not-ready state, unplugging and reconnecting the OTG cable
forces a clean re-enumeration.

### Why `tud_msc_get_maxlun_cb` returns 0

Returning 0 means one LUN (LUN 0 = the SD card). Returning 1 would tell the host
two LUNs exist; it would probe LUN 1, receive ILLEGAL_REQUEST, and Windows would
mark the drive write-protected.

### DMA bounce buffer

`sdmmc_read/write_sectors()` transfers data via GDMA. GDMA requires the buffer
to be in internal SRAM (not PSRAM) and 4-byte aligned. `heap_caps_aligned_alloc`
with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` satisfies both constraints. In SDMMC
native mode the peripheral has its own internal DMA engine, but keeping the buffer
in internal SRAM is still correct and safe.

### FAT volume label

`sd_set_volume_label()` writes the label to two locations so Windows sees it
immediately without a reconnect:
1. The BPB VolumeLabel field in the boot sector (offset 43 for FAT16/12, 71 for FAT32).
2. A root-directory entry with `ATTR_VOLUME_ID` (0x08) — Windows reads this as authoritative.

It is called after `sdmmc_card_init()` but before `s_card_ready = true`, so the
USB host receives NOT_READY during the write and cannot race the sector buffer.

### Composite CDC + MSC — why `bDeviceClass = 0xEF`

Windows requires `bDeviceClass=0xEF / bDeviceSubClass=0x02 / bDeviceProtocol=0x01`
(Miscellaneous Device Class with Interface Association Descriptors) to correctly
split CDC and MSC to separate drivers. Without this, Windows may try to load one
driver for the whole device and fail. The TinyUSB `TUD_CDC_DESCRIPTOR` macro
emits the IAD automatically.

---

## Tested Hardware

| Board            | IDF version | SD mode | Result |
|------------------|-------------|---------|--------|
| ESP32-S3-WROOM-1 | 5.5.1       | SPI     | Pass — CDC + MSC both work |

SD cards tested: SanDisk Ultra 16 GB SDHC, Samsung EVO 32 GB SDHC.

---

## License

MIT License — Copyright (c) 2025

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
