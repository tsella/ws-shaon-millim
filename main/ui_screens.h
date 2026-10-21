#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_screens_init(void);
void ui_show_clock_screen(void);
void ui_show_settings_screen(void);
void ui_wake_screen(void);
void ui_sleep_screen(void);
void ui_toggle_screen_sleep(void);
bool ui_is_screen_asleep(void);
int64_t ui_get_last_activity_us(void);

void ui_request_clock_redraw(void);
bool ui_consume_clock_redraw_request(void);
void ui_refresh_wifi_status_label(void);
void ui_update_wifi_scan_results(const char *opts_newline_sep, int ap_count);

#ifdef __cplusplus
}
#endif
