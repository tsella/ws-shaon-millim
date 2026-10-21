# Touch, IMU, RTC, Buttons & I/O Expander (`controls.md`)

Guide to user inputs, motion sensing, real-time clock, and the onboard I²C peripheral bus.

---

## 1. Shared I²C Bus Architecture

All control peripherals share a single I²C bus on the ESP32-S3:
* **SDA**: **GPIO 15**
* **SCL**: **GPIO 14**
* **Bus Speed**: **400 kHz** (Fast Mode)

> [!IMPORTANT]
> **Concurrency Rule**: Because Touch (`CST9217`), IMU (`QMI8658`), RTC (`PCF85063`), PMIC (`AXP2101`), Audio Codecs (`ES8311`/`ES7210`), and I/O Expander (`TCA9554`) all sit on `GPIO 14/15`, **never access `I2C_NUM_0` from multiple FreeRTOS tasks without synchronization**. In ESP-IDF, use the BSP's shared `i2c_master_bus_handle_t` (which handles bus-level locking in `driver/i2c_master.h`) or wrap LVGL calls in `bsp_display_lock(0)` / `bsp_display_unlock()`.

---

## 2. Capacitive Touch Controller — CST9217 (`0x5A`)

* **I²C Address**: `0x5A`
* **Interrupt (`TP_INT`)**: **GPIO 11** (Active Low / Falling Edge when touch data is ready)
* **Reset (`TP_RST`)**: **GPIO 40** *(Note: on some hardware revisions, `EXIO2` on TCA9554 is also tied or used; the official ESP-IDF BSP drives GPIO 40).*

### Best Practices for CST9217
* **Reset Sequence**: Pull `TP_RST` Low for `10 ms`, pull High, then wait at least **`50 ms`** before polling `0x5A` on I²C. Polling too early during boot causes I²C NACKs.
* **Interrupt-Driven Polling**: Instead of hammering the I²C bus on every 5ms LVGL tick when no finger is on the glass, check `gpio_get_level(GPIO_NUM_11) == 0` (or latch an ISR flag on falling edge of `GPIO 11`) before issuing the I²C read to `0x5A`. This reduces I²C bus traffic by >90% and frees bandwidth for the IMU and audio controls.
* **Coordinate Clamping**: The CST9217 reports `X` and `Y` in `0..465`. If rotating the display via software (`90°`, `180°`, `270°`), apply matching `swap_xy`, `mirror_x`, and `mirror_y` flags in the touch driver config.

---

## 3. TCA9554 8-Bit I/O Expander (`0x20`)

The TCA9554 sits at I²C address `0x20` and uses four standard registers:
* `0x00`: **Input Port Register** (Read live pin states)
* `0x01`: **Output Port Register** (Write output states)
* `0x02`: **Polarity Inversion Register** (Keep at `0x00`)
* `0x03`: **Configuration Register** (`1` = Input [default], `0` = Output)

### Pin Map & Direction Setup
| Bit | Name | Direction (`Reg 0x03`) | Function |
| :--- | :--- | :--- | :--- |
| Bit 0 (`0x01`) | `EXIO0` | Output (`0`) or Input | Aux IO / Display reset on some revisions |
| Bit 1 (`0x02`) | `EXIO1` | Input/Output | Test pad / User expansion |
| Bit 2 (`0x04`) | `EXIO2` | Output (`0`) or Input | Aux IO / Touch reset on some revisions |
| Bit 3 (`0x08`) | `RTC_INT` | Input (`1`) | PCF85063 RTC Interrupt (Active Low) |
| Bit 4 (`0x10`) | `PWR_BTN` | Input (`1`) | **Physical PWR Button State** (`1` = Pressed, `0` = Released) |
| Bit 5 (`0x20`) | `AXP_IRQ` | Input (`1`) | AXP2101 PMIC Interrupt (Active Low) |
| Bit 6 (`0x40`) | `QMI_INT1` | Input (`1`) | QMI8658 IMU Interrupt 1 |
| Bit 7 (`0x80`) | `GPS_RST` | Output (`0`) | GPS Reset (on `-G` model) |

> [!WARNING]
> **Never overwrite `Reg 0x03` with `0x00` (all outputs)!** Bits 3, 4, 5, and 6 (`0b01111000 = 0x78`) are driven by external interrupt/button circuits. Always keep bits 3–6 configured as **Inputs (`1`)** in Register `0x03` (e.g., `config_reg |= 0x78;`).

---

## 4. Physical Buttons (`BOOT` & `PWR`)

### `BOOT` Button (`GPIO 0`)
* Directly wired to **GPIO 0** with an external pull-up.
* **Active Low**: Reads `0` when pressed, `1` when released.
* Can be used as a standard user button (short press, double click, long press via `iot_button` or `OneButton`) once the board has booted, **and** as a wake-up source from Deep/Light sleep (`esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0)`).

### `PWR` Button (`EXIO4` + `AXP2101`)
The physical `PWR` button is connected to the AXP2101 `PWRON` pin and level-shifted to **TCA9554 `EXIO4`**:
* **Live State Reading (Recommended for UI/Games)**: Read bit 4 (`0x10`) of TCA9554 Input Register (`0x00`).
  * `(input_reg & (1 << 4)) != 0` $\rightarrow$ **Button is currently held down (Active High)**.
* **Event Reading via AXP2101**: The AXP2101 also latches `PKEY_SHORT_IRQ` (short press) and `PKEY_LONG_IRQ` (long press, default 1.5s) in its interrupt status registers.
* **Hardware Hard Reset / Power Off**: Holding `PWR` for **> 6 seconds** triggers a hardware power-off inside the AXP2101 PMIC. When powered off on battery, a 1-second press of `PWR` boots the board back up.

---

## 5. 6-Axis IMU — QMI8658 (`0x6B`)

* **I²C Address**: `0x6B`
* **Capabilities**: 3-axis Accelerometer ($\pm 2g / \pm 4g / \pm 8g / \pm 16g$) + 3-axis Gyroscope ($\pm 16^\circ/\text{s}$ to $\pm 2048^\circ/\text{s}$), hardware pedometer, wake-on-motion (WoM).
* **Interrupt**: `QMI_INT1` routed to TCA9554 `EXIO6`.
* **Libraries**:
  * **Arduino**: `SensorQMI8658` inside `lewisxhe/SensorLib`.
  * **ESP-IDF**: `waveshare/qmi8658` or included via the Waveshare BSP.
* **Tips**:
  * For wrist-raise wake or tilt-controlled UIs, `ODR` of `100 Hz`–`200 Hz` with a low-pass filter enabled gives responsive, jitter-free readings.
  * When entering sleep, put the QMI8658 into power-down or low-power Wake-on-Motion mode (`enableWakeOnMotion()`) so it draws $< 15\ \mu\text{A}$ instead of $\sim 650\ \mu\text{A}$.

---

## 6. Real-Time Clock — PCF85063 (`0x51`)

* **I²C Address**: `0x51`
* **Interrupt**: `RTC_INT` routed to TCA9554 `EXIO3`.
* **Library**: `SensorPCF85063` inside `lewisxhe/SensorLib` or `esp_rtc` / I²C register access.
* **Usage Pattern**:
  1. On boot, read time from `PCF85063` (`0x51`) and call `settimeofday()` to initialize the ESP32-S3 system clock immediately (before Wi-Fi even connects).
  2. Whenever NTP syncs over Wi-Fi (`sntp`), write the updated UTC/local time back to the `PCF85063`.
