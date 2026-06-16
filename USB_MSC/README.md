# ESP32-S3 USB MSC SD Card

Standalone ESP-IDF 5.5.1 example for exposing an SD card as a USB Mass
Storage Class disk on the ESP32-S3 native USB OTG port.

This example is MSC-only. It does not expose CDC. UART0 remains the console.

## Hardware

- ESP32-S3 with native USB OTG connected to the host computer.
- SD card wired in SPI mode:
  - CS: GPIO38
  - SCK: GPIO41
  - MISO: GPIO42
  - MOSI: GPIO39
- UART0/FTDI console at 115200 baud for boot logs.

## Behavior

- Initializes the SD card first.
- Starts TinyUSB only after SD probing, so the host sees ready media on first
  enumeration.
- Uses one 512-byte TinyUSB MSC endpoint buffer.
- Uses a 4096-byte internal SRAM DMA bounce buffer for SD sector reads/writes.
- Answers MODE_SENSE(6) and MODE_SENSE(10) as writable media.
- Keeps TinyUSB mount/suspend/resume callbacks silent.
- Uses a MAC-derived serial string.

The development USB identity is:

```text
VID: 0xCAFE
PID: 0x4007
BCD: 0x0103
Product: Geye MSC SD
Interface: Geye SD Card
```

## Build And Flash

```powershell
cd D:\Freelancer\gaby\USB_CDC_MSC\USB_MSC
idf.py set-target esp32s3
idf.py build
idf.py -p COM14 flash monitor
```

Use the ESP32-S3 native USB connector for the MSC disk. Use UART0/FTDI for
logs.
