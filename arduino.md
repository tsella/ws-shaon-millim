# Arduino & PlatformIO Guide (`arduino.md`)

Best practices for developing with **PlatformIO** (recommended over Arduino IDE for reproducible dependency management) or **Arduino IDE 2.x** using `arduino-esp32` Core v3.x+.

---

## 1. PlatformIO Configuration (`platformio.ini`)

Use this battle-tested `platformio.ini` template for the **Waveshare ESP32-S3-Touch-AMOLED-1.75 / 1.75C**:

```ini
[env:waveshare_s3_amoled_175c]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino

; Memory Topology: 16MB (or 32MB) QIO Flash + 8MB Octal PSRAM
board_build.arduino.memory_type = qio_opi
board_build.f_cpu = 240000000L
board_build.f_flash = 80000000L
board_build.flash_mode = qio
board_upload.flash_size = 16MB
board_build.partitions = default_16MB.csv

; Native USB Serial/JTAG Configuration (GPIO 19/20)
build_flags = 
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DBOARD_HAS_PSRAM
    -DLV_CONF_INCLUDE_SIMPLE
    -I include

monitor_speed = 115200
monitor_filters = esp32_exception_decoder, colorize

lib_deps =
    moononournation/GFX Library for Arduino @ ^1.4.9
    lewisxhe/XPowersLib @ ^0.2.6
    lewisxhe/SensorLib @ ^0.2.2
    lvgl/lvgl @ ^8.4.0
```

> [!IMPORTANT]
> **`qio_opi` Memory Type**: The ESP32-S3R8 has **Quad Flash (`qio`)** and **Octal PSRAM (`opi`)**. If you leave the default `qio_qspi`, the board will crash in a boot loop as soon as `psramInit()` runs.

---

## 2. Arduino IDE 2.x Tool Menu Settings

If using the Arduino IDE directly, select **ESP32S3 Dev Module** and configure:
* **USB CDC On Boot**: `"Enabled"` *(Required for `Serial.println()` over USB-C)*
* **USB Mode**: `"Hardware CDC and JTAG"`
* **CPU Frequency**: `"240MHz (WiFi)"`
* **Flash Mode**: `"QIO 80MHz"`
* **Flash Size**: `"16MB (128Mb)"` *(or 32MB if verified)*
* **PSRAM**: `"OPI PSRAM"` *(CRITICAL)*
* **Partition Scheme**: `"16M Flash (3MB APP/9.9MB FATFS)"` or `"Huge APP"`

---

## 3. Core Library Stack & Initialization Order

In `setup()`, initialize hardware buses in this exact order to prevent bus lockups:

1. **Serial (`Serial.begin(115200)`)**
2. **Primary I²C Bus (`Wire.begin(15, 14, 400000)`)** — Must come first!
3. **AXP2101 PMIC (`XPowersAXP2101`)** — Ensure peripheral LDO rails are enabled.
4. **TCA9554 I/O Expander (`0x20`)** — Configure input/output directions for `EXIO0`–`EXIO7`.
5. **Display (`Arduino_CO5300` over `Arduino_ESP32QSPI`)** — Initialize QSPI and send `0x51` brightness.
6. **Touch (`CST9217`) & IMU (`QMI8658`)** — Attach to `Wire`.

### Minimal Working Display + I²C Bring-Up (Arduino_GFX)
```cpp
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

// 1. QSPI Bus (CS=12, SCK=38, D0=4, D1=5, D2=6, D3=7)
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    12 /* CS */, 38 /* SCK */, 4 /* D0 */, 5 /* D1 */, 6 /* D2 */, 7 /* D3 */
);

// 2. CO5300 466x466 Round AMOLED (RST=39, col_offset1=6 is MANDATORY)
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, 39 /* RST */, 0 /* rotation */, false /* IPS */,
    466 /* width */, 466 /* height */,
    6 /* col_offset1 (x_gap) */, 0 /* row_offset1 */,
    0 /* col_offset2 */, 0 /* row_offset2 */
);

void setDisplayBrightness(uint8_t level) {
    bus->beginWrite();
    bus->writeC8D8(0x51, level); // MIPI DCS Write Display Brightness
    bus->endWrite();
}

void setup() {
    Serial.begin(115200);
    Wire.begin(15 /* SDA */, 14 /* SCL */, 400000);

    if (!gfx->begin()) {
        Serial.println("CO5300 init failed!");
    }
    gfx->fillScreen(BLACK);
    setDisplayBrightness(200);
}
```

---

## 4. PSRAM Allocation in Arduino

* `malloc()` in Arduino allocates from internal SRAM first unless configured otherwise.
* For large assets (audio buffers, JSON payloads, sprite sheets, full-screen canvas), explicitly allocate in the 8 MB Octal PSRAM using **`ps_malloc()`**, **`ps_calloc()`**, or `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`.
* Verify PSRAM at startup:
  ```cpp
  Serial.printf("Total PSRAM: %u bytes, Free PSRAM: %u bytes\n",
                ESP.getPsramSize(), ESP.getFreePsram());
  ```
