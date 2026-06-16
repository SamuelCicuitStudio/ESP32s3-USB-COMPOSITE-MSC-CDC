# ESP32-S3 USB Examples

Self-contained ESP-IDF 5.5.1 USB examples for the ESP32-S3 native USB OTG
peripheral.

## Examples

- `USB_MSC_CDC`: composite USB CDC console plus USB MSC SD-card disk.
- `USB_MSC`: USB MSC-only SD-card disk.

Each folder is its own ESP-IDF project with its own `components/tinyusb`
component, `main`, partition table, and defaults.

Build from inside the example you want:

```powershell
cd USB_MSC
idf.py set-target esp32s3
idf.py build
```
