# Installation & Build Guide (`INSTALL.md`)

Step-by-step instructions to configure, build, flash, and debug **`ws-shaon-millim`** (`v0.1.0`) on the **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (and standard `-1.75` variant).

---

## 1. Prerequisites

### Hardware
* **Waveshare ESP32-S3-Touch-AMOLED-1.75C** (ESP32-S3R8 with **8 MB Octal PSRAM** and **32 MB Quad SPI Flash**).
* USB-C data cable connected to your computer.

### Software
1. **ESP-IDF v5.3.6** (minimum; v5.3.6 is the current bugfix release of the 5.3 branch):
   * Installed at `~/esp/esp-idf` (standard Espressif installation path).
   * If you haven't installed ESP-IDF yet:
     ```bash
     mkdir -p ~/esp && cd ~/esp
     git clone -b v5.3.6 --recursive https://github.com/espressif/esp-idf.git
     cd esp-idf && ./install.sh esp32s3
     ```
2. **Python 3.10+** (for the optional asset/configuration generator and live screen-capture tool).

---

## 2. Clone & Configure Default Settings (`config.json`)

1. Clone the repository and copy the example configuration file to `config.json`:
   ```bash
   git clone https://github.com/tsella/ws-shaon-millim.git
   cd ws-shaon-millim
   cp config.json.example config.json
   ```
   > **Security Note**: `config.json` and the generated `main/default_config_seed.h` are listed in `.gitignore` so your Wi-Fi credentials are never committed to Git.

2. Edit `config.json` with your Wi-Fi SSID, password, and preferred defaults:
   ```json
   {
     "wifi": {
       "ssid": "YOUR_WIFI_SSID",
       "password": "YOUR_WIFI_PASSWORD"
     },
     "timezone": "America/New_York",
     "colors": {
       "preset": "shabbat",
       "pure_black_oled": false
     },
     "render": {
       "sizing": "static",
       "align": "center",
       "nikkud": "color"
     },
     "speech": {
       "voice": "hila",
       "volume": 80,
       "hourly_enabled": true,
       "hourly_start": 7,
       "hourly_end": 21
     },
     "display": {
       "brightness": 85,
       "timeout_sec": 60,
       "always_on_usb": true
     }
   }
   ```

---

## 3. Set Up Python Virtual Environment & Generate Assets

Create a local Python virtual environment for the asset generator and screen-capture utility:

```bash
python3 -m venv .venv
.venv/bin/pip install uharfbuzz freetype-py pillow numpy edge-tts
```

### A. Generate `main/default_config_seed.h` & HarfBuzz Glyph Tables (Fast — 0.5s)
The pre-shaped Hebrew glyph tables (`main/hebrew_glyphs.h` and `main/hebrew_glyphs.c`) are already included in the repository. To regenerate them along with `main/default_config_seed.h` from your `config.json`:

```bash
.venv/bin/python tools/generate_assets.py --glyphs-only
```

### B. Generate the Offline Hebrew Neural Voice Pack (`build/voice_pack.bin`)
To synthesize all 45 Hebrew word tokens in both **Hila** (`he-IL-HilaNeural`) and **Avri** (`he-IL-AvriNeural`) voices and pack them into `build/voice_pack.bin` (~2.1 MB):

```bash
.venv/bin/python tools/generate_assets.py
```

---

## 4. Build & Flash Firmware

1. Activate the ESP-IDF environment:
   ```bash
   source ~/esp/esp-idf/export.sh
   ```

2. Find your board's serial port:
   * **macOS**: `ls /dev/cu.usbmodem*` (e.g., `/dev/cu.usbmodem101`)
   * **Linux**: `ls /dev/ttyACM*` (e.g., `/dev/ttyACM0`)

3. **First-Time Flash (Bootloader + Partition Table + Firmware + Voice Pack)**:
   Flash the firmware and partition table, then write `build/voice_pack.bin` to the `voice` partition at offset `0x400000`:
   ```bash
   idf.py -p /dev/cu.usbmodem101 flash
   esptool.py --chip esp32s3 -p /dev/cu.usbmodem101 -b 460800 write_flash 0x400000 build/voice_pack.bin
   ```

4. **Subsequent Code Updates (App-Only Flash in ~12 seconds)**:
   Once `build/voice_pack.bin` is flashed at `0x400000`, it persists across firmware updates. Use `app-flash` to update only the application binary (`0x20000`):
   ```bash
   idf.py -p /dev/cu.usbmodem101 app-flash
   ```

   > **Note on `managed_components/`**: ESP-IDF's component manager automatically downloads LVGL, `esp_lvgl_port`, `esp_codec_dev`, and the Waveshare BSP into `managed_components/` during the first build based on `main/idf_component.yml`.

---

## 5. Live Hardware Screen Capture & Remote Debugging

`ws-shaon-millim` includes a built-in USB-Serial/JTAG screen-capture server (`main/screencap.cpp`) that mirrors the exact hardware-flushed 466×466 GRAM buffer in PSRAM.

Capture a live 466×466 PNG screenshot from the device at any time:

```bash
# Capture the Clock Face screen:
.venv/bin/python tools/screencap.py -c SHOW_CLOCK -o clock.png

# Switch to the Settings screen and capture it:
.venv/bin/python tools/screencap.py -c SHOW_SETTINGS -o settings.png

# Trigger spoken Hebrew time announcement remotely:
.venv/bin/python tools/screencap.py -c SPEAK -o clock.png
```

---

## 6. Recovery Mode (If USB Serial Disconnects)

If the board does not appear under `/dev/cu.usbmodem*`:
1. Hold down the **bottom side button (`BOOT` / `GPIO 0`)**.
2. Tap and release the **top side button (`PWR`)** (or plug in the USB-C cable while holding `BOOT`).
3. Release **`BOOT`** — the ESP32-S3 will enter ROM download mode and enumerate as `/dev/cu.usbmodem*`.
