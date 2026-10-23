#include "net_time.h"
#include "app_config.h"
#include "hal_board.h"
#include "ui_screens.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "net_time";

volatile bool g_wifi_connected = false;
volatile bool g_wifi_sleeping  = false;
volatile int  g_wifi_rssi      = -100;
char          g_wifi_ip[32]    = "Disconnected";
volatile bool g_ntp_synced     = false;

static constexpr uint64_t WIFI_SLEEP_HOLD_AFTER_SYNC_US = 8ULL * 1000000ULL;           // 8s grace window after NTP sync
static constexpr uint64_t WIFI_WAKE_INTERVAL_US         = 6ULL * 3600ULL * 1000000ULL; // Wake every 6 hours to re-sync NTP

static esp_timer_handle_t s_reconnect_timer     = NULL;
static esp_timer_handle_t s_sleep_hold_timer    = NULL;
static esp_timer_handle_t s_wakeup_timer        = NULL;
static volatile bool      s_wifi_started        = false;
static volatile bool      s_scanning            = false;
static volatile bool      s_manual_reconnecting = false;
static volatile int       s_fail_count          = 0;

static void wifi_roam_scan_task(void *arg);

static inline bool is_external_power_active(void) {
    return g_usb_plugged || g_is_charging;
}

static void enter_wifi_sleep_cb(void *arg) {
    if (s_scanning || s_manual_reconnecting) {
        return;
    }
    if (is_external_power_active()) {
        ESP_LOGI(TAG, "USB/charging active -> keeping Wi-Fi connected.");
        return;
    }
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    g_wifi_sleeping  = true;
    g_wifi_connected = false;
    snprintf(g_wifi_ip, sizeof(g_wifi_ip), "Sleeping (6h sync)");

    if (s_wifi_started) {
        s_wifi_started = false;
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    if (s_wakeup_timer) {
        esp_timer_stop(s_wakeup_timer);
        esp_timer_start_once(s_wakeup_timer, WIFI_WAKE_INTERVAL_US);
    }
    ui_request_clock_redraw();
    ESP_LOGI(TAG, "On battery power: Wi-Fi radio powered down after NTP sync. Next automatic wake in 6 hours.");
}

static void wakeup_wifi_cb(void *arg) {
    ESP_LOGI(TAG, "6-hour Wi-Fi wake timer fired -> waking radio for NTP re-sync.");
    net_time_wake();
}

static void reconnect_timer_cb(void *arg) {
    if (!s_scanning && !g_wifi_sleeping && !g_wifi_connected && g_cfg.wifi_ssid[0] != '\0') {
        ESP_LOGI(TAG, "Reconnecting to Wi-Fi SSID '%s'...", g_cfg.wifi_ssid);
        esp_wifi_connect();
    }
}

static void schedule_reconnect(uint64_t delay_ms) {
    if (!s_reconnect_timer || s_scanning || g_wifi_sleeping || g_cfg.wifi_ssid[0] == '\0') {
        return;
    }
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, delay_ms * 1000ULL);
}

static void sntp_sync_cb(struct timeval *tv) {
    g_ntp_synced = true;
    ui_request_clock_redraw();
    hal_rtc_save_utc(tv->tv_sec);
    if (!is_external_power_active()) {
        ESP_LOGI(TAG, "NTP time synchronized on battery! Saving UTC to RTC; scheduling Wi-Fi sleep in 8s.");
        if (s_sleep_hold_timer) {
            esp_timer_stop(s_sleep_hold_timer);
            esp_timer_start_once(s_sleep_hold_timer, WIFI_SLEEP_HOLD_AFTER_SYNC_US);
        }
    } else {
        ESP_LOGI(TAG, "NTP time synchronized! Saving UTC to RTC (keeping Wi-Fi active while charging/USB).");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
        esp_wifi_set_max_tx_power(52); // 13 dBm clean linear TX (avoids near-field saturation at -38 dBm)
        if (g_cfg.wifi_ssid[0] != '\0' && !s_scanning) {
            ESP_LOGI(TAG, "Wi-Fi STA started, connecting to SSID '%s'...", g_cfg.wifi_ssid);
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_wifi_sleeping) {
            return;
        }
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)event_data;
        int reason = dis ? dis->reason : -1;
        g_wifi_connected = false;
        g_wifi_rssi = -100;
        snprintf(g_wifi_ip, sizeof(g_wifi_ip), "Disconnected");
        ui_request_clock_redraw();
        ESP_LOGW(TAG, "Wi-Fi disconnected from '%s' (reason=%d)",
                 g_cfg.wifi_ssid, reason);

        s_fail_count = s_fail_count + 1;
        if (!s_scanning && !s_manual_reconnecting) {
            // If current network is not in range (reason=201 NO_AP_FOUND) or after 2 failed attempts,
            // scan for any remembered Wi-Fi network in range and auto-join it!
            if ((reason == WIFI_REASON_NO_AP_FOUND || s_fail_count >= 2) && g_cfg.known_wifi_count > 0) {
                s_fail_count = 0;
                xTaskCreate(wifi_roam_scan_task, "wifi_roam", 6144, NULL, 4, NULL);
            } else if (g_cfg.wifi_ssid[0] != '\0') {
                schedule_reconnect(1500);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_fail_count = 0;
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
        }
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(g_wifi_ip, sizeof(g_wifi_ip), IPSTR, IP2STR(&event->ip_info.ip));
        g_wifi_connected = true;
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            g_wifi_rssi = ap_info.rssi;
        }
        // Ensure the connected network is at the front of the remembered networks list
        if (g_cfg.wifi_ssid[0] != '\0' && g_cfg.wifi_pass[0] != '\0') {
            if (g_cfg.known_wifi_count == 0 || strcmp(g_cfg.known_wifi[0].ssid, g_cfg.wifi_ssid) != 0) {
                app_config_save();
            } else {
                app_config_remember_wifi(g_cfg.wifi_ssid, g_cfg.wifi_pass);
            }
        }
        ui_request_clock_redraw();
        ESP_LOGI(TAG, "Wi-Fi connected! SSID: '%s', IP: %s, RSSI: %d dBm",
                 g_cfg.wifi_ssid, g_wifi_ip, g_wifi_rssi);

        if (esp_sntp_enabled()) {
            esp_sntp_restart();
        }
    }
}

static void populate_wifi_sta_config(wifi_config_t *wifi_config) {
    memset(wifi_config, 0, sizeof(*wifi_config));
    strncpy((char *)wifi_config->sta.ssid, g_cfg.wifi_ssid, sizeof(wifi_config->sta.ssid) - 1);
    strncpy((char *)wifi_config->sta.password, g_cfg.wifi_pass, sizeof(wifi_config->sta.password) - 1);
    wifi_config->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config->sta.threshold.rssi = -90;
    // Require at least WPA2-PSK. With ALL_CHANNEL_SCAN plus signal sorting, an open
    // or WEP twin of a known SSID would otherwise win the association race.
    wifi_config->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (g_cfg.wifi_pass[0] == '\0') {
        ESP_LOGW(TAG, "No Wi-Fi password stored: open networks are refused, so connect will keep failing until one is set");
    }
    wifi_config->sta.pmf_cfg.capable = false;
    wifi_config->sta.pmf_cfg.required = false;
    wifi_config->sta.failure_retry_cnt = 2;
}

void net_time_init(void) {
    const esp_timer_create_args_t reconn_args = {
        .callback = &reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_reconn",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconn_args, &s_reconnect_timer));

    const esp_timer_create_args_t sleep_hold_args = {
        .callback = &enter_wifi_sleep_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_sleep_hold",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&sleep_hold_args, &s_sleep_hold_timer));

    const esp_timer_create_args_t wakeup_args = {
        .callback = &wakeup_wifi_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_6h_wake",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&wakeup_args, &s_wakeup_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // Force pure WPA2-PSK negotiation on WPA2/WPA3-transitional routers (prevents reason=15 EAPOL timeout)
    cfg.feature_caps &= ~CONFIG_FEATURE_WPA3_SAE_BIT;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();

    wifi_config_t wifi_config;
    populate_wifi_sta_config(&wifi_config);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_disable_pmf_config(WIFI_IF_STA));
    s_wifi_started = true;
    ESP_ERROR_CHECK(esp_wifi_start());
}

void net_time_wake(void) {
    if (s_sleep_hold_timer) {
        esp_timer_stop(s_sleep_hold_timer);
    }
    if (s_wakeup_timer) {
        esp_timer_stop(s_wakeup_timer);
    }
    if (g_wifi_sleeping || !s_wifi_started) {
        g_wifi_sleeping = false;
        snprintf(g_wifi_ip, sizeof(g_wifi_ip), "Connecting...");
        wifi_config_t wifi_config;
        populate_wifi_sta_config(&wifi_config);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_disable_pmf_config(WIFI_IF_STA);
        if (esp_wifi_start() == ESP_OK) {
            s_wifi_started = true;
        }
        ui_request_clock_redraw();
    }
}

void net_time_reconnect(void) {
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_sleep_hold_timer) {
        esp_timer_stop(s_sleep_hold_timer);
    }
    if (s_wakeup_timer) {
        esp_timer_stop(s_wakeup_timer);
    }
    s_manual_reconnecting = true;
    if (g_wifi_sleeping || !s_wifi_started) {
        g_wifi_sleeping = false;
        wifi_config_t wifi_config;
        populate_wifi_sta_config(&wifi_config);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_disable_pmf_config(WIFI_IF_STA);
        if (esp_wifi_start() == ESP_OK) {
            s_wifi_started = true;
        }
        s_manual_reconnecting = false;
        return;
    }

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t wifi_config;
    populate_wifi_sta_config(&wifi_config);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_disable_pmf_config(WIFI_IF_STA);
    s_manual_reconnecting = false;

    if (g_cfg.wifi_ssid[0] != '\0') {
        esp_wifi_connect();
    }
}

void net_time_poll_rssi(void) {
    if (is_external_power_active()) {
        if (s_sleep_hold_timer) {
            esp_timer_stop(s_sleep_hold_timer);
        }
        if (g_wifi_sleeping) {
            ESP_LOGI(TAG, "External USB/charging power detected -> waking Wi-Fi radio.");
            net_time_wake();
            return;
        }
    } else if (g_ntp_synced && g_wifi_connected && !g_wifi_sleeping && !s_scanning) {
        if (s_sleep_hold_timer && !esp_timer_is_active(s_sleep_hold_timer)) {
            ESP_LOGI(TAG, "Switched to battery power after NTP sync -> scheduling Wi-Fi sleep in 8s.");
            esp_timer_start_once(s_sleep_hold_timer, WIFI_SLEEP_HOLD_AFTER_SYNC_US);
        }
    }

    if (g_wifi_connected) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            g_wifi_rssi = ap_info.rssi;
        }
    }
}

static bool ssid_list_contains_exact(const char *opts, const char *ssid) {
    size_t ssid_len = strlen(ssid);
    if (ssid_len == 0) return true;
    const char *p = opts;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        if (line_len == ssid_len && strncmp(p, ssid, line_len) == 0) {
            return true;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

static void wifi_scan_task(void *arg) {
    s_scanning = true;
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_sleep_hold_timer) {
        esp_timer_stop(s_sleep_hold_timer);
    }
    if (g_wifi_sleeping || !s_wifi_started) {
        g_wifi_sleeping = false;
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (esp_wifi_start() == ESP_OK) {
            s_wifi_started = true;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 80;
    scan_cfg.scan_time.active.max = 200;

    ESP_LOGI(TAG, "Starting Wi-Fi AP scan...");
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_ERR_WIFI_STATE) {
        // STA was in mid-connection state; pause connection attempt and retry scan
        ESP_LOGI(TAG, "STA busy connecting; disconnecting briefly to run Wi-Fi scan...");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
        err = esp_wifi_scan_start(&scan_cfg, true);
    }

    char *opts = (char *)calloc(1, 1024);
    int unique_count = 0;

    if (err == ESP_OK && opts != NULL) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 30) ap_count = 30;
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)calloc(ap_count > 0 ? ap_count : 1, sizeof(wifi_ap_record_t));
        if (ap_list && ap_count > 0) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);
            for (int i = 0; i < ap_count; i++) {
                const char *ssid = (const char *)ap_list[i].ssid;
                if (ssid[0] == '\0') continue;
                if (ssid_list_contains_exact(opts, ssid)) continue;
                if (opts[0] != '\0') strncat(opts, "\n", 1023 - strlen(opts));
                strncat(opts, ssid, 1023 - strlen(opts));
                unique_count++;
            }
        }
        free(ap_list);
        ESP_LOGI(TAG, "Wi-Fi scan complete: %d unique SSIDs (from %u APs)", unique_count, (unsigned)ap_count);
        ui_update_wifi_scan_results(opts, unique_count);
    } else {
        ESP_LOGW(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
        ui_update_wifi_scan_results("", 0);
    }

    // Also check if a known network was discovered during manual scan while disconnected
    free(opts);
    s_scanning = false;
    if (!g_wifi_connected && g_cfg.wifi_ssid[0] != '\0') {
        schedule_reconnect(1500);
    }
    vTaskDelete(NULL);
}

static void wifi_roam_scan_task(void *arg) {
    (void)arg;
    if (s_scanning || g_wifi_sleeping || g_wifi_connected) {
        vTaskDelete(NULL);
        return;
    }
    s_scanning = true;
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 60;
    scan_cfg.scan_time.active.max = 160;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_ERR_WIFI_STATE) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(120));
        err = esp_wifi_scan_start(&scan_cfg, true);
    }

    char matched_ssid[64] = "";
    char matched_pass[64] = "";
    int  matched_rssi     = -127;

    if (err == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 30) ap_count = 30;
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)calloc(ap_count > 0 ? ap_count : 1, sizeof(wifi_ap_record_t));
        if (ap_list && ap_count > 0) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);
            // ap_list is sorted strongest-RSSI first; find strongest AP in g_cfg.known_wifi[]
            for (int i = 0; i < ap_count; i++) {
                const char *ssid = (const char *)ap_list[i].ssid;
                if (ssid[0] == '\0' || ap_list[i].authmode < WIFI_AUTH_WPA2_PSK) continue;
                char pass_buf[64] = "";
                if (app_config_find_wifi_pass(ssid, pass_buf, sizeof(pass_buf))) {
                    strncpy(matched_ssid, ssid, sizeof(matched_ssid) - 1);
                    strncpy(matched_pass, pass_buf, sizeof(matched_pass) - 1);
                    matched_rssi = ap_list[i].rssi;
                    break;
                }
            }
        }
        free(ap_list);
    }

    s_scanning = false;

    if (matched_ssid[0] != '\0') {
        bool switched = (strcmp(g_cfg.wifi_ssid, matched_ssid) != 0) || (strcmp(g_cfg.wifi_pass, matched_pass) != 0);
        if (switched) {
            ESP_LOGI(TAG, "Auto-roam: found remembered Wi-Fi '%s' (RSSI %d dBm) -> switching & joining",
                     matched_ssid, matched_rssi);
            memset(g_cfg.wifi_ssid, 0, sizeof(g_cfg.wifi_ssid));
            memset(g_cfg.wifi_pass, 0, sizeof(g_cfg.wifi_pass));
            strncpy(g_cfg.wifi_ssid, matched_ssid, sizeof(g_cfg.wifi_ssid) - 1);
            strncpy(g_cfg.wifi_pass, matched_pass, sizeof(g_cfg.wifi_pass) - 1);
            app_config_save();
            ui_sync_wifi_fields();
            net_time_reconnect();
            vTaskDelete(NULL);
            return;
        }
    }

    if (!g_wifi_connected && !g_wifi_sleeping && g_cfg.wifi_ssid[0] != '\0') {
        schedule_reconnect(2000);
    }
    vTaskDelete(NULL);
}

void net_time_start_scan(void) {
    if (s_scanning) return;
    xTaskCreate(wifi_scan_task, "wifi_scan", 8192, NULL, 4, NULL);
}
