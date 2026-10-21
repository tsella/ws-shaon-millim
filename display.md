# Display & Graphics Guide (`display.md`)

Everything you need to know about driving the **1.75-inch Round AMOLED (466 × 466)** with the **CO5300** QSPI controller and **LVGL**.

---

## 1. Hardware Interface (CO5300 QSPI)

* **Controller**: Chipone **CO5300** (MIPI-DSI / QSPI AMOLED Driver)
* **Active Resolution**: **466 × 466** pixels (Circular active area)
* **Color Depth**: 16-bit RGB565 (65K colors, commonly used for LVGL speed) or 24-bit RGB888 (16.7M colors)
* **QSPI Pins**:
  * `CS`: **GPIO 12**
  * `SCK`: **GPIO 38** (Supports up to **80 MHz** PCLK)
  * `D0`–`D3`: **GPIO 4, GPIO 5, GPIO 6, GPIO 7**
  * `RST`: **GPIO 39** (Active Low; pulse Low for $\ge 10\text{ ms}$, then wait $\ge 120\text{ ms}$ after `SLPOUT`)

---

## 2. The Three Critical CO5300 Gotchas

### Gotcha #1: The 6-Pixel Horizontal Offset (`x_gap = 6`)
The CO5300 internal GRAM is 480 columns wide, and this 466-pixel glass panel is wired starting at column index `6` (`466 + 6 + 8 = 480`).
* **Symptom**: Image is shifted 6 pixels to the left, and a 6-pixel crescent of random static/garbage appears on the right edge.
* **Fix**:
  * **ESP-IDF**: Call `esp_lcd_panel_set_gap(panel_handle, 6, 0);` immediately after `esp_lcd_new_panel_co5300(...)`.
  * **Arduino_GFX**: Pass `col_offset1 = 6` to the `Arduino_CO5300` constructor.

### Gotcha #2: Even-Pixel Window Alignment (`rounder_cb`)
The CO5300 Column Address Set (`0x2A`) and Page Address Set (`0x2B`) commands require both the **start coordinate (`x1`, `y1`)** and the **window dimensions (`width`, `height`)** to be multiples of **2** (meaning `x1`, `y1` must be **even** and `x2`, `y2` must be **odd**).
* **Symptom**: Sliced/corrupted text, horizontal tearing on partial screen updates, or missing pixels when buttons animate.
* **Fix**: Always attach a `rounder_cb` in LVGL:

#### LVGL v8 `rounder_cb`
```c
void my_lvgl_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area) {
    area->x1 = area->x1 & ~1; // Round down to even
    area->y1 = area->y1 & ~1; // Round down to even
    area->x2 = area->x2 | 1;  // Round up to odd (keeps width = x2 - x1 + 1 even)
    area->y2 = area->y2 | 1;  // Round up to odd (keeps height = y2 - y1 + 1 even)
}
```

#### LVGL v9 `LV_EVENT_INVALIDATE_AREA`
```c
void my_lvgl_rounder_event_cb(lv_event_t *e) {
    lv_area_t *area = lv_event_get_invalidated_area(e);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}
// Register via: lv_display_add_event_cb(disp, my_lvgl_rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
```

### Gotcha #3: RGB565 Byte Order (Big-Endian SPI)
QSPI transmits MSB first, so standard little-endian ESP32-S3 `uint16_t` RGB565 pixels will have their high and low bytes swapped (blue and green/red inverted).
* **Fix**: Enable `LV_COLOR_16_SWAP 1` in `lv_conf.h` / `sdkconfig` (LVGL v8), or call `lv_draw_sw_rgb565_swap(px_map, pixel_count)` inside your flush callback before `esp_lcd_panel_draw_bitmap()`.

---

## 3. Brightness & Display Sleep Control

There is **no backlight GPIO pin**. AMOLED pixels emit their own light, controlled via MIPI DCS commands over QSPI:

| Command Hex | Name | Payload | Description |
| :--- | :--- | :--- | :--- |
| `0x53` | `WRCTRLD` | `0x20` | Enable brightness control block (sent during init) |
| `0x51` | `WRDISBV` | `0x00`–`0xFF` | Set display brightness (`0x00` = completely off, `0xFF` = 100% max nits) |
| `0x28` | `DISPOFF` | None | Blank display output |
| `0x29` | `DISPON` | None | Unblank display output |
| `0x10` | `SLPIN` | None | Enter minimum-power panel sleep mode (wait 120ms after) |
| `0x11` | `SLPOUT` | None | Exit panel sleep mode (wait 120ms after) |

> [!TIP]
> **Espressif BSP Helper**: When using `waveshare/esp32_s3_touch_amoled_1_75`, simply call `bsp_display_brightness_set(percent)` (0–100) to adjust the CO5300 `0x51` register cleanly.

---

## 4. Designing UI for a 466×466 Round AMOLED

1. **Circular Safe Zone**:
   * Center of screen: `(233, 233)`, Radius: `233 px`.
   * The inscribed square inside the circle spans from **`(34, 34)` to `(432, 432)`** (`398 × 398 px`). Never place corner text or rectangular buttons outside this radius or they will be clipped by the round bezel.
   * Keep a `12px–16px` radial padding inside the outer rim for arc gauges (`lv_arc`) and circular progress rings.
2. **True Black (`#000000`) Saves Massive Power & Looks Invisible**:
   * On an AMOLED panel, `#000000` pixels turn completely off (zero current draw, infinite contrast).
   * Always design with a **pure `#000000` background** so the UI blends seamlessly into the black glass bezel with zero "glowing edge".
3. **Burn-In Prevention**:
   * Avoid leaving static 100%-brightness white (`#FFFFFF`) status icons on-screen 24/7. Dim the display (`0x51` $\le$ `0x30`) after 15–30 seconds of inactivity, and turn off (`0x28` / `0x10`) after timeout.
4. **Waveshare BSP `v1.0.2` `bsp_display_start()` QSPI DMA Bug**:
   * Do **not** call `bsp_display_start()` in `waveshare/esp32_s3_touch_amoled_1_75` (`v1.0.2`): its internal `bsp_display_lcd_init()` mistakenly calls `lvgl_port_add_disp_rgb()` with `buff_dma = false`, which skips registering the SPI `on_color_trans_done` callback and causes horizontal striped DMA corruption (`spi transmit (queue) color failed`).
   * Instead, call `bsp_display_new()` and register the panel via **`lvgl_port_add_disp(&disp_cfg)`** with `disp_cfg.flags.buff_dma = true` and `co5300_rounder_cb` (see [main/hal_board.cpp](main/hal_board.cpp) and [lessons.md](lessons.md)).
