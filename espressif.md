# ESP-IDF Development Guide (`espressif.md`)

Best practices for building, configuring, flashing, and debugging firmware using **Espressif ESP-IDF (v5.5.6 or newer)** on the Waveshare ESP32-S3-Touch-AMOLED-1.75 / 1.75C.

---

## 1. Official Board Support Package (BSP)

Waveshare publishes an official managed component in the Espressif Component Registry that handles QSPI display setup, CST9217 touch, AXP2101 PMIC, ES8311/ES7210 audio, and LVGL porting.

### Add the BSP to `main/idf_component.yml`
```yaml
dependencies:
  idf: ">=5.5.6"
  waveshare/esp32_s3_touch_amoled_1_75: "*"
```

Or add it via CLI:
```bash
idf.py add-dependency "waveshare/esp32_s3_touch_amoled_1_75"
```

### Individual Component Dependencies (When Not Using Full BSP)
If building a custom lightweight HAL without the full BSP, include only the required drivers:
```yaml
dependencies:
  espressif/esp_lcd_co5300: "^1.0.0"
  espressif/esp_lcd_touch_cst9217: "^1.0.0" # or waveshare touch component
  espressif/esp_codec_dev: "^1.1.0"
  espressif/esp_lvgl_port: "^2.0.0"
  lewisxhe/XPowersLib: "*"
```

---

## 2. Mandatory `sdkconfig.defaults`

Always commit these baseline settings into `sdkconfig.defaults` for the **ESP32-S3R8**:

```ini
# Target
CONFIG_IDF_TARGET="esp32s3"

# CPU Frequency (240 MHz for smooth 60fps LVGL and audio DSP)
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y

# Flash Configuration (QIO @ 80MHz, 32MB verified on our unit: c8:4019)
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y

# PSRAM Configuration (CRITICAL: ESP32-S3R8 uses OCTAL PSRAM)
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536

# Native USB Serial/JTAG Console (GPIO 19/20)
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y

# Cache Configuration (Boosts PSRAM + Flash throughput for LVGL)
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y

# FreeRTOS Tick Rate
CONFIG_FREERTOS_HZ=1000

# LVGL Optimization
CONFIG_LV_COLOR_16_SWAP=y
CONFIG_LV_MEM_CUSTOM=y
```

> [!CAUTION]
> **Internal DMA Memory Reservation**: QSPI DMA descriptors and Wi-Fi buffers **must** reside in internal SRAM (`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`). Never set `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` too low (keep $\ge 16384$ bytes) and always reserve at least `65536` bytes of internal RAM (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`), otherwise QSPI display flushes or SDMMC transfers will fail with `ESP_ERR_NO_MEM`.

---

## 3. Memory & DMA Architecture Best Practices

### Framebuffers & Draw Buffers
A full-frame `466 × 466` RGB565 buffer is **`434,312` bytes (~424 KB)**—too large to fit in internal SRAM alongside Wi-Fi and FreeRTOS tasks.
* **Strategy A (Recommended for 60 FPS): Internal SRAM Partial DMA Buffers**
  Allocate two `466 × 46` line buffers (~42.8 KB each) in internal DMA-capable SRAM (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`). Double-buffering allows the CPU to render into Buffer B while QSPI DMA transmits Buffer A at 80 MHz without PSRAM bus contention.
* **Strategy B (Full Screen Buffer in PSRAM)**
  Allocate full-screen buffers in Octal PSRAM (`MALLOC_CAP_SPIRAM`) with a small internal SRAM bounce buffer for QSPI DMA. Required if doing full-screen canvas blends or complex canvas rotations.

### Task Core Pinning (`xTaskCreatePinnedToCore`)
* **Core 0 (`PRO_CPU`)**: Wi-Fi, BLE controller, TCP/IP stack, network HTTP/WebSockets.
* **Core 1 (`APP_CPU`)**: LVGL rendering task (`esp_lvgl_port`), QSPI display flush, touch & IMU processing, real-time audio I²S loops.

---

## 4. Partition Table Strategy

With 16 MB or 32 MB of flash, never stick to the default 4 MB partition table. Create a custom `partitions.csv`:

```csv
# Name,   Type, SubType, Offset,  Size,     Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
otadata,  data, ota,     0x10000, 0x2000,
ota_0,    app,  ota_0,   0x20000, 0x400000,
ota_1,    app,  ota_1,   0x420000,0x400000,
storage,  data, spiffs,  0x820000,0x7E0000,
```
*(If using ESP-SR voice recognition models, add a `model` partition of subtype `spiffs` or `fat` per `esp-sr` documentation.)*

---

## 5. Essential `idf.py` Commands (macOS)

```bash
# 1. Verify Flash Size (16MB vs 32MB) and MAC Address
esptool.py --port /dev/cu.usbmodem* flash_id

# 2. Set Target (run once on new project)
idf.py set-target esp32s3

# 3. Build Firmware
idf.py build

# 4. Flash & Open Serial Monitor (with automatic panic backtrace decoding)
idf.py -p /dev/cu.usbmodem* flash monitor

# 5. Analyze RAM/Flash Footprint
idf.py size-components

# 6. Full Clean (when changing sdkconfig.defaults)
rm -rf build sdkconfig && idf.py build
```
*(Exit `idf.py monitor` anytime with `Ctrl+]`.)*
