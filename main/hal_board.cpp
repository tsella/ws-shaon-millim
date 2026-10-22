#include "hal_board.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"

static const char *TAG = "hal_board";

uint16_t *g_vram_shadow = NULL;

volatile int  g_batt_percent = 85;
volatile bool g_is_charging  = false;
volatile bool g_usb_plugged  = true;

static esp_lcd_panel_handle_t    s_panel_handle    = NULL;
static esp_lcd_panel_io_handle_t s_panel_io_handle = NULL;
static i2c_master_dev_handle_t   s_axp_dev         = NULL;
static i2c_master_dev_handle_t   s_rtc_dev         = NULL;
static esp_io_expander_handle_t  s_io_exp          = NULL;

static const uint8_t            *s_voice_mmap_ptr    = NULL;
static esp_partition_mmap_handle_t s_voice_mmap_handle = 0;
static esp_codec_dev_handle_t    s_spk_dev           = NULL;
static SemaphoreHandle_t         s_audio_mutex       = NULL;

static bool s_prev_boot_pressed = false;
static bool s_prev_pwr_pressed  = false;

// ============================================================================
// 1. Display & Capacitive Touch (Fixes Waveshare BSP 1.0.2 QSPI DMA Bug)
// ============================================================================
static void (*s_orig_flush_cb)(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) = NULL;

static void hal_wrapping_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    if (g_vram_shadow && area && color_map) {
        int w = area->x2 - area->x1 + 1;
        const uint16_t *src = (const uint16_t *)color_map;
        for (int y = area->y1; y <= area->y2; y++) {
            if (y >= 0 && y < 466) {
                for (int x = area->x1; x <= area->x2; x++) {
                    if (x >= 0 && x < 466) {
                        g_vram_shadow[y * 466 + x] = src[(y - area->y1) * w + (x - area->x1)];
                    }
                }
            }
        }
    }
    if (s_orig_flush_cb) {
        s_orig_flush_cb(drv, area, color_map);
    }
}

static void co5300_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area) {
    // CO5300 requires even start coordinates (2M) and odd end coordinates (2N+1)
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

void hal_display_set_brightness(int percent) {
    bsp_display_brightness_set(percent);
}

lv_disp_t *hal_display_and_touch_init(int initial_brightness_percent) {
    if (!g_vram_shadow) {
        g_vram_shadow = (uint16_t *)heap_caps_calloc(466 * 466, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }

    // Configure physical BOOT button (GPIO 0, Active Low)
    gpio_config_t boot_btn_cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_btn_cfg);

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // Initialize CO5300 QSPI Panel via BSP (sets x_gap=6 and runs vendor init sequence)
    bsp_display_config_t bsp_disp_cfg = {};
    ESP_ERROR_CHECK(bsp_display_new(&bsp_disp_cfg, &s_panel_handle, &s_panel_io_handle));

    // Register with esp_lvgl_port using lvgl_port_add_disp() (which registers on_color_trans_done!)
    // and an internal DMA-capable SRAM buffer so QSPI DMA never corrupts strips or exhausts bounce RAM.
    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = s_panel_io_handle;
    disp_cfg.panel_handle = s_panel_handle;
    disp_cfg.buffer_size = 466 * 40;
    disp_cfg.double_buffer = false;
    disp_cfg.hres = 466;
    disp_cfg.vres = 466;
    disp_cfg.monochrome = false;
    disp_cfg.rotation.swap_xy = false;
    disp_cfg.rotation.mirror_x = false;
    disp_cfg.rotation.mirror_y = false;
    disp_cfg.flags.buff_dma = true;
    disp_cfg.flags.buff_spiram = false;
    disp_cfg.flags.sw_rotate = false;

    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    assert(disp != NULL);
    if (disp->driver) {
        disp->driver->rounder_cb = co5300_rounder_cb;
        s_orig_flush_cb = disp->driver->flush_cb;
        disp->driver->flush_cb = hal_wrapping_flush_cb;
    }

    // Initialize CST9217 Capacitive Touch Controller on shared I2C
    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) == ESP_OK && tp != NULL) {
        lvgl_port_touch_cfg_t touch_cfg = {};
        touch_cfg.disp = disp;
        touch_cfg.handle = tp;
        lvgl_port_add_touch(&touch_cfg);
    }

    hal_display_set_brightness(initial_brightness_percent);
    return disp;
}

// ============================================================================
// 2. I2C Helpers, AXP2101 PMIC, PCF85063 RTC, & Buttons
// ============================================================================
static esp_err_t i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
    if (!dev) return ESP_FAIL;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, 2, 50);
}

static esp_err_t i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t len) {
    if (!dev) return ESP_FAIL;
    return i2c_master_transmit_receive(dev, &reg, 1, out, len, 50);
}

void hal_pmic_and_rtc_init(void) {
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return;

    if (!bsp_display_lock(1000)) return;

    // 1. Attach AXP2101 PMIC (0x34) FIRST and ensure ALDO1 (A3V3 audio analog rail) and ALDO2 are 3.3V & enabled
    // Per schematic: ALDO1=3.3V (A3V3 -> ES8311 AVDD), ALDO2=3.3V, ALDO3=3.0V, ALDO4=1.8V, BLDO1=1.2V, BLDO2=2.8V
    i2c_device_config_t axp_cfg = {};
    axp_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    axp_cfg.device_address = 0x34;
    axp_cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &axp_cfg, &s_axp_dev) == ESP_OK) {
        uint8_t ldo_en = 0;
        i2c_read_regs(s_axp_dev, 0x90, &ldo_en, 1);
        ESP_LOGI(TAG, "AXP2101 initial LDO_ONOFF_CONTROL0 (0x90) = 0x%02X", ldo_en);

        // Ensure ALDO1 (0x92, A3V3 for ES8311 AVDD) and ALDO2 (0x93) are 3.3V (0x1C), without altering 1.8V/1.2V rails
        i2c_write_reg(s_axp_dev, 0x92, 0x1C); // ALDO1 = 3.3V (A3V3)
        i2c_write_reg(s_axp_dev, 0x93, 0x1C); // ALDO2 = 3.3V
        // Ensure ALDO1..4 stay enabled
        i2c_write_reg(s_axp_dev, 0x90, ldo_en | 0x0F);

        i2c_write_reg(s_axp_dev, 0x30, 0x3F); // Enable ADC channels
        i2c_write_reg(s_axp_dev, 0x68, 0x01); // Enable battery detection / fuel gauge
        i2c_write_reg(s_axp_dev, 0x41, 0x0F); // Enable PWRON key short/long IRQ
        i2c_write_reg(s_axp_dev, 0x48, 0xFF); // Clear pending IRQ0
        i2c_write_reg(s_axp_dev, 0x49, 0xFF); // Clear pending IRQ1
        i2c_write_reg(s_axp_dev, 0x4A, 0xFF); // Clear pending IRQ2
        vTaskDelay(pdMS_TO_TICKS(30));
        ESP_LOGI(TAG, "AXP2101 PMIC initialized at 0x34 (A3V3 ALDO1 enabled)");
    }

    char found_str[256] = "";
    bool has_tca9554 = false;
    bool has_rtc     = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "0x%02X ", addr);
            strncat(found_str, tmp, sizeof(found_str) - strlen(found_str) - 1);
            if (addr == 0x20) has_tca9554 = true;
            if (addr == 0x51) has_rtc = true;
        }
    }
    ESP_LOGI(TAG, "I2C Bus Scan (SDA=15, SCL=14): [ %s]", found_str);

    // 2. Attach PCF85063 RTC (0x51) if present
    if (has_rtc) {
        i2c_device_config_t rtc_cfg = {};
        rtc_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        rtc_cfg.device_address = 0x51;
        rtc_cfg.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(bus, &rtc_cfg, &s_rtc_dev) == ESP_OK) {
            uint8_t regs[7] = {0};
            if (i2c_read_regs(s_rtc_dev, 0x04, regs, 7) == ESP_OK) {
                auto bcd2dec = [](uint8_t v) -> int { return ((v >> 4) * 10) + (v & 0x0F); };
                int sec  = bcd2dec(regs[0] & 0x7F);
                int min  = bcd2dec(regs[1] & 0x7F);
                int hour = bcd2dec(regs[2] & 0x3F);
                int mday = bcd2dec(regs[3] & 0x3F);
                int mon  = bcd2dec(regs[5] & 0x1F) - 1;
                int year = bcd2dec(regs[6]) + 100;
                if (year >= 125 && mon >= 0 && mon < 12 && mday >= 1 && mday <= 31) {
                    struct tm tm_rtc = {};
                    tm_rtc.tm_sec = sec;
                    tm_rtc.tm_min = min;
                    tm_rtc.tm_hour = hour;
                    tm_rtc.tm_mday = mday;
                    tm_rtc.tm_mon = mon;
                    tm_rtc.tm_year = year;
                    char old_tz[64] = "";
                    const char *cur_tz = getenv("TZ");
                    if (cur_tz) strncpy(old_tz, cur_tz, sizeof(old_tz) - 1);
                    setenv("TZ", "UTC0", 1);
                    tzset();
                    time_t t = mktime(&tm_rtc);
                    if (old_tz[0]) setenv("TZ", old_tz, 1);
                    tzset();
                    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                    settimeofday(&tv, NULL);
                    ESP_LOGI(TAG, "Restored UTC time from PCF85063 RTC: %04d-%02d-%02d %02d:%02d:%02d",
                             year + 1900, mon + 1, mday, hour, min, sec);
                }
            }
        }
    }

    // 3. Attach TCA9554 IO Expander (0x20) if present on this revision (-1.75 bare board)
    if (has_tca9554) {
        s_io_exp = bsp_io_expander_init();
        if (s_io_exp) {
            esp_io_expander_set_dir(s_io_exp, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
        }
    } else {
        // On -1.75C (cased variant without TCA9554), PWR button is routed through a BSS138 inverter to GPIO 3 (High = Pressed)
        gpio_config_t pwr_btn_cfg = {
            .pin_bit_mask = (1ULL << GPIO_NUM_3),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&pwr_btn_cfg);
        s_prev_pwr_pressed = (gpio_get_level(GPIO_NUM_3) == 1);
    }

    bsp_display_unlock();
}

void hal_rtc_save_utc(time_t now_utc) {
    if (!s_rtc_dev) return;
    struct tm tm_utc;
    gmtime_r(&now_utc, &tm_utc);
    auto dec2bcd = [](int v) -> uint8_t { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
    uint8_t buf[8] = {
        0x04,
        dec2bcd(tm_utc.tm_sec),
        dec2bcd(tm_utc.tm_min),
        dec2bcd(tm_utc.tm_hour),
        dec2bcd(tm_utc.tm_mday),
        dec2bcd(tm_utc.tm_wday),
        dec2bcd(tm_utc.tm_mon + 1),
        dec2bcd((tm_utc.tm_year + 1900) % 100),
    };
    if (bsp_display_lock(50)) {
        i2c_master_transmit(s_rtc_dev, buf, sizeof(buf), 50);
        bsp_display_unlock();
    }
}

void hal_poll_pmic_telemetry(void) {
    if (!s_axp_dev) return;
    uint8_t st0 = 0, st1 = 0, soc = 0;
    if (bsp_display_lock(50)) {
        if (i2c_read_regs(s_axp_dev, 0x00, &st0, 1) == ESP_OK &&
            i2c_read_regs(s_axp_dev, 0x01, &st1, 1) == ESP_OK &&
            i2c_read_regs(s_axp_dev, 0xA4, &soc, 1) == ESP_OK) {
            g_usb_plugged = ((st0 & 0x20) != 0) || ((st0 & 0x01) != 0);
            uint8_t chg_dir = (st1 >> 5) & 0x03;
            g_is_charging = (chg_dir == 1);
            if (soc <= 100 && (st0 & 0x08)) {
                g_batt_percent = (int)soc;
            } else {
                uint8_t vbuf[2] = {0};
                if (i2c_read_regs(s_axp_dev, 0x34, vbuf, 2) == ESP_OK) {
                    int mv = ((vbuf[0] & 0x3F) << 8) | vbuf[1];
                    if (mv >= 3200 && mv <= 4300) {
                        int est = (mv - 3300) * 100 / (4150 - 3300);
                        if (est < 0) est = 0;
                        if (est > 100) est = 100;
                        g_batt_percent = est;
                    } else if (g_usb_plugged) {
                        g_batt_percent = 100;
                    }
                }
            }
        }
        bsp_display_unlock();
    }
}

bool hal_poll_boot_button_pressed(void) {
    bool pressed = (gpio_get_level(GPIO_NUM_0) == 0);
    bool edge = (pressed && !s_prev_boot_pressed);
    s_prev_boot_pressed = pressed;
    return edge;
}

bool hal_poll_pwr_button_short_press(void) {
    bool triggered = false;
    if (s_io_exp) {
        uint32_t in_mask = 0;
        if (bsp_display_lock(30)) {
            if (esp_io_expander_get_level(s_io_exp, IO_EXPANDER_PIN_NUM_4, &in_mask) == ESP_OK) {
                bool pwr_pressed = (in_mask != 0);
                if (pwr_pressed && !s_prev_pwr_pressed) {
                    triggered = true;
                }
                s_prev_pwr_pressed = pwr_pressed;
            }
            bsp_display_unlock();
        }
    } else {
        bool pwr_pressed = (gpio_get_level(GPIO_NUM_3) == 1);
        if (pwr_pressed && !s_prev_pwr_pressed) {
            triggered = true;
        }
        s_prev_pwr_pressed = pwr_pressed;
    }
    return triggered;
}

// ============================================================================
// 3. Audio Subsystem (ES8311 DAC + NS4150B PA + Voice Partition mmap)
// ============================================================================
typedef struct __attribute__((packed)) {
    uint32_t offset;
    uint32_t length;
} voice_entry_t;

// Pack layout: "WSMV" | num_voices (u16) | num_tokens (u16) | entries[num_voices][num_tokens]
static constexpr uint32_t VOICE_HEADER_SIZE = 8;

static uint16_t s_voice_num_voices = 0;
static uint16_t s_voice_num_tokens = 0;
static uint32_t s_voice_pack_size  = 0;

// Validate the header and every index entry against the real partition size.
// A corrupt or truncated pack fails closed: s_voice_mmap_ptr stays NULL and the
// play path refuses to run, instead of reading whatever follows the mapping.
static bool voice_pack_validate(const uint8_t *bytes, uint32_t size) {
    if (size < VOICE_HEADER_SIZE || memcmp(bytes, "WSMV", 4) != 0) {
        ESP_LOGW(TAG, "Voice partition has no 'WSMV' header (%02X %02X %02X %02X)",
                 bytes[0], bytes[1], bytes[2], bytes[3]);
        return false;
    }
    uint16_t n_voices = (uint16_t)(bytes[4] | (bytes[5] << 8));
    uint16_t n_tokens = (uint16_t)(bytes[6] | (bytes[7] << 8));
    if (n_voices == 0 || n_tokens == 0) {
        ESP_LOGW(TAG, "Voice pack index is empty (%u voices, %u words)", n_voices, n_tokens);
        return false;
    }
    uint64_t index_bytes = (uint64_t)VOICE_HEADER_SIZE
                         + (uint64_t)n_voices * n_tokens * sizeof(voice_entry_t);
    if (index_bytes > size) {
        ESP_LOGW(TAG, "Voice pack index (%llu B) exceeds partition (%u B)",
                 (unsigned long long)index_bytes, (unsigned)size);
        return false;
    }

    const voice_entry_t *entries = (const voice_entry_t *)(bytes + VOICE_HEADER_SIZE);
    const int count = (int)n_voices * (int)n_tokens;
    for (int i = 0; i < count; i++) {
        uint32_t off = entries[i].offset;
        uint32_t len = entries[i].length;
        if (len == 0) continue;                       // word absent -> silence
        if (off < index_bytes || (off & 1u) != 0 || (uint64_t)off + len > size) {
            ESP_LOGW(TAG, "Voice pack entry %d out of bounds (offset=%u len=%u partition=%u)",
                     i, (unsigned)off, (unsigned)len, (unsigned)size);
            return false;
        }
    }

    s_voice_num_voices = n_voices;
    s_voice_num_tokens = n_tokens;
    s_voice_pack_size  = size;
    ESP_LOGI(TAG, "Voice pack OK: %u voices x %u words, %u-byte index in %u-byte partition",
             n_voices, n_tokens, (unsigned)index_bytes, (unsigned)size);
    return true;
}

void hal_audio_init(int initial_volume) {
    s_audio_mutex = xSemaphoreCreateMutex();
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice");
    if (!part) {
        ESP_LOGW(TAG, "Voice partition not found!");
        return;
    }
    const void *ptr = NULL;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &s_voice_mmap_handle) == ESP_OK) {
        const uint8_t *bytes = (const uint8_t *)ptr;
        if (voice_pack_validate(bytes, part->size)) {
            s_voice_mmap_ptr = bytes;
            ESP_LOGI(TAG, "Mounted Hebrew voice pack ('WSMV') in flash at %p", ptr);
        } else {
            ESP_LOGW(TAG, "Voice pack rejected; spoken audio is disabled until it is reflashed");
        }
    }

    if (bsp_display_lock(1000)) {
        // CRITICAL HARDWARE DIFFERENCE (-1.75C vs -1.75):
        // Waveshare ESP32-S3-Touch-AMOLED-1.75C routes I2S_MCLK to GPIO 16 (see waveshare/esp32_s3_touch_amoled_1_75c),
        // whereas the bare -1.75 board (with TCA9554) routes I2S_MCLK to GPIO 42.
        gpio_num_t mclk_pin = (s_io_exp != NULL) ? GPIO_NUM_42 : GPIO_NUM_16;
        i2s_std_config_t i2s_cfg = {};
        i2s_cfg.clk_cfg.sample_rate_hz = 16000;
        i2s_cfg.clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;
        i2s_cfg.clk_cfg.mclk_multiple  = I2S_MCLK_MULTIPLE_256;
        i2s_cfg.slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
        i2s_cfg.gpio_cfg.mclk = mclk_pin;
        i2s_cfg.gpio_cfg.bclk = BSP_I2S_SCLK;
        i2s_cfg.gpio_cfg.ws   = BSP_I2S_LCLK;
        i2s_cfg.gpio_cfg.dout = BSP_I2S_DOUT;
        i2s_cfg.gpio_cfg.din  = BSP_I2S_DSIN;
        ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_audio_init(&i2s_cfg));

        s_spk_dev = bsp_audio_codec_speaker_init();
        esp_codec_dev_handle_t mic_dev = bsp_audio_codec_microphone_init();
        if (s_spk_dev) {
            esp_codec_dev_sample_info_t fs = {
                .bits_per_sample = 16,
                .channel = 2,
                .channel_mask = 0,
                .sample_rate = 16000,
                .mclk_multiple = 0,
            };
            int open_ret = esp_codec_dev_open(s_spk_dev, &fs);
            if (mic_dev) {
                esp_codec_dev_open(mic_dev, &fs);
                esp_codec_dev_set_in_gain(mic_dev, 24.0);
            }
            int vol_ret  = esp_codec_dev_set_out_vol(s_spk_dev, initial_volume);
            esp_codec_dev_set_out_mute(s_spk_dev, false);

            int r00 = -1, r01 = -1, r31 = -1, r32 = -1, r37 = -1;
            esp_codec_dev_read_reg(s_spk_dev, 0x00, &r00);
            esp_codec_dev_read_reg(s_spk_dev, 0x01, &r01);
            esp_codec_dev_read_reg(s_spk_dev, 0x31, &r31);
            esp_codec_dev_read_reg(s_spk_dev, 0x32, &r32);
            esp_codec_dev_read_reg(s_spk_dev, 0x37, &r37);
            ESP_LOGI(TAG, "ES8311 init (MCLK=GPIO%d, 16kHz Stereo): open=%d vol=%d | 00:%02X 01:%02X 31:%02X 32:%02X 37:%02X",
                     (int)mclk_pin, open_ret, vol_ret, r00, r01, r31, r32, r37);
        }
        bsp_display_unlock();
    }
}

void hal_audio_play_tokens(const heb_token_id_t *seq, int seq_len, int voice_id, int volume) {
    if (!s_voice_mmap_ptr || !s_spk_dev || !s_audio_mutex) {
        ESP_LOGW(TAG, "Audio not ready (mmap=%p, spk=%p)", s_voice_mmap_ptr, s_spk_dev);
        return;
    }
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    int vid = (voice_id == 1) ? 1 : 0;
    if (vid >= (int)s_voice_num_voices) {
        ESP_LOGW(TAG, "Voice %d not in pack (%u voices); falling back to voice 0",
                 vid, (unsigned)s_voice_num_voices);
        vid = 0;
    }
    const uint16_t num_tokens = s_voice_num_tokens;
    const voice_entry_t *index_base = (const voice_entry_t *)(s_voice_mmap_ptr + VOICE_HEADER_SIZE);

    int r31 = -1, r32 = -1;
    if (bsp_display_lock(200)) {
        esp_codec_dev_set_out_mute(s_spk_dev, false);
        esp_codec_dev_set_out_vol(s_spk_dev, volume);
        esp_codec_dev_read_reg(s_spk_dev, 0x31, &r31);
        esp_codec_dev_read_reg(s_spk_dev, 0x32, &r32);
        bsp_display_unlock();
    }
    gpio_set_level(BSP_POWER_AMP_IO, 1);
    ESP_LOGI(TAG, "Playing %d Hebrew words (voice=%d, vol=%d, REG31=0x%02X, REG32=0x%02X, PA=%d)",
             seq_len, vid, volume, r31, r32, gpio_get_level(BSP_POWER_AMP_IO));

    // Allocate a writable internal-RAM stereo chunk buffer (256 stereo frames = 1024 bytes)
    int16_t stereo_chunk[512];
    memset(stereo_chunk, 0, sizeof(stereo_chunk));

    int total_bytes_written = 0;
    int last_err = 0;
    for (int i = 0; i < seq_len; i++) {
        int tok = (int)seq[i];
        if (tok < 0 || tok >= (int)num_tokens) continue;
        const voice_entry_t *ent = &index_base[vid * num_tokens + tok];
        // Re-checked per play: the pack is flash-mapped data, not trusted input.
        if (ent->length > 0 && ent->offset >= VOICE_HEADER_SIZE &&
            (uint64_t)ent->offset + ent->length <= s_voice_pack_size) {
            const int16_t *mono_samples = (const int16_t *)(s_voice_mmap_ptr + ent->offset);
            int total_samples = (int)(ent->length / sizeof(int16_t));
            int idx = 0;
            while (idx < total_samples) {
                int batch = total_samples - idx;
                if (batch > 256) batch = 256;
                for (int k = 0; k < batch; k++) {
                    int16_t s = mono_samples[idx + k];
                    stereo_chunk[2 * k]     = s; // Left
                    stereo_chunk[2 * k + 1] = s; // Right
                }
                int bytes = batch * 2 * (int)sizeof(int16_t);
                int w_ret = esp_codec_dev_write(s_spk_dev, stereo_chunk, bytes);
                if (w_ret != 0) last_err = w_ret;
                else total_bytes_written += bytes;
                idx += batch;
            }
            // Short 30ms pause between words
            memset(stereo_chunk, 0, sizeof(stereo_chunk));
            esp_codec_dev_write(s_spk_dev, stereo_chunk, sizeof(stereo_chunk));
            esp_codec_dev_write(s_spk_dev, stereo_chunk, sizeof(stereo_chunk));
        }
    }

    // Trailing silence (~90ms) to flush I2S DMA ringbuffer
    memset(stereo_chunk, 0, sizeof(stereo_chunk));
    for (int p = 0; p < 6; p++) {
        esp_codec_dev_write(s_spk_dev, stereo_chunk, sizeof(stereo_chunk));
    }
    ESP_LOGI(TAG, "Finished playing Hebrew time phrase (%d stereo bytes written, last_err=%d)",
             total_bytes_written, last_err);

    xSemaphoreGive(s_audio_mutex);
}

void hal_audio_play_chime(int volume) {
    if (!s_spk_dev || !s_audio_mutex) {
        ESP_LOGW(TAG, "Audio not ready for chime (spk=%p)", s_spk_dev);
        return;
    }
    if (xSemaphoreTake(s_audio_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    int r31 = -1, r32 = -1;
    if (bsp_display_lock(200)) {
        esp_codec_dev_set_out_mute(s_spk_dev, false);
        esp_codec_dev_set_out_vol(s_spk_dev, volume);
        esp_codec_dev_read_reg(s_spk_dev, 0x31, &r31);
        esp_codec_dev_read_reg(s_spk_dev, 0x32, &r32);
        bsp_display_unlock();
    }
    gpio_set_level(BSP_POWER_AMP_IO, 1);
    ESP_LOGI(TAG, "Playing vintage mechanical winding clock chime (vol=%d, REG31=0x%02X, REG32=0x%02X, PA=%d)",
             volume, r31, r32, gpio_get_level(BSP_POWER_AMP_IO));

    // Allocate or reuse pre-rendered 16kHz 16-bit mono PCM buffer for the chime in PSRAM (65,200 samples = ~130 KB)
    static int16_t *s_chime_pcm = NULL;
    static constexpr int CHIME_WIND_SAMPLES  = 9200;  // 0.575s mechanical winding ratchet
    static constexpr int CHIME_BELL_SAMPLES  = 56000; // 3.50s 4-note Westminster / Mantel bronze bells
    static constexpr int CHIME_TOTAL_SAMPLES = CHIME_WIND_SAMPLES + CHIME_BELL_SAMPLES;

    if (!s_chime_pcm) {
        s_chime_pcm = (int16_t *)heap_caps_calloc(CHIME_TOTAL_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (s_chime_pcm) {
            const float sr = 16000.0f;
            const float two_pi = 6.283185307f;

            // Part 1: 11 crisp mechanical winding escapement clicks
            const int click_offsets[11] = {
                200, 920, 1600, 2240, 2860, 3480, 4120, 4780, 5480, 6220, 7040
            };
            for (int c = 0; c < 11; c++) {
                int start = click_offsets[c];
                for (int dt = 0; dt < 360 && (start + dt) < CHIME_WIND_SAMPLES; dt++) {
                    float t = dt / sr;
                    float env = expf(-t * 170.0f);
                    float gear = sinf(two_pi * 2450.0f * t) * 0.60f
                               + sinf(two_pi * 3820.0f * t) * 0.30f
                               + sinf(two_pi * 1180.0f * t) * 0.25f;
                    int32_t v = s_chime_pcm[start + dt] + (int32_t)(env * gear * 22000.0f);
                    if (v > 32000) v = 32000;
                    if (v < -32000) v = -32000;
                    s_chime_pcm[start + dt] = (int16_t)v;
                }
            }

            // Part 2: Four-note Westminster / Mantel Clock Bronze Bell Chime (E5 -> D5 -> C5 -> G4)
            // Synthesized via fast complex phasor recurrence (zero sinf/expf inside sample loop!)
            struct bell_note_t {
                float freq;
                int   start_sample;
                int   dur_samples;
            };
            const bell_note_t notes[4] = {
                { 659.25f,     0, 18000 }, // E5
                { 587.33f,  8800, 18000 }, // D5
                { 523.25f, 17600, 18000 }, // C5
                { 392.00f, 26400, 29600 }, // G4
            };
            const float ratios[5] = { 1.000f, 2.000f, 2.756f, 4.070f, 5.410f };
            const float decays[5] = { 2.4f,   4.5f,   8.0f,   15.0f,  95.0f  };
            const float amps[5]   = { 0.52f,  0.24f,  0.14f,  0.08f,  0.18f  };

            int16_t *bell_dst = s_chime_pcm + CHIME_WIND_SAMPLES;
            for (int b = 0; b < 4; b++) {
                float f0 = notes[b].freq;
                int start = notes[b].start_sample;
                int dur   = notes[b].dur_samples;

                // Complex phasor state (re, im) and per-sample multiplier (m_re, m_im) for each partial
                float re[5], im[5], m_re[5], m_im[5];
                for (int p = 0; p < 5; p++) {
                    re[p] = 0.0f;
                    im[p] = amps[p]; // sin starts at 0 (re=0, im=amp; we take re after rotation or im)
                    float w = two_pi * (f0 * ratios[p]) / sr;
                    float d = expf(-decays[p] / sr);
                    m_re[p] = d * cosf(w);
                    m_im[p] = d * sinf(w);
                }

                for (int dn = 0; dn < dur && (start + dn) < CHIME_BELL_SAMPLES; dn++) {
                    float sum = 0.0f;
                    for (int p = 0; p < 5; p++) {
                        float n_re = re[p] * m_re[p] - im[p] * m_im[p];
                        float n_im = re[p] * m_im[p] + im[p] * m_re[p];
                        re[p] = n_re;
                        im[p] = n_im;
                        sum += re[p];
                    }
                    float attack = (dn < 64) ? (dn / 64.0f) : 1.0f;
                    int32_t v = bell_dst[start + dn] + (int32_t)(attack * sum * 26000.0f);
                    if (v > 32000) v = 32000;
                    if (v < -32000) v = -32000;
                    bell_dst[start + dn] = (int16_t)v;
                }
            }
            vTaskDelay(1);
        }
    }

    int16_t stereo_chunk[512];
    memset(stereo_chunk, 0, sizeof(stereo_chunk));

    if (s_chime_pcm) {
        for (int idx = 0; idx < CHIME_TOTAL_SAMPLES; idx += 256) {
            int batch = CHIME_TOTAL_SAMPLES - idx;
            if (batch > 256) batch = 256;
            for (int k = 0; k < batch; k++) {
                int16_t s = s_chime_pcm[idx + k];
                stereo_chunk[2 * k]     = s;
                stereo_chunk[2 * k + 1] = s;
            }
            esp_codec_dev_write(s_spk_dev, stereo_chunk, batch * 2 * (int)sizeof(int16_t));
        }
    }

    // Flush I2S DMA ringbuffer with trailing silence
    memset(stereo_chunk, 0, sizeof(stereo_chunk));
    for (int p = 0; p < 6; p++) {
        esp_codec_dev_write(s_spk_dev, stereo_chunk, sizeof(stereo_chunk));
    }
    ESP_LOGI(TAG, "Finished playing vintage winding clock chime");

    xSemaphoreGive(s_audio_mutex);
}
