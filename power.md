# Power Management & Battery Guide (`power.md`)

Everything about the **AXP2101 Power Management IC (PMIC)**, battery charging, voltage rails, and sleep current optimization on the Waveshare ESP32-S3-Touch-AMOLED-1.75 / 1.75C.

---

## 1. AXP2101 PMIC Overview (`I²C 0x34`)

The **X-Powers AXP2101** sits on the primary I²C bus (`SDA = GPIO 15`, `SCL = GPIO 14`, address **`0x34`**) and manages:
* Seamless USB-C (`VBUS`) to 3.7V LiPo Battery (`VBAT`, MX1.25 2-pin connector) power-path switching.
* CC/CV lithium battery charging, thermal regulation, and hardware fuel-gauge percentage estimation (`getBatteryPercent()`, `getBattVoltage()`).
* Multiple step-down DC-DC bucks and Low-Dropout (LDO) voltage rails (`ALDO1`–`ALDO4`, `BLDO1`–`BLDO2`, `DLDO1`–`DLDO2`).
* Hardware power button (`PWRON`) conditioning and interrupt generation (`AXP_IRQ` $\rightarrow$ TCA9554 `EXIO5`).

---

## 2. Voltage Rails & Safety Rules

> [!CAUTION]
> **Golden Rule of AXP2101 Rails**:
> Never run code copied from another ESP32-S3 board (such as a LilyGo T-Watch or M5Stack CoreS3) that reconfigures AXP2101 DCDC or LDO voltages without verifying the Waveshare schematic! Over-volting an LDO rail above `3.3V` can permanently damage the CO5300 AMOLED panel or onboard sensors, and disabling `DCDC1` will instantly cut 3.3V power to the ESP32-S3 itself.

### Standard Rail Configuration (`3300 mV`)
On the Waveshare ESP32-S3-Touch-AMOLED-1.75 / 1.75C:
* **`DCDC1` (`3.3V`)**: Primary `3V3` system rail powering the ESP32-S3 and core logic. **Never disable `DCDC1`!**
* **`ALDO1` / `ALDO2` / `ALDO3` / `ALDO4` & `BLDO1` / `DLDO1` (`3.3V`)**: Supply power to onboard peripheral domains (AMOLED analog/IO, Touch, Audio codecs, IMU, SD card).
* In official Waveshare initialization routines (using `XPowersAXP2101`), peripheral rails are set to **`3300 mV`** (`3.3V`) and enabled during boot:

```cpp
#include "XPowersLib.h"

XPowersPMU pmu;

bool initPMIC() {
    if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, 15 /* SDA */, 14 /* SCL */)) {
        return false;
    }

    // Configure VBUS input limits
    pmu.setVbusVoltageLimit(XPOWERS_AXP2101_VBUS_VOL_LIM_4V36);
    pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_1500MA);

    // System cut-off voltage (2.6V - 3.0V safe LiPo floor)
    pmu.setSysPowerDownVoltage(3000);

    // Enable ADC telemetry channels
    pmu.enableBattDetection();
    pmu.enableVbusVoltageMeasure();
    pmu.enableBattVoltageMeasure();
    pmu.enableSystemVoltageMeasure();

    // Ensure peripheral 3.3V LDO rails are set to 3300mV and enabled
    pmu.setALDO1Voltage(3300); pmu.enableALDO1();
    pmu.setALDO2Voltage(3300); pmu.enableALDO2();
    pmu.setALDO3Voltage(3300); pmu.enableALDO3();
    pmu.setALDO4Voltage(3300); pmu.enableALDO4();
    pmu.setBLDO1Voltage(3300); pmu.enableBLDO1();
    pmu.setDLDO1Voltage(3300); pmu.enableDLDO1();

    // Charging parameters (safe for typical 300mAh - 500mAh badge LiPo)
    pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_300MA);
    pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);

    return true;
}
```

---

## 3. Battery Telemetry & Charging Status

Query battery state periodically (e.g., every 2–5 seconds, **not** every frame):

```cpp
bool isUSBPlugged = pmu.isVbusIn();
bool isCharging   = pmu.isCharging();
uint16_t vbat_mV  = pmu.getBattVoltage();      // e.g., 3850 mV
int battPercent   = pmu.getBatteryPercent();   // 0..100% (or -1 if no battery connected)
```

> [!TIP]
> **Charge Current Sizing**: Standard LiPo cells should be charged at $\le 0.5\text{C}\text{–}1\text{C}$. If using a compact `300 mAh` pouch cell inside the `-1.75C` aluminum case, keep `setChargerConstantCurr` at `200mA` or `300mA` (`XPOWERS_AXP2101_CHG_CUR_300MA`) to minimize heat inside the enclosure.

---

## 4. Power Consumption & Sleep Strategies

### Typical Current Draw Breakdown
| Subsystem State | Approx. Current | Notes |
| :--- | :--- | :--- |
| ESP32-S3 @ 240 MHz + Wi-Fi Active | `80 – 150 mA` | Enable Wi-Fi modem sleep (`WIFI_PS_MIN_MODEM`) to drop average to `~35 mA` |
| AMOLED Display (`466×466`) — Mostly Black UI | `15 – 30 mA` | Self-emissive: only lit pixels draw current! |
| AMOLED Display (`466×466`) — 100% Full White | `80 – 120 mA` | Avoid full-white backgrounds on battery |
| Audio Amp (`NS4150B` + Codecs) Idle / Active | `8 – 80 mA` | Pull `GPIO 46` (`PA_CTRL`) `LOW` when silent |
| Light Sleep (Display `SLPIN`, CPU paused, RAM retained) | `~1.5 – 3 mA` | Instant `< 20 ms` wake on touch (`GPIO 11`) or `BOOT` (`GPIO 0`) |
| PMIC Hard Power-Off (`pmu.shutdown()`) | `< 50 µA` | Wakes via `PWR` button press (`PWRON`) or USB-C insertion |

### Recommended Sleep Hierarchy for Wearable / Badge Projects
1. **Active Mode (`0 – 20s` idle)**: Full 60 FPS LVGL, brightness `180–255`.
2. **Dimmed Mode (`20 – 45s` idle)**: Drop CO5300 brightness (`0x51`) to `30`, reduce LVGL refresh rate or pause animations.
3. **Screen-Off Light Sleep (`> 45s` idle)**:
   * Send `0x28` (`DISPOFF`) and `0x10` (`SLPIN`) to CO5300.
   * Mute audio (`GPIO 46 = LOW`).
   * Configure wake-up on **Touch Interrupt (`GPIO 11` Low)** or **BOOT Button (`GPIO 0` Low)** using `esp_sleep_enable_gpio_wakeup()`, then enter `esp_light_sleep_start()`.
4. **Full Shutdown (Long-press `PWR` or Deep Idle)**:
   * Call `pmu.shutdown()` to turn off all AXP2101 rails cleanly. Press `PWR` to cold-boot.
