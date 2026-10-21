#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "lvgl.h"
#include "hebrew_glyphs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Live PMIC telemetry state
extern volatile int  g_batt_percent;
extern volatile bool g_is_charging;
extern volatile bool g_usb_plugged;

// 1. Display & Capacitive Touch (CO5300 QSPI + CST9217 I2C)
extern uint16_t *g_vram_shadow; // 466x466 RGB565 hardware-flush shadow buffer in PSRAM
lv_disp_t *hal_display_and_touch_init(int initial_brightness_percent);
void hal_display_set_brightness(int percent);

// 2. Power Management (AXP2101), Real-Time Clock (PCF85063), & Physical Buttons (BOOT + PWR)
void hal_pmic_and_rtc_init(void);
void hal_poll_pmic_telemetry(void);
void hal_rtc_save_utc(time_t now_utc);
bool hal_poll_boot_button_pressed(void);
bool hal_poll_pwr_button_short_press(void);

// 3. Audio Subsystem (ES8311 DAC + NS4150B PA + Voice Partition mmap)
void hal_audio_init(int initial_volume);
void hal_audio_play_tokens(const heb_token_id_t *seq, int seq_len, int voice_id, int volume);
void hal_audio_play_chime(int volume);

#ifdef __cplusplus
}
#endif
