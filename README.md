# USB CDC + MSC Composite Device - ESP32-S3

A complete, self-contained ESP-IDF 5.5.1 project that exposes an SD card
(connected via SPI) as a **USB Mass Storage device** and simultaneously provides
a **USB CDC serial port** on a single native ESP32-S3 USB connector.

Validated on ESP32-S3 with Espressif ESP-IDF v5.5.1. Confirmed behavior:
USB enumerates quickly, CDC appears as `Geye CDC Console`, MSC appears as
`Geye SD Card`, and the SD card is writable from the host computer.

---

## Features

- Composite USB device: CDC serial port + MSC mass storage on one cable.
- Uses ESP32-S3 native USB OTG with TinyUSB compiled from source in this project.
- SD card served directly as a raw block device; no FAT mount on the ESP32 side.
  Windows, macOS, and Linux see the SD card's native filesystem.
- CDC port prints SD card identity, type, speed, size, and MSC counters when
  you type `status`.
- USB starts before SD probing; if the card is slow or missing, CDC/MSC still
  enumerate and MSC reports no media until the card is ready.
- Direct TinyUSB callbacks are used for MSC. `tud_msc_is_writable_cb()` belongs
  to `main/app_main.c`, so the host sees the SD card as writable.
- Boot log is buffered in RAM and replayed over CDC the first time a terminal opens,
  so you never miss startup messages even when the port is not open at boot.
- No byte-swapping, no filesystem layer, no extra chips; pure SPI SD to USB MSC.
- DMA-safe sector buffer allocated from internal SRAM with 4-byte alignment.
- Graceful degradation: if no SD card is inserted, MSC reports "no media" via
  SCSI SENSE NOT_READY; the CDC port still works.

---

## Hardware Requirements

- **ESP32-S3** development board (any variant with USB OTG pin exposed: D+/D-).
- **SD card module** wired to SPI (any standard breakout with 3.3 V logic).
- **USB cable**: one end to the ESP32-S3 USB OTG port, the other to the host PC.
  This is NOT the UART/programming port; it is the second USB connector typically
  labelled "USB" or "OTG" on dev boards.
- **Flash size**: standard 4 MB or larger.
- **PSRAM**: optional; not used by this project.

---

## Wiring

Connect your SD card module to the ESP32-S3 as follows:

| SD card pin | ESP32-S3 GPIO | Notes                          |
|-------------|---------------|--------------------------------|
| CS          | 38            | Chip select (active LOW)       |
| SCK / CLK   | 41            | SPI clock                      |
| MISO / DO   | 42            | Master In, Slave Out           |
| MOSI / DI   | 39            | Master Out, Slave In           |
| VCC         | 3.3 V         | Use the 3.3 V rail, NOT 5 V   |
| GND         | GND           |                                |

To change the pins, edit the `SD_*` defines at the top of `main/app_main.c`:

```c
#define SD_CS    38
#define SD_SCK   41
#define SD_MISO  42
#define SD_MOSI  39
```

To change the SPI clock speed (default 4 MHz), edit `SD_FREQ`. Do not exceed the
maximum supported by your SD module or cable length.

---

## Build and Flash

### Prerequisites

- Espressif ESP-IDF v5.5.1 installed and `idf.py` on your PATH.
- Target board: ESP32-S3.

### Steps

```bash
# 1. Enter the project directory
cd D:\Freelancer\gaby\USB_CDC_MSC

# 2. Set target (only needed once; sdkconfig.defaults sets CONFIG_IDF_TARGET)
idf.py set-target esp32s3

# 3. Build
idf.py build

# 4. Flash and open the UART monitor (programming port, not OTG port)
idf.py -p COM3 flash monitor
```

Replace `COM3` with the COM port of the **UART/programming** port on your board
(shown in Device Manager as "Silicon Labs CP210x" or "CH340" or similar). This is
separate from the USB OTG port.

### Plug in the OTG cable

After flashing, plug a USB cable into the **OTG port** of the ESP32-S3. Windows
will detect two new devices:

1. A **COM port** labelled "Geye CDC Console".
2. A **removable drive** labelled "Geye SD Card" pointing to the SD card.

---

## Expected Output

### UART monitor (programming port)

```
I (312) usb_cdc_msc: === USB CDC + MSC (SD card) ===
I (318) usb_cdc_msc: NVS: OK
I (322) usb_cdc_msc: DMA buf: 0x3fc9e000 (4096 B, internal SRAM)
I (328) usb_cdc_msc: USB: READY as Geye USB Composite / Geye CDC Console
I (329) usb_cdc_msc: SD: SPI3  CS=38 SCK=41 MISO=42 MOSI=39  4000000 Hz
I (891) usb_cdc_msc: SD: READY  name=SC16G  30801920 x 512 B = 14901 MB
I (892) usb_cdc_msc: SD: type=SDHC/SDXC  speed=4000 kHz
I (901) usb_cdc_msc: Tasks started. Plug in USB cable.
I (902) usb_cdc_msc: Type 'status' in CDC terminal to see SD and MSC counters.
```

### CDC port (OTG USB COM port) - open with any terminal at any baud rate

```
=== Boot log ===
=== USB CDC + MSC (SD card) ===
NVS: OK
DMA buf: 0x3fc9e000 (4096 B, internal SRAM)
USB: READY as Geye USB Composite / Geye CDC Console
SD: SPI3  CS=38 SCK=41 MISO=42 MOSI=39  4000000 Hz
SD: READY  name=SC16G  30801920 x 512 B = 14901 MB
SD: type=SDHC/SDXC  speed=4000 kHz
Tasks started. Plug in USB cable.
Type 'status' in CDC terminal to see SD and MSC counters.
=== SD status ===
Name   : SC16G
Type   : SDHC/SDXC
Speed  : 4000 kHz
Sectors: 30801920 x 512 B
Size   : 14901 MB
MSC    : R=0/0 W=0/0
LastErr: ESP_OK LBA=0 SCSI=0
=================
```

### Windows Explorer

The SD card appears as a removable drive. You can read, write, and safely eject it
just like a USB flash drive. The card's native FAT32 or exFAT filesystem is used;
no reformatting is needed.

---

## Technical Deep-Dive

### Why SPI3 at 4 MHz - not native SDMMC, not 30 MHz

**Not native SDMMC:** The ESP32-S3's native SDMMC peripheral uses specific GPIO
pins (slots 0 and 1 are mapped to fixed GPIOs on the silicon). Those pins are
typically occupied by other functions on a given board. The `sdspi` driver allows
any SPI-capable GPIOs to be used, at the cost of lower maximum throughput.

**Not 30 MHz:** The IDF `sdspi` driver has a practical ceiling around 20 MHz on
SPI3 due to GDMA transfer overhead and the single-wire SPI protocol. More
importantly, 4 MHz is chosen here as the safe default for breadboard wiring and
long cable runs. The SPI bus can be shared with other devices (display, sensors)
by lowering the clock speed to the slowest device's maximum. For a dedicated SD
card on short traces, you can raise `SD_FREQ` to 20000000 (20 MHz) safely.

**USB MSC throughput at 4 MHz SPI:** Full-speed USB MSC peaks at ~1 MB/s; SPI at
4 MHz delivers ~400 kB/s. The bottleneck is USB full-speed, not the SPI clock.
Raising the SPI clock to 10-20 MHz will not improve USB transfer rates on
full-speed USB OTG.

---

### How TinyUSB composite CDC+MSC works - IAD and bDeviceClass=0xEF

A standard USB device can only have one class per device descriptor. To expose
two different classes (CDC for serial, MSC for storage) on a single USB device,
the USB specification defines **Interface Association Descriptors (IADs)**. An IAD
groups consecutive interfaces under a single class driver, allowing the host to
assign separate drivers to each group.

Windows requires the device descriptor to declare:

```
bDeviceClass    = 0xEF   (Miscellaneous Device Class)
bDeviceSubClass = 0x02
bDeviceProtocol = 0x01   (Interface Association Descriptor protocol)
```

This signals to the Windows USB stack that IADs are present. Without this, Windows
may attempt to load a single driver for the entire composite device, which fails for
mixed CDC+MSC combinations.

The configuration descriptor then contains:

1. A standard configuration header.
2. A CDC IAD + CDC control interface + CDC data interface.
3. An MSC interface.

The TinyUSB macros `TUD_CDC_DESCRIPTOR` and `TUD_MSC_DESCRIPTOR` generate the
correct IAD+interface blocks automatically.

---

### Why direct TinyUSB callbacks are used

This project compiles TinyUSB from source as `components/tinyusb`, so the app owns
the real `tud_msc_*`, `tud_cdc_*`, and descriptor callbacks directly. Do not use
`--wrap` here. TinyUSB calls `tud_msc_is_writable_cb()` from inside
`msc_device.c`; direct callback ownership is the clearest way to guarantee the SD
card is reported writable.

If another component also provides strong TinyUSB callbacks, remove that conflict
or compile those callbacks out for the test build. Do not rely on
`--allow-multiple-definition` or link order for MSC writable behavior.

---

### Why the DMA bounce buffer must use `heap_caps_aligned_alloc` with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`

`sdmmc_read_sectors()` and `sdmmc_write_sectors()` drive the SPI peripheral via
the ESP32-S3's GDMA (General DMA) engine. GDMA imposes two constraints:

1. **Internal SRAM only:** GDMA cannot access PSRAM (external SPI RAM). If the
   buffer is allocated with `heap_caps_malloc(n, MALLOC_CAP_SPIRAM)` or with the
   default allocator when PSRAM is the primary heap, the DMA transfer will silently
   read from or write to the wrong address, corrupting data or causing a bus fault.

2. **4-byte alignment:** GDMA descriptors require the buffer start address to be
   4-byte aligned. `malloc()` and `heap_caps_malloc()` on IDF typically return
   8-byte aligned addresses, but `heap_caps_aligned_alloc(4, n, caps)` is explicit
   and immune to future allocator changes.

Using `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` together ensures the allocator selects
a region of internal SRAM that is mapped into the GDMA address space. This is the
correct pattern for any buffer used directly by a DMA peripheral on ESP32-S3.

---

### Why USB starts before SD probing in this test

This test intentionally installs TinyUSB first, then probes the SD card. That
keeps the CDC console available quickly, even if the SD card is slow, absent, or
electrically unstable during bring-up.

While the card is not ready, `tud_msc_test_unit_ready_cb()` reports no media and
the SCSI sense state is set to NOT_READY. After SD probing succeeds, the same MSC
LUN reports the real sector count and accepts reads/writes. If a host caches the
early no-media state too aggressively, unplugging and reconnecting the OTG cable
forces a clean re-enumeration against the now-ready SD card.

For production firmware, the policy can choose either startup mode:

- **Fast diagnostic mode**: USB first, SD second. Best for CDC logs and recovery.
- **Storage-first mode**: SD first, USB second. Best when the host must see the
  drive ready immediately on the first enumeration.

---

### The boot log buffer pattern

The CDC serial port is not visible to the host until USB enumeration completes and a
terminal application opens the port (asserts DTR). This happens several hundred
milliseconds to a few seconds after the board powers up; long after `app_main` has
run its initialisation sequence.

Without the boot log buffer, all startup messages (SD card identity, DMA buffer
address, error codes) are lost. A developer opening a terminal after the board has
booted would have no way to know what happened at startup.

The boot log pattern solves this by accumulating formatted messages into a static
`char s_boot_log[1024]` buffer. The CDC task accepts the `status` command and
then replays the boot log followed by live SD and MSC counters. The buffer is
never cleared; every reconnect can request the same boot-time snapshot.

The buffer is kept small (1024 bytes) to avoid fragmentation. Messages longer than
the buffer are silently truncated at the buffer boundary.

---

## Tested Hardware

| Board              | IDF version | Result                       |
|--------------------|-------------|------------------------------|
| ESP32-S3-WROOM-1   | 5.5.1       | Pass - CDC + MSC both work   |

SD cards tested: SanDisk Ultra 16 GB SDHC, Samsung EVO 32 GB SDHC.

---

## License

MIT License

Copyright (c) 2025

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
