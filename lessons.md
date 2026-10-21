# Living Engineering Log (`lessons.md`)

A chronological log of hardware discoveries, verified configurations, benchmark findings, and lessons learned while building on the **Waveshare ESP32-S3-Touch-AMOLED-1.75C**.

> **Rule for Agents & Developers**: Whenever we hit a hardware quirk, solve a tricky bug, verify exact flash/revision behavior on our physical unit, or find a performance win, add a concise entry below and cross-reference it in the relevant `.md` file.

---

## Baseline Platform Checklist (For First Hardware Connection)

When we plug in the physical board for the first time, let's verify and record:
- [x] **Serial Port Enumeration**: Confirmed `/dev/cu.usbmodem101` (`USB-Serial/JTAG` mode) on macOS.
- [x] **Exact Flash Capacity**: Confirmed **32 MB Quad NOR Flash** (`Manufacturer: c8`, `Device: 4019`, 3.3V) and **8 MB Embedded Octal PSRAM (`AP_3v3`)** on ESP32-S3 rev v0.2.
- [ ] **I²C Bus Scan (`GPIO 15` SDA / `GPIO 14` SCL)**: Confirm responses from `0x18` (ES8311), `0x20` (TCA9554), `0x34` (AXP2101), `0x40` (ES7210), `0x51` (PCF85063), `0x5A` (CST9217), and `0x6B` (QMI8658).
- [ ] **Reset Line Verification**: Confirm `GPIO 39` (`LCD_RST`) and `GPIO 40` (`TP_RST`) vs `TCA9554` `EXIO0`/`EXIO2` behavior on our exact PCB revision.

---

## Lessons Log

### 2026-10-20 — First Hardware Handshake & Toolchain Verification
* **Hardware Verified via `esptool flash_id`**:
  * **Port**: `/dev/cu.usbmodem101`
  * **SoC**: `ESP32-S3 (QFN56) (revision v0.2)`, `Crystal: 40MHz`
  * **PSRAM**: `Embedded PSRAM 8MB (AP_3v3)` (Octal)
  * **Flash**: **32 MB** (`Manufacturer: c8`, `Device: 4019`, Quad SPI @ 3.3V) — updated `sdkconfig.defaults` in [espressif.md](espressif.md) to `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y`.
* **Physical Button Orientation**:
  * Holding the board screen-facing-you with USB-C at 6 o'clock and buttons on the right rim: **Top button (2–3 o'clock) = `PWR`**, **Bottom button (4 o'clock) = `BOOT` (`GPIO 0`)**.
* **Toolchain Paths**:
  * ESP-IDF v5.3 installed at `~/esp/esp-idf` with Python venv at `~/.espressif/python_env/idf5.3_py3.11_env`.

### 2026-10-20 — Critical Hardware & BSP Discoveries During `ws-shaon-millim` Bring-Up
* **1. Waveshare BSP `v1.0.2` QSPI Display DMA Corruption Bug (`bsp_display_start`)**:
  * **Symptom**: Calling `bsp_display_start()` from `waveshare/esp32_s3_touch_amoled_1_75` (`v1.0.2`) produces alternating horizontal pink/corrupted bands across the 466×466 screen and logs `E (...) lcd_panel.io.spi: panel_io_spi_tx_color(390): spi transmit (queue) color failed`.
  * **Root Cause**: Inside `esp32_s3_touch_amoled_1_75.c`, `bsp_display_lcd_init()` mistakenly calls `lvgl_port_add_disp_rgb()` (intended for parallel RGB panels) instead of `lvgl_port_add_disp()`, and sets `buff_dma = false, buff_spiram = false`. Because `disp_type` is set to `LVGL_PORT_DISP_TYPE_RGB`, `esp_lvgl_port` never registers the SPI `on_color_trans_done` callback and calls `lv_disp_flush_ready()` synchronously before QSPI DMA completes. LVGL immediately overwrites the draw buffer with the next strip mid-transfer and overflows the SPI DMA bounce buffer pool.
  * **Verified Fix** ([main/hal_board.cpp](main/hal_board.cpp)): Bypass `bsp_display_start()`. Call `bsp_display_new()` to initialize the CO5300 QSPI panel (`x_gap = 6`), then register the display with **`lvgl_port_add_disp(&disp_cfg)`** using **`disp_cfg.flags.buff_dma = true`** (internal DMA-capable SRAM buffer, `466 * 40` pixels) and attach `co5300_rounder_cb`.
* **2. Header Include Order for `bsp/touch.h` & `esp_lvgl_port.h`**:
  * **Symptom**: Compile error `'esp_lcd_touch_handle_t' has not been declared` and `'lvgl_port_add_touch' was not declared in this scope`.
  * **Root Cause**: `bsp/touch.h` references `esp_lcd_touch_handle_t` without including `esp_lcd_touch.h`, and `esp_lvgl_port_touch.h` guards `lvgl_port_add_touch()` behind `#ifdef ESP_LCD_TOUCH_H`.
  * **Verified Fix**: Always `#include "esp_lcd_touch.h"` **before** `#include "esp_lvgl_port.h"` and `#include "bsp/touch.h"`.
* **3. AXP2101 Peripheral LDO Rail Map & Safe I²C Probing**:
  * **Schematic Rail Voltages** (`UP1 AXP2101` in `ESP32-S3-Touch-AMOLED-1.75-schematic.pdf`):
    * `ALDO1` (`0x92`) = **`3.3V` (`A3V3`)** — powers `ES8311` `AVDD` and `ES7210` analog supply.
    * `ALDO2` (`0x93`) = **`3.3V`** (`VL2_3.3V`).
    * `ALDO3` (`0x94`) = **`3.0V`** (`VL3_3.0V`).
    * `ALDO4` (`0x95`) = **`1.8V`** (`VL3_1.8V`) — **do not overwrite to 3.3V!**
    * `BLDO1` (`0x96`) = **`1.2V`** (`VL_1.2V`) — **do not overwrite to 3.3V!**
    * `BLDO2` (`0x97`) = **`2.8V`** (`VL_2.8V`).
  * **Verified Fix** ([main/hal_board.cpp](main/hal_board.cpp)): Initialize `AXP2101` (`0x34`) **first** while holding `bsp_display_lock()`, ensure `ALDO1` (`0x92`) and `ALDO2` (`0x93`) are set to `3300 mV` (`0x1C`) and enabled in `Reg 0x90` (`|= 0x0F`), and probe the I²C bus (`[ 0x18 0x34 0x40 0x5A 0x6B ]` on `-1.75C`) before conditionally attaching `PCF85063` (`0x51`) and `TCA9554` (`0x20`). Poll `AXP2101` `Reg 0x49` (`PKEY_SHORT_IRQ`) as a fallback for the physical `PWR` button on revisions without `TCA9554`.
* **4. `-1.75C` vs `-1.75` PCB Pinout Differences (`I2S_MCLK` = `GPIO 16` on `-1.75C` vs `GPIO 42` on `-1.75`!)**:
  * **Symptom**: On the **ESP32-S3-Touch-AMOLED-1.75C** (cased variant), `ES8311` (`0x18`) initializes cleanly over I²C and `NS4150B` (`GPIO 46`) un-mutes (`click`), but speaker playback produces 100% silence when using the standard `waveshare/esp32_s3_touch_amoled_1_75` BSP defaults.
  * **Root Cause (Confirmed via `facebookincubator/muse-gadget-sdk` & `waveshare/esp32_s3_touch_amoled_1_75c` v3.0.0 BSP)**:
    * The cased **`-1.75C`** and bare **`-1.75`** boards have different GPIO assignments! Specifically:
      | Signal | **`-1.75C`** (`esp32_s3_touch_amoled_1_75c`) | **`-1.75`** (`esp32_s3_touch_amoled_1_75`) |
      | :--- | :--- | :--- |
      | **`I2S_MCLK`** | **`GPIO 16`** | **`GPIO 42`** |
      | **`LCD_RST`** | **`GPIO 1`** | **`GPIO 39`** |
      | **`TP_RST`** | **`GPIO 2`** | **`GPIO 40`** |
      | **`PWR` Button** | **`GPIO 3`** (Active High via BSS138 inverter) | **`TCA9554 EXIO4`** (`0x20`) / `AXP2101` IRQ |
      | **`TCA9554` (`0x20`) & `PCF85063` (`0x51`)** | Not populated | Populated |
    * Because `ES8311` is configured with `.use_mclk = true` (external `MCLK` pin), outputting `I2S_MCLK` on `GPIO 42` instead of `GPIO 16` leaves the `ES8311` delta-sigma DAC modulator with **zero master clock**, resulting in total silence.
  * **Verified Fix** ([main/hal_board.cpp](main/hal_board.cpp)):
    1. Probe `0x20` (`TCA9554`) at boot to distinguish `-1.75` (`s_io_exp != NULL`) from `-1.75C` (`s_io_exp == NULL`).
    2. Before calling `bsp_audio_codec_speaker_init()`, call `bsp_audio_init(&i2s_cfg)` with `.gpio_cfg.mclk = (s_io_exp != NULL) ? GPIO_NUM_42 : GPIO_NUM_16` and `16,000 Hz` 16-bit Stereo (`I2S_SLOT_MODE_STEREO`), then open both `s_spk_dev` (`ES8311`) and `mic_dev` (`ES7210`) at `16,000 Hz`, `channel = 2`, `bits_per_sample = 16`.
    3. On `-1.75C`, read the physical `PWR` button directly from `GPIO 3` (`gpio_get_level(GPIO_NUM_3) == 1`).
* **7. Wi-Fi `reason=15` (`WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT`) on WPA2/WPA3-Transitional Mesh Routers**:
  * **Symptom**: `W (...) net_time: Wi-Fi disconnected from '...' (reason=15)` (`state: run -> init (0xf00)`) when connecting to a WPA2/WPA3-Personal Transitional (`kCWSecurityWPA3Transition`) 2.4 GHz AP.
  * **Root Causes (Proven via `libnet80211.a` Disassembly)**:
    1. **`ieee80211_parse_rsn` Selects WPA3-SAE + PMF**: When an AP advertises both `AKM 2` (`WPA2-PSK`) and `AKM 8` (`WPA3-SAE`) with `MFPC=1`, `ieee80211_parse_rsn` checks `g_wifi_feature_caps & CONFIG_FEATURE_WPA3_SAE_BIT` and selects WPA3-SAE + PMF (802.11w).
    2. **`wifi_set_config_process` Ignores `pmf_cfg.capable = false`**: `esp_wifi_set_config()` unconditionally ORs `1` into `pmf_cfg.capable` (`offset 146`), so setting `wifi_config.sta.pmf_cfg.capable = false` is a no-op! Even with WPA3-SAE disabled, `pmf_cfg.capable == 1` still negotiates PMF/IGTK (`BIP-CMAC-128`), causing the router's 4-way EAPOL handshake to time out (`0xf00` / `reason=15`).
    3. **Near-Field PA Saturation & Reconnect Storms**: At close range (`-38 dBm`), 20 dBm TX power saturates the receiver front-end during EAPOL bursts, and 0 ms reconnect retries hit the AP before the prior session is torn down.
  * **Verified Fix** ([main/net_time.cpp](main/net_time.cpp)):
    1. Clear `cfg.feature_caps &= ~CONFIG_FEATURE_WPA3_SAE_BIT` before calling `esp_wifi_init(&cfg)`.
    2. Call **`esp_wifi_disable_pmf_config(WIFI_IF_STA)`** **after** `esp_wifi_set_config(WIFI_IF_STA, &wifi_config)`. Together, these two calls force `ieee80211_parse_rsn` to negotiate pure standard `WPA2-PSK` (`key_mgmt = 5, authmode = 3`, no PMF/IGTK, no RSNXE).
    3. Set `esp_wifi_set_max_tx_power(52)` (`13 dBm`), `esp_wifi_set_ps(WIFI_PS_NONE)`, `esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20)`, `esp_wifi_set_storage(WIFI_STORAGE_RAM)`, and a 1500 ms `esp_timer` reconnect backoff.
* **5. Live Hardware Shadow VRAM & USB-Serial Screen Capture**:
  * Built a non-intrusive `flush_cb` wrapper in [main/hal_board.cpp](main/hal_board.cpp) that records every flushed pixel strip into a 466×466 PSRAM shadow buffer (`g_vram_shadow`), paired with [main/screencap.cpp](main/screencap.cpp) and [tools/screencap.py](tools/screencap.py) to capture live 466×466 PNG screenshots over `/dev/cu.usbmodem*` on demand.
* **6. HarfBuzz `ccmp=False, liga=False` for Accurate Two-Layer Hebrew Nikkud Coloring**:
  * **Symptom**: In `color` nikkud mode, the Patah under Alef (`אַ` in `וְאַרְבָּעִים`, `וְאַחַת`, `אַרְבַּע`), Holam on Vav (`וֹ`), Shuruk (`וּ`), Shin/Sin dots (`שׁ`/`שׂ`), and interior Dagesh (`בּ`, `תּ`, `לּ`) were colored in the consonant color (gold/blue) instead of the nikkud color (amber).
  * **Root Cause**: HarfBuzz enables OpenType `ccmp` (`Glyph Composition/Decomposition`) by default. In `FrankRuhlLibre-VF.ttf`, `ccmp` and `liga` substitute those base+mark pairs into single precomposed `U+FB2A`–`U+FB4B` presentation-form glyphs (`uniFB2E` `אַ`, `uniFB4B` `וֹ`, `uniFB2A` `שׁ`, etc.), merging the vowel mark into the consonant bitmap.
  * **Verified Fix** ([tools/generate_assets.py](tools/generate_assets.py)): Pass `{"kern": True, "mark": True, "mkmk": True, "ccmp": False, "liga": False}` to `hb.shape()`. This prevents `U+FBxx` precomposition while preserving full OpenType `GPOS` `mark` and `mkmk` anchor placement, keeping 100% of vowel/diacritic marks in `nikk_mask`. Also compensate for tight ink-box cropping (`max_x - min_x`) by adding `px_size / 5` to the inter-word space width in [main/clock_renderer.cpp](main/clock_renderer.cpp).

### 2026-10-20 — Initial Documentation & Architecture Setup
* **Context**: Created modular documentation suite ([AGENTS.md](AGENTS.md), [espressif.md](espressif.md), [arduino.md](arduino.md), [display.md](display.md), [controls.md](controls.md), [audio.md](audio.md), [power.md](power.md)).
* **Key Takeaways Captured**:
  * Display `CO5300` requires a **6-pixel horizontal offset (`x_gap = 6`)** and **even-coordinate/even-dimension alignment (`rounder_cb`)** for partial window updates.
  * Brightness is controlled purely via QSPI command `0x51` (no PWM backlight pin).
  * `ESP32-S3R8` requires **Octal PSRAM (`OPI`)** mode (`qio_opi` in PlatformIO / `CONFIG_SPIRAM_MODE_OCT=y` in ESP-IDF).
  * Physical `PWR` button state is easily polled in real time via **TCA9554 (`0x20`) Input Register bit 4 (`EXIO4`)**, where `1 = Pressed` and `0 = Released`.
