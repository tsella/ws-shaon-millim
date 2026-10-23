#include "app_config.h"
#if __has_include("default_config_seed.h")
#include "default_config_seed.h"
#else
#define SEED_WIFI_SSID      ""
#define SEED_WIFI_PASSWORD  ""
#define SEED_TIMEZONE       "America/New_York"
#define SEED_COLOR_PRESET   "shabbat"
#define SEED_PURE_BLACK     0
#define SEED_SIZING_STATIC  1
#define SEED_ALIGN_CENTER   1
#define SEED_NIKKUD_MODE    0
#define SEED_SHOW_SECONDS   1
#define SEED_VOICE_ID       0
#define SEED_VOLUME         80
#define SEED_HOURLY_ENABLED 1
#define SEED_HOURLY_START   7
#define SEED_HOURLY_END     21
#define SEED_BRIGHTNESS     85
#define SEED_TIMEOUT_SEC    60
#define SEED_ALWAYS_ON_USB  1
#endif

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "app_config";

app_config_t g_cfg;

const preset_spec_t PRESETS[] = {
    { "shabbat",       {0x09, 0x0D, 0x1E}, {0xFD, 0xE6, 0x8A}, {0xF5, 0x9E, 0x0B}, {0x60, 0xA5, 0xFA} },
    { "oled-warm",     {0x00, 0x00, 0x00}, {0xFF, 0xF8, 0xE7}, {0xF5, 0x9E, 0x0B}, {0x38, 0xBD, 0xF8} },
    { "cyber",         {0x06, 0x04, 0x12}, {0xF0, 0xAB, 0xFC}, {0x22, 0xD3, 0xEE}, {0x81, 0x8C, 0xF8} },
    { "emerald",       {0x02, 0x10, 0x08}, {0xEC, 0xFD, 0xF5}, {0x34, 0xD3, 0x99}, {0xFB, 0xBF, 0x24} },
    { "crimson-night", {0x09, 0x02, 0x04}, {0xFE, 0xCA, 0xCA}, {0xEF, 0x44, 0x44}, {0x7F, 0x1D, 0x1D} },
    { "eink",          {0xF5, 0xF2, 0xEB}, {0x1A, 0x1A, 0x1A}, {0x5A, 0x5A, 0x5A}, {0x78, 0x78, 0x78} },
    { "dynamic",       {0x05, 0x06, 0x08}, {0xFF, 0xF8, 0xEB}, {0xF5, 0x9E, 0x0B}, {0x94, 0xA3, 0xB8} },
};
const int PRESET_COUNT = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));

const tz_entry_t TZ_TABLE[] = {
    { "America/New_York",    "EST5EDT,M3.2.0,M11.1.0" },
    { "Asia/Jerusalem",      "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "America/Chicago",     "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver",      "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Toronto",     "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Sao_Paulo",   "<-03>3" },
    { "Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Paris",        "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich",       "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Athens",       "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Asia/Tokyo",          "JST-9" },
    { "Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "UTC",                 "UTC0" },
};
const int TZ_COUNT = (int)(sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]));

const int TIMEOUT_VALUES_SEC[5] = { 30, 60, 120, 300, 0 };

static int find_tz_index(const char *iana) {
    for (int i = 0; i < TZ_COUNT; i++) {
        if (strcasecmp(TZ_TABLE[i].iana, iana) == 0 ||
            (strcasecmp(iana, "America/New-York") == 0 && i == 0)) {
            return i;
        }
    }
    return 0;
}

static int find_preset_index(const char *name) {
    for (int i = 0; i < PRESET_COUNT; i++) {
        if (strcasecmp(PRESETS[i].name, name) == 0) {
            return i;
        }
    }
    return 0;
}

static int clamp_int(int v, int lo, int hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// Every value below is read back from NVS, which a power loss during a settings
// save or a bad flash image can leave out of range. The renderer and the supervisor
// loop index fixed arrays with these values, and a negative index reads before the
// array, so normalise them once here.
static void app_config_clamp(void) {
    g_cfg.tz_index       = clamp_int(g_cfg.tz_index, 0, TZ_COUNT - 1);
    g_cfg.preset_index   = clamp_int(g_cfg.preset_index, 0, PRESET_COUNT - 1);
    g_cfg.nikkud_mode    = clamp_int(g_cfg.nikkud_mode, 0, 2);
    g_cfg.voice_id       = clamp_int(g_cfg.voice_id, 0, 2);
    g_cfg.volume         = clamp_int(g_cfg.volume, 0, 100);
    g_cfg.hourly_start   = clamp_int(g_cfg.hourly_start, 0, 23);
    g_cfg.hourly_end     = clamp_int(g_cfg.hourly_end, 0, 23);
    g_cfg.brightness     = clamp_int(g_cfg.brightness, 10, 100);
    g_cfg.timeout_idx    = clamp_int(g_cfg.timeout_idx, 0, 4);
}

void app_config_apply_timezone(void) {
    int idx = g_cfg.tz_index;
    if (idx < 0 || idx >= TZ_COUNT) idx = 0;
    setenv("TZ", TZ_TABLE[idx].posix, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to %s (%s)", TZ_TABLE[idx].iana, TZ_TABLE[idx].posix);
}

void app_config_save(void) {
    nvs_handle_t h;
    if (nvs_open("ws_millim", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "seeded", 1);
        nvs_set_str(h, "ssid", g_cfg.wifi_ssid);
        nvs_set_str(h, "pass", g_cfg.wifi_pass);
        nvs_set_i32(h, "tz_idx", g_cfg.tz_index);
        nvs_set_i32(h, "preset", g_cfg.preset_index);
        nvs_set_u8(h, "pure_blk", g_cfg.pure_black_oled ? 1 : 0);
        nvs_set_u8(h, "static_sz", g_cfg.sizing_static ? 1 : 0);
        nvs_set_u8(h, "align_c", g_cfg.align_center ? 1 : 0);
        nvs_set_i32(h, "nikkud", g_cfg.nikkud_mode);
        nvs_set_u8(h, "sec_ind", g_cfg.show_seconds ? 1 : 0);
        nvs_set_i32(h, "voice", g_cfg.voice_id);
        nvs_set_i32(h, "volume", g_cfg.volume);
        nvs_set_u8(h, "hr_en", g_cfg.hourly_enabled ? 1 : 0);
        nvs_set_i32(h, "hr_start", g_cfg.hourly_start);
        nvs_set_i32(h, "hr_end", g_cfg.hourly_end);
        nvs_set_i32(h, "bright", g_cfg.brightness);
        nvs_set_i32(h, "tmo_idx", g_cfg.timeout_idx);
        nvs_set_u8(h, "usb_on", g_cfg.always_on_usb ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "Settings saved to NVS");
    }
}

void app_config_load(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    strncpy(g_cfg.wifi_ssid, SEED_WIFI_SSID, sizeof(g_cfg.wifi_ssid) - 1);
    strncpy(g_cfg.wifi_pass, SEED_WIFI_PASSWORD, sizeof(g_cfg.wifi_pass) - 1);
    g_cfg.tz_index        = find_tz_index(SEED_TIMEZONE);
    g_cfg.preset_index    = find_preset_index(SEED_COLOR_PRESET);
    g_cfg.pure_black_oled = (SEED_PURE_BLACK != 0);
    g_cfg.sizing_static   = (SEED_SIZING_STATIC != 0);
    g_cfg.align_center    = (SEED_ALIGN_CENTER != 0);
    g_cfg.nikkud_mode     = SEED_NIKKUD_MODE;
    g_cfg.show_seconds    = (SEED_SHOW_SECONDS != 0);
    g_cfg.voice_id        = SEED_VOICE_ID;
    g_cfg.volume          = SEED_VOLUME;
    g_cfg.hourly_enabled  = (SEED_HOURLY_ENABLED != 0);
    g_cfg.hourly_start    = SEED_HOURLY_START;
    g_cfg.hourly_end      = SEED_HOURLY_END;
    g_cfg.brightness      = SEED_BRIGHTNESS;
    g_cfg.timeout_idx     = 1;
    for (int i = 0; i < 5; i++) {
        if (TIMEOUT_VALUES_SEC[i] == SEED_TIMEOUT_SEC) g_cfg.timeout_idx = i;
    }
    g_cfg.always_on_usb   = (SEED_ALWAYS_ON_USB != 0);
    app_config_clamp();

    nvs_handle_t h;
    if (nvs_open("ws_millim", NVS_READWRITE, &h) == ESP_OK) {
        uint8_t seeded = 0;
        if (nvs_get_u8(h, "seeded", &seeded) == ESP_OK && seeded == 1) {
            size_t len = sizeof(g_cfg.wifi_ssid);
            nvs_get_str(h, "ssid", g_cfg.wifi_ssid, &len);
            len = sizeof(g_cfg.wifi_pass);
            nvs_get_str(h, "pass", g_cfg.wifi_pass, &len);
            // If NVS had empty SSID or matches config.json seed SSID, ensure config.json password is up to date
            if ((g_cfg.wifi_ssid[0] == '\0' || strcmp(g_cfg.wifi_ssid, SEED_WIFI_SSID) == 0) &&
                strlen(SEED_WIFI_SSID) > 0) {
                memset(g_cfg.wifi_ssid, 0, sizeof(g_cfg.wifi_ssid));
                memset(g_cfg.wifi_pass, 0, sizeof(g_cfg.wifi_pass));
                strncpy(g_cfg.wifi_ssid, SEED_WIFI_SSID, sizeof(g_cfg.wifi_ssid) - 1);
                strncpy(g_cfg.wifi_pass, SEED_WIFI_PASSWORD, sizeof(g_cfg.wifi_pass) - 1);
                nvs_set_str(h, "ssid", g_cfg.wifi_ssid);
                nvs_set_str(h, "pass", g_cfg.wifi_pass);
                nvs_commit(h);
            }
            int32_t v32;
            uint8_t v8;
            if (nvs_get_i32(h, "tz_idx", &v32) == ESP_OK) g_cfg.tz_index = v32;
            if (nvs_get_i32(h, "preset", &v32) == ESP_OK) g_cfg.preset_index = v32;
            if (nvs_get_u8(h, "pure_blk", &v8) == ESP_OK) g_cfg.pure_black_oled = (v8 != 0);
            if (nvs_get_u8(h, "static_sz", &v8) == ESP_OK) g_cfg.sizing_static = (v8 != 0);
            if (nvs_get_u8(h, "align_c", &v8) == ESP_OK) g_cfg.align_center = (v8 != 0);
            if (nvs_get_i32(h, "nikkud", &v32) == ESP_OK) g_cfg.nikkud_mode = v32;
            if (nvs_get_u8(h, "sec_ind", &v8) == ESP_OK) g_cfg.show_seconds = (v8 != 0);
            if (nvs_get_i32(h, "voice", &v32) == ESP_OK) g_cfg.voice_id = v32;
            if (nvs_get_i32(h, "volume", &v32) == ESP_OK) g_cfg.volume = v32;
            if (nvs_get_u8(h, "hr_en", &v8) == ESP_OK) g_cfg.hourly_enabled = (v8 != 0);
            if (nvs_get_i32(h, "hr_start", &v32) == ESP_OK) g_cfg.hourly_start = v32;
            if (nvs_get_i32(h, "hr_end", &v32) == ESP_OK) g_cfg.hourly_end = v32;
            if (nvs_get_i32(h, "bright", &v32) == ESP_OK) g_cfg.brightness = v32;
            if (nvs_get_i32(h, "tmo_idx", &v32) == ESP_OK) g_cfg.timeout_idx = v32;
            if (nvs_get_u8(h, "usb_on", &v8) == ESP_OK) g_cfg.always_on_usb = (v8 != 0);
            nvs_close(h);
        } else {
            nvs_close(h);
            app_config_save();
        }
    }
    app_config_clamp();
    app_config_apply_timezone();
}
