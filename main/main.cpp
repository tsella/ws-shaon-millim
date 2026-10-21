#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"

#include "app_config.h"
#include "hal_board.h"
#include "hebrew_clock.h"
#include "clock_renderer.h"
#include "ui_screens.h"
#include "net_time.h"
#include "screencap.h"

static const char *TAG = "ws_millim";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting ws-millim Hebrew Word Clock on ESP32-S3-Touch-AMOLED-1.75C");

    // 1. Initialize NVS & load persistent settings (seeded from config.json on first boot)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    app_config_load();

    // 2. Initialize Display (CO5300 QSPI with DMA fix) & Capacitive Touch (CST9217)
    lv_disp_t *disp = hal_display_and_touch_init(g_cfg.brightness);
    assert(disp != NULL);

    // 3. Initialize PMIC (AXP2101), RTC (PCF85063), IO Expander (TCA9554), and Audio (ES8311 + Voice Pack)
    hal_pmic_and_rtc_init();
    hal_audio_init(g_cfg.volume);
    hal_poll_pmic_telemetry();

    // 4. Build LVGL Clock & Settings Screens
    if (bsp_display_lock(0)) {
        ui_screens_init();
        bsp_display_unlock();
    }

    // 5. Start Wi-Fi Station, SNTP Time Synchronization, & Serial Screen-Capture Facility
    net_time_init();
    screencap_init();

    int telem_div = 0;
    int last_rendered_min = -1;
    int last_spoken_hour  = -1;

    // 6. Supervisor Control Loop (20 Hz)
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));

        // A. Physical BOOT Button (GPIO 0) -> Speak current time in Hebrew & wake screen
        if (hal_poll_boot_button_pressed()) {
            ESP_LOGI(TAG, "BOOT button pressed -> Speaking time in Hebrew");
            if (bsp_display_lock(100)) {
                ui_wake_screen();
                bsp_display_unlock();
            }
            hebrew_clock_speak_current_time();
        }

        // B. Physical PWR Button (EXIO4 or AXP2101 PKEY_SHORT_IRQ) -> Toggle screen sleep/wake
        if (hal_poll_pwr_button_short_press()) {
            ESP_LOGI(TAG, "PWR button short press -> Toggling screen sleep");
            if (bsp_display_lock(100)) {
                ui_toggle_screen_sleep();
                bsp_display_unlock();
            }
        }

        // C. 1 Hz Telemetry, Hourly Chime, Auto-Sleep, & Clock Redraw
        if (++telem_div >= 20) {
            telem_div = 0;
            hal_poll_pmic_telemetry();
            net_time_poll_rssi();

            time_t now;
            time(&now);
            struct tm tm_loc;
            localtime_r(&now, &tm_loc);

            // Hourly Spoken Time Check (triggers on XX:00:00..05 within [hourly_start, hourly_end] inclusive)
            if (g_cfg.hourly_enabled && tm_loc.tm_min == 0 && tm_loc.tm_sec <= 5 &&
                tm_loc.tm_hour != last_spoken_hour) {
                bool in_window = false;
                if (g_cfg.hourly_start <= g_cfg.hourly_end) {
                    in_window = (tm_loc.tm_hour >= g_cfg.hourly_start && tm_loc.tm_hour <= g_cfg.hourly_end);
                } else {
                    in_window = (tm_loc.tm_hour >= g_cfg.hourly_start || tm_loc.tm_hour <= g_cfg.hourly_end);
                }
                if (in_window) {
                    last_spoken_hour = tm_loc.tm_hour;
                    ESP_LOGI(TAG, "Hourly announcement triggered for %02d:00", tm_loc.tm_hour);
                    hebrew_clock_speak_current_time();
                }
            } else if (tm_loc.tm_min != 0) {
                last_spoken_hour = -1;
            }

            // Screen Auto-Sleep Timeout Check
            int tmo_sec = TIMEOUT_VALUES_SEC[g_cfg.timeout_idx % 5];
            bool stay_on = (tmo_sec == 0) || (g_cfg.always_on_usb && g_usb_plugged);
            if (!stay_on && !ui_is_screen_asleep()) {
                int64_t idle_sec = (esp_timer_get_time() - ui_get_last_activity_us()) / 1000000LL;
                if (idle_sec >= tmo_sec) {
                    if (bsp_display_lock(100)) {
                        ui_sleep_screen();
                        bsp_display_unlock();
                    }
                }
            }

            // Redraw Clock Face on Minute Rollover or Explicit Redraw Request
            int cur_key = tm_loc.tm_hour * 60 + tm_loc.tm_min;
            bool force = ui_consume_clock_redraw_request();
            if (!ui_is_screen_asleep() && (force || cur_key != last_rendered_min)) {
                last_rendered_min = cur_key;
                if (bsp_display_lock(100)) {
                    clock_renderer_render_now();
                    ui_refresh_wifi_status_label();
                    bsp_display_unlock();
                }
            }
        }
    }
}
