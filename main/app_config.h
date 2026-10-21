#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t r, g, b;
} rgb_t;

typedef struct {
    const char *name;
    rgb_t bg;
    rgb_t letter;
    rgb_t nikkud;
    rgb_t period;
} preset_spec_t;

typedef struct {
    const char *iana;
    const char *posix;
} tz_entry_t;

typedef struct {
    char     wifi_ssid[64];
    char     wifi_pass[64];
    int      tz_index;
    int      preset_index;
    bool     pure_black_oled;
    bool     sizing_static;   // true = Static 60px (default), false = Fit (84/72/60px)
    bool     align_center;    // true = Center (default), false = Right
    int      nikkud_mode;     // 0 = color (default), 1 = full, 2 = none
    bool     show_seconds;    // true = show 60-second perimeter nikkud dot indicator (default)
    int      voice_id;        // 0 = Hila (female, default), 1 = Avri (male), 2 = Chime (winding clock chime)
    int      volume;          // 0..100 (default 80)
    bool     hourly_enabled;  // true = speak on the hour
    int      hourly_start;    // 0..23 (default 7 = 7:00 AM)
    int      hourly_end;      // 0..23 (default 21 = 9:00 PM)
    int      brightness;      // 10..100% (default 85%)
    int      timeout_idx;     // 0=30s, 1=60s (default), 2=120s, 3=300s, 4=Always On (0)
    bool     always_on_usb;   // true = never auto-sleep while USB-C power is connected
} app_config_t;

extern app_config_t g_cfg;

extern const preset_spec_t PRESETS[];
extern const int PRESET_COUNT;

extern const tz_entry_t TZ_TABLE[];
extern const int TZ_COUNT;

extern const int TIMEOUT_VALUES_SEC[5];

void app_config_load(void);
void app_config_save(void);
void app_config_apply_timezone(void);

#ifdef __cplusplus
}
#endif
