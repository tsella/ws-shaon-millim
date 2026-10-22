# AGENTS.md — Waveshare ESP32-S3-Touch-AMOLED-1.75 / 1.75C

Single source of truth for developing, flashing, debugging, and optimizing firmware on the **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (and standard **-1.75** variant).

---

## 1. Documentation Index

Read the relevant subsystem guide before writing or modifying hardware-facing code:

| Reference File | Scope & When to Read |
| :--- | :--- |
| [espressif.md](espressif.md) | **ESP-IDF** workflow, Official BSP (`waveshare/esp32_s3_touch_amoled_1_75`), `sdkconfig` defaults, PSRAM/Flash config, partition tables, `idf.py` CLI. |
| [arduino.md](arduino.md) | **Arduino / PlatformIO** setup, Board settings, `Arduino_GFX` (CO5300), `XPowersLib`, `SensorLib`, LVGL integration. |
| [display.md](display.md) | **1.75" Round AMOLED (CO5300, 466×466 QSPI)**, **6px X-offset gotcha**, **2px even-boundary alignment (`rounder_cb`)**, hardware brightness (`0x51`), circular UI rules. |
| [controls.md](controls.md) | **Touch (CST9217)**, **6-Axis IMU (QMI8658)**, **RTC (PCF85063)**, **I/O Expander (TCA9554)**, physical `BOOT` & `PWR` buttons. |
| [audio.md](audio.md) | **Audio Subsystem**: `ES8311` (DAC/Speaker), `ES7210` (4-ch ADC / Dual-Mic array + AEC), `NS4150B` (PA Amp), shared I2S bus rules, ESP-SR voice pipelines. |
| [power.md](power.md) | **AXP2101 PMIC**, LDO rail maps (`ALDO1`–`4`, `DLDO1`), LiPo battery charging/telemetry, deep/light sleep, AMOLED power optimization. |
| [lessons.md](lessons.md) | **Living Engineering Log**: Project history, discovered hardware quirks, verified workarounds, and benchmark notes updated over time. |

---

## 2. Hardware Specifications Summary

* **SoC**: Espressif **ESP32-S3R8** (Dual-core Xtensa 32-bit LX7 @ up to 240 MHz, Wi-Fi 802.11 b/g/n + BLE 5.0).
* **Memory**:
  * **SRAM**: 512 KB internal SRAM + 16 KB RTC SRAM.
  * **PSRAM**: **8 MB Octal PSRAM** (Stacked in-package `R8`, AP_3v3 — **must use `OPI` mode**).
  * **Flash**: **32 MB** Quad SPI NOR Flash (GigaDevice `c8:4019`, `QIO` @ 80 MHz, 3.3V — verified on our hardware unit!).
* **Unit Identifiers**:
  * **Serial Port (macOS)**: `/dev/cu.usbmodem*` (e.g. `/dev/cu.usbmodem101`, `USB-Serial/JTAG`)
  * **ESP-IDF Environment**: `~/esp/esp-idf` (v5.3.6+) + `~/.espressif/python_env/idf5.3_py3.11_env`
* **Display**: 1.75" Round AMOLED, **466 × 466** pixels, 16.7M colors, **CO5300** QSPI driver.
* **Touch**: **CST9217** capacitive touch controller (I²C).
* **Audio**: Dual-microphone array with hardware acoustic echo cancellation (AEC) feedback channel via **ES7210** ADC + **ES8311** DAC + **NS4150B** Class-D speaker amp.
* **Sensors & Time**: **QMI8658** 6-axis IMU (Accel + Gyro), **PCF85063** RTC.
* **Power**: **AXP2101** PMIC with 3.7V MX1.25 LiPo battery management.
* **Variant Note (`-1.75C` vs `-1.75` CRITICAL PINOUT DIFFERENCES)**:
  * The **`-1.75C`** (cased badge variant, BSP `waveshare/esp32_s3_touch_amoled_1_75c`) and standard **`-1.75`** (bare board with SD slot, BSP `waveshare/esp32_s3_touch_amoled_1_75`) have **different GPIOs** for `I2S_MCLK`, resets, and `PWR` button:
    * **`I2S_MCLK`**: **`GPIO 16`** on **`-1.75C`** vs **`GPIO 42`** on **`-1.75`**! (Using `GPIO 42` on `-1.75C` produces zero audio!)
    * **`LCD_RST`**: **`GPIO 1`** on **`-1.75C`** vs **`GPIO 39`** on **`-1.75`**.
    * **`TP_RST`**: **`GPIO 2`** on **`-1.75C`** vs **`GPIO 40`** on **`-1.75`**.
    * **`PWR` Button**: **`GPIO 3`** (Active High via BSS138 inverter) on **`-1.75C`** vs `TCA9554 EXIO4` (`0x20`) on **`-1.75`**.
    * **`TCA9554` (`0x20`) & `PCF85063` (`0x51`)**: Not populated on **`-1.75C`**.

---

## 3. Master Pinout & Bus Map

### A. Display Bus — CO5300 (QSPI)
| Signal | ESP32-S3 GPIO | Notes |
| :--- | :--- | :--- |
| `LCD_CS` | **GPIO 12** | QSPI Chip Select |
| `LCD_SCK` (PCLK) | **GPIO 38** | QSPI Clock (up to 80 MHz) |
| `LCD_D0` (SIO0) | **GPIO 4** | QSPI Data 0 |
| `LCD_D1` (SIO1) | **GPIO 5** | QSPI Data 1 |
| `LCD_D2` (SIO2) | **GPIO 6** | QSPI Data 2 |
| `LCD_D3` (SIO3) | **GPIO 7** | QSPI Data 3 |
| `LCD_RST` | **GPIO 39** | Display Hardware Reset *(Active Low; also check `EXIO0` on some board revs)* |

### B. Primary Shared I²C Bus (`400 kHz`)
* **SDA**: **GPIO 15**
* **SCL**: **GPIO 14**

| I²C Address | Chip | Function | Extra Pins / Interrupts |
| :--- | :--- | :--- | :--- |
| `0x18` | **ES8311** | Mono Audio DAC / Codec | Shared I²S bus |
| `0x20` | **TCA9554** | 8-bit I/O Expander (`EXIO0`–`EXIO7`) | Manages IRQ/status & sub-resets |
| `0x34` | **AXP2101** | Power Management IC (PMIC) | `AXP_IRQ` $\rightarrow$ `EXIO5` |
| `0x40` | **ES7210** | 4-Channel Audio ADC (Dual Mics + AEC) | Shared I²S bus |
| `0x51` | **PCF85063** | Real-Time Clock (RTC) | `RTC_INT` $\rightarrow$ `EXIO3` |
| `0x5A` | **CST9217** | Capacitive Touch Controller | `TP_INT` $\rightarrow$ **GPIO 11**, `TP_RST` $\rightarrow$ **GPIO 40** *(or `EXIO2`)* |
| `0x6B` | **QMI8658** | 6-Axis IMU (Accelerometer + Gyro) | `IMU_INT1` $\rightarrow$ `EXIO6` |

### C. TCA9554 I/O Expander (`0x20`) Pin Map
| Pin | Signal | Dir | Role |
| :--- | :--- | :--- | :--- |
| `EXIO0` | `EXIO0` / `LCD_RST` | Out/IO | Auxiliary IO / Display reset on certain revisions |
| `EXIO1` | `EXIO1` | IO | Test point / Expansion |
| `EXIO2` | `EXIO2` / `TP_RST` | Out/IO | Auxiliary IO / Touch reset on certain revisions |
| `EXIO3` | `RTC_INT` | In | PCF85063 Alarm/Timer Interrupt |
| `EXIO4` | `PWR_BTN` (`SYS_OUT`) | In | **Physical PWR Button State** (High = Pressed, Low = Released) |
| `EXIO5` | `AXP_IRQ` | In | AXP2101 PMIC Interrupt |
| `EXIO6` | `QMI_INT1` | In | QMI8658 IMU Interrupt 1 |
| `EXIO7` | `GPS_RST` | Out | LC76G Reset (used on `-G` variant) |

### D. Audio Bus — Shared I²S + Amplifier
| Signal | ESP32-S3 GPIO | Notes |
| :--- | :--- | :--- |
| `I2S_MCLK` | **GPIO 16** (`-1.75C`) / **GPIO 42** (`-1.75`) | **Master Clock to ES8311 & ES7210 (`-1.75C` MUST use GPIO 16!)** |
| `I2S_BCLK` (SCLK) | **GPIO 9** | Bit Clock |
| `I2S_LRCK` (WS) | **GPIO 45** | Word Select / Frame Clock *(Strapping pin: do not pull high externally at boot)* |
| `I2S_DOUT` (DSDIN) | **GPIO 8** | Playback data from ESP32-S3 $\rightarrow$ ES8311 (Speaker) |
| `I2S_DIN` (ASDOUT) | **GPIO 10** | Capture data from ES7210 (Mics) $\rightarrow$ ESP32-S3 |
| `PA_CTRL` | **GPIO 46** | NS4150B Power Amp Enable *(High = Enable; Strapping pin: must stay Low at boot)* |

### E. Storage — MicroSD / TF Slot (1-Bit SDMMC / SPI)
| Signal | ESP32-S3 GPIO | Notes |
| :--- | :--- | :--- |
| `SD_CMD` (MOSI) | **GPIO 1** | SDMMC Command |
| `SD_CLK` (SCLK) | **GPIO 2** | SDMMC Clock |
| `SD_D0` (MISO) | **GPIO 3** | SDMMC Data 0 |
| `SD_CS` | **GPIO 41** | Chip Select (used in SPI mode) |

### F. Buttons, USB & Expansion Header (8-Pin 2.54mm)
* **`BOOT` Button**: **GPIO 0** (Active Low; also strapping pin for ROM bootloader).
* **`PWR` Button**: Read via TCA9554 **`EXIO4`** (Active High when pressed) or AXP2101 IRQ events. Long-press (>6s) triggers hard PMIC power-off.
* **Native USB-C**: **GPIO 19** (`USB_D-`), **GPIO 20** (`USB_D+`) — Hardware USB Serial/JTAG.
* **8-Pin Header**: `VBUS`, `GND`, `3V3`, `UART0_TX` (**GPIO 43**), `UART0_RX` (**GPIO 44**), **GPIO 16**, **GPIO 17**, **GPIO 18**.

---

## 4. Golden Rules & Critical Gotchas

1. **PSRAM Must Be Octal (`OPI`)**: The ESP32-S3R8 uses **Octal PSRAM** (occupying internal GPIOs 33–37). Selecting `Quad` PSRAM in Arduino or ESP-IDF will cause an immediate boot loop / panic.
2. **CO5300 Display Has a 6-Pixel X-Offset**: The 466×466 panel is mapped inside a 480-wide controller GRAM window. You **must** configure `x_gap = 6` (or `col_offset1 = 6`) so pixels aren't shifted 6px to the left with garbage on the right edge.
3. **CO5300 Requires Even Pixel Alignment (`2px`)**: The CO5300 QSPI window commands (`0x2A`, `0x2B`) require `x1`, `y1` to be **even** and `width`, `height` to be **even** (`x2`, `y2` odd). Always register an LVGL `rounder_cb` (see [display.md](display.md)).
4. **No Hardware Backlight PWM Pin**: OLED pixels are self-emissive. Brightness is controlled exclusively by sending MIPI DCS command `0x51` (`Write Display Brightness`, `0x00`–`0xFF`) over QSPI to the CO5300.
5. **Never Blindly Disable AXP2101 Rails**: Disabling `ALDO1`–`ALDO4` or `DLDO1` without checking rail assignments will cut power to the display, touch controller, or I²C pull-ups, hanging the I²C bus.
6. **Protect Shared I²C (`GPIO 14/15`) in Multi-Tasking**: Touch polling (LVGL task), IMU reads, PMIC telemetry, and audio codec setup all share `I2C_NUM_0`. Always use thread-safe driver calls or a mutex around I²C transactions.
7. **Native USB Serial (`USB_SERIAL_JTAG`)**: The USB-C port connects directly to the ESP32-S3 internal PHY (`GPIO 19/20`), not an external UART bridge. Ensure `ARDUINO_USB_CDC_ON_BOOT=1` (Arduino) or `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (ESP-IDF) is set, or serial output will go only to `UART0` (`GPIO 43/44`).

---

## 5. Quick Flashing & Monitoring (macOS)

### Find the Serial Port
```bash
ls /dev/cu.usbmodem*
```

### Recovery / Bootloader Mode
If firmware crashes in a tight loop or disables USB:
1. Hold down the **BOOT** button (`GPIO 0`).
2. Tap and release the **PWR** button (or plug in USB-C while holding **BOOT**).
3. Release **BOOT** — the board will enumerate as a stable `/dev/cu.usbmodem*` ROM download port.

### Continuous Learning
Whenever we solve a non-trivial bug, discover a timing nuance, or establish a project pattern on this board, document it immediately in [lessons.md](lessons.md) and update the relevant subsystem file.
