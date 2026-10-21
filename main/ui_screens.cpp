#include "ui_screens.h"
#include "app_config.h"
#include "hal_board.h"
#include "hebrew_clock.h"
#include "clock_renderer.h"
#include "net_time.h"

#include <string.h>
#include "lvgl.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

static lv_obj_t *s_scr_clock       = NULL;
static lv_obj_t *s_scr_settings    = NULL;
static lv_obj_t *s_cont_settings   = NULL;
static lv_obj_t *s_lbl_wifi_status = NULL;
static lv_obj_t *s_dd_wifi_ssid    = NULL;
static lv_obj_t *s_ta_wifi_pass    = NULL;
static lv_obj_t *s_kb_wifi         = NULL;

static volatile bool    s_screen_asleep       = false;
static volatile int64_t s_last_activity_us    = 0;
static volatile bool    s_force_redraw        = true;
static volatile int64_t s_status_hold_until_us = 0;

void ui_wake_screen(void) {
    s_last_activity_us = esp_timer_get_time();
    if (s_screen_asleep) {
        s_screen_asleep = false;
        hal_display_set_brightness(g_cfg.brightness);
        s_force_redraw = true;
    }
}

void ui_show_clock_screen(void) {
    ui_wake_screen();
    clock_renderer_render_now();
    if (s_scr_clock) {
        lv_scr_load(s_scr_clock);
        lv_refr_now(NULL);
    }
}

void ui_show_settings_screen(void) {
    ui_wake_screen();
    ui_refresh_wifi_status_label();
    if (s_cont_settings) {
        lv_obj_scroll_to_y(s_cont_settings, 0, LV_ANIM_OFF);
    }
    if (s_scr_settings) {
        lv_scr_load(s_scr_settings);
        lv_refr_now(NULL);
    }
}

void ui_sleep_screen(void) {
    if (!s_screen_asleep) {
        s_screen_asleep = true;
        hal_display_set_brightness(0);
    }
}

void ui_toggle_screen_sleep(void) {
    if (s_screen_asleep) ui_wake_screen();
    else ui_sleep_screen();
}

bool ui_is_screen_asleep(void) {
    return s_screen_asleep;
}

int64_t ui_get_last_activity_us(void) {
    return s_last_activity_us;
}

void ui_request_clock_redraw(void) {
    s_force_redraw = true;
}

bool ui_consume_clock_redraw_request(void) {
    if (s_force_redraw) {
        s_force_redraw = false;
        return true;
    }
    return false;
}

void ui_refresh_wifi_status_label(void) {
    if (esp_timer_get_time() < s_status_hold_until_us) {
        return;
    }
    if (s_lbl_wifi_status) {
        lv_label_set_text_fmt(s_lbl_wifi_status, "SSID: %s (%s)",
                              g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "None", g_wifi_ip);
    }
}

void ui_update_wifi_scan_results(const char *opts_newline_sep, int ap_count) {
    if (bsp_display_lock(300)) {
        s_status_hold_until_us = esp_timer_get_time() + 10000000LL; // Hold scan status message for 10s
        if (s_dd_wifi_ssid && opts_newline_sep && opts_newline_sep[0] != '\0') {
            lv_dropdown_set_options(s_dd_wifi_ssid, opts_newline_sep);
            char sel_buf[64] = "";
            lv_dropdown_get_selected_str(s_dd_wifi_ssid, sel_buf, sizeof(sel_buf));
            if (sel_buf[0] != '\0') {
                strncpy(g_cfg.wifi_ssid, sel_buf, sizeof(g_cfg.wifi_ssid) - 1);
            }
            if (lv_scr_act() == s_scr_settings) {
                lv_dropdown_open(s_dd_wifi_ssid);
            }
        }
        if (s_lbl_wifi_status) {
            if (ap_count > 0) {
                lv_label_set_text_fmt(s_lbl_wifi_status, "Found %d networks", ap_count);
            } else {
                lv_label_set_text(s_lbl_wifi_status, "No networks found");
            }
        }
        bsp_display_unlock();
    }
}

static void on_clock_touch_cb(lv_event_t *e) {
    ui_wake_screen();
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    // Bottom 6 o'clock Settings Cog touch zone (x in 180..286, y >= 390)
    if (pt.x >= 180 && pt.x <= 286 && pt.y >= 390) {
        ui_refresh_wifi_status_label();
        if (s_cont_settings) {
            lv_obj_scroll_to_y(s_cont_settings, 0, LV_ANIM_OFF);
        }
        lv_scr_load_anim(s_scr_settings, LV_SCR_LOAD_ANIM_MOVE_TOP, 220, 0, false);
    }
}

static lv_obj_t *make_section_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x60A5FA), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_pad_top(lbl, 10, 0);
    return lbl;
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *title) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 356, 52);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x131B2E), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    return row;
}

void ui_screens_init(void) {
    s_last_activity_us = esp_timer_get_time();

    // ------------------------------------------------------------------------
    // Screen 1: Clock Face
    // ------------------------------------------------------------------------
    s_scr_clock = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_clock, lv_color_black(), 0);
    lv_obj_clear_flag(s_scr_clock, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *canvas = clock_renderer_init_canvas(s_scr_clock);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, on_clock_touch_cb, LV_EVENT_CLICKED, NULL);

    // ------------------------------------------------------------------------
    // Screen 2: Settings Menu (Circular Safe-Zone Scrollable Container)
    // ------------------------------------------------------------------------
    s_scr_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_settings, lv_color_hex(0x090D1E), 0);

    lv_obj_t *cont = lv_obj_create(s_scr_settings);
    s_cont_settings = cont;
    lv_obj_set_size(cont, 466, 466);
    lv_obj_center(cont);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_top(cont, 46, 0);
    lv_obj_set_style_pad_bottom(cont, 76, 0);
    lv_obj_set_style_pad_row(cont, 10, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Back to Clock Button
    lv_obj_t *btn_back = lv_btn_create(cont);
    lv_obj_set_size(btn_back, 200, 44);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0xF59E0B), 0);
    lv_obj_set_style_radius(btn_back, 22, 0);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT "  Back to Clock");
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(0x090D1E), 0);
    lv_obj_set_style_text_font(lbl_back, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, [](lv_event_t *e) {
        ui_wake_screen();
        app_config_save();
        ui_request_clock_redraw();
        clock_renderer_render_now();
        lv_scr_load_anim(s_scr_clock, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 220, 0, false);
    }, LV_EVENT_CLICKED, NULL);

    // --- Section: Display & Theme ---
    make_section_label(cont, "Display & Theme");

    {
        lv_obj_t *row = make_row(cont, "Theme");
        lv_obj_t *dd = lv_dropdown_create(row);
        lv_dropdown_set_options(dd, "shabbat\noled-warm\ncyber\nemerald\ncrimson-night\neink\ndynamic");
        lv_dropdown_set_selected(dd, g_cfg.preset_index);
        lv_obj_set_width(dd, 165);
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.preset_index = lv_dropdown_get_selected(lv_event_get_target(e));
            ui_request_clock_redraw();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Pure Black OLED");
        lv_obj_t *sw = lv_switch_create(row);
        if (g_cfg.pure_black_oled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.pure_black_oled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
            ui_request_clock_redraw();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Font Sizing");
        lv_obj_t *dd = lv_dropdown_create(row);
        lv_dropdown_set_options(dd, "Static (60px)\nFit Phrase");
        lv_dropdown_set_selected(dd, g_cfg.sizing_static ? 0 : 1);
        lv_obj_set_width(dd, 155);
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.sizing_static = (lv_dropdown_get_selected(lv_event_get_target(e)) == 0);
            ui_request_clock_redraw();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Alignment");
        lv_obj_t *dd = lv_dropdown_create(row);
        lv_dropdown_set_options(dd, "Center\nRight (RTL)");
        lv_dropdown_set_selected(dd, g_cfg.align_center ? 0 : 1);
        lv_obj_set_width(dd, 155);
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.align_center = (lv_dropdown_get_selected(lv_event_get_target(e)) == 0);
            ui_request_clock_redraw();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Nikkud Vowels");
        lv_obj_t *dd = lv_dropdown_create(row);
        lv_dropdown_set_options(dd, "Color-Coded\nFull Same\nNone");
        lv_dropdown_set_selected(dd, g_cfg.nikkud_mode);
        lv_obj_set_width(dd, 155);
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.nikkud_mode = lv_dropdown_get_selected(lv_event_get_target(e));
            ui_request_clock_redraw();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Seconds Dot");
        lv_obj_t *sw = lv_switch_create(row);
        if (g_cfg.show_seconds) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.show_seconds = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
            ui_request_clock_redraw();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Brightness");
        lv_obj_t *sl = lv_slider_create(row);
        lv_slider_set_range(sl, 10, 100);
        lv_slider_set_value(sl, g_cfg.brightness, LV_ANIM_OFF);
        lv_obj_set_width(sl, 160);
        lv_obj_add_event_cb(sl, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.brightness = lv_slider_get_value(lv_event_get_target(e));
            hal_display_set_brightness(g_cfg.brightness);
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Screen Sleep");
        lv_obj_t *dd = lv_dropdown_create(row);
        lv_dropdown_set_options(dd, "30 sec\n1 min\n2 min\n5 min\nAlways On");
        lv_dropdown_set_selected(dd, g_cfg.timeout_idx);
        lv_obj_set_width(dd, 145);
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.timeout_idx = lv_dropdown_get_selected(lv_event_get_target(e));
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Stay On (USB)");
        lv_obj_t *sw = lv_switch_create(row);
        if (g_cfg.always_on_usb) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.always_on_usb = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // --- Section: Hebrew Voice & Chime ---
    make_section_label(cont, "Hebrew Voice & Chime");

    {
        lv_obj_t *row = make_row(cont, "Voice");
        lv_obj_t *dd = lv_dropdown_create(row);
        lv_dropdown_set_options(dd, "Hila (Female)\nAvri (Male)\nChime");
        lv_dropdown_set_selected(dd, g_cfg.voice_id);
        lv_obj_set_width(dd, 155);
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.voice_id = lv_dropdown_get_selected(lv_event_get_target(e));
            if (g_cfg.voice_id == 2) {
                hebrew_clock_speak_current_time();
            }
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Volume");
        lv_obj_t *sl = lv_slider_create(row);
        lv_slider_set_range(sl, 20, 100);
        lv_slider_set_value(sl, g_cfg.volume, LV_ANIM_OFF);
        lv_obj_set_width(sl, 160);
        lv_obj_add_event_cb(sl, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.volume = lv_slider_get_value(lv_event_get_target(e));
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Speak Hourly");
        lv_obj_t *sw = lv_switch_create(row);
        if (g_cfg.hourly_enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.hourly_enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    static const char *HOUR_OPTS =
        "00:00 (12 AM)\n01:00 (1 AM)\n02:00 (2 AM)\n03:00 (3 AM)\n04:00 (4 AM)\n05:00 (5 AM)\n"
        "06:00 (6 AM)\n07:00 (7 AM)\n08:00 (8 AM)\n09:00 (9 AM)\n10:00 (10 AM)\n11:00 (11 AM)\n"
        "12:00 (12 PM)\n13:00 (1 PM)\n14:00 (2 PM)\n15:00 (3 PM)\n16:00 (4 PM)\n17:00 (5 PM)\n"
        "18:00 (6 PM)\n19:00 (7 PM)\n20:00 (8 PM)\n21:00 (9 PM)\n22:00 (10 PM)\n23:00 (11 PM)";

    {
        lv_obj_t *row = make_row(cont, "Start Hour");
        lv_obj_t *dd = lv_dropdown_create(row);
        lv_dropdown_set_options(dd, HOUR_OPTS);
        lv_dropdown_set_selected(dd, g_cfg.hourly_start % 24);
        lv_obj_set_width(dd, 155);
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.hourly_start = lv_dropdown_get_selected(lv_event_get_target(e));
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "End Hour");
        lv_obj_t *dd = lv_dropdown_create(row);
        lv_dropdown_set_options(dd, HOUR_OPTS);
        lv_dropdown_set_selected(dd, g_cfg.hourly_end % 24);
        lv_obj_set_width(dd, 155);
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.hourly_end = lv_dropdown_get_selected(lv_event_get_target(e));
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *btn_test = lv_btn_create(cont);
        lv_obj_set_size(btn_test, 260, 44);
        lv_obj_set_style_bg_color(btn_test, lv_color_hex(0x2563EB), 0);
        lv_obj_set_style_radius(btn_test, 12, 0);
        lv_obj_t *lbl = lv_label_create(btn_test);
        lv_label_set_text(lbl, LV_SYMBOL_AUDIO "  Speak Current Time Now");
        lv_obj_center(lbl);
        lv_obj_add_event_cb(btn_test, [](lv_event_t *e) {
            ui_wake_screen();
            hebrew_clock_speak_current_time();
        }, LV_EVENT_CLICKED, NULL);
    }

    // --- Section: Timezone & Sync ---
    make_section_label(cont, "Timezone & Sync");
    {
        lv_obj_t *row = make_row(cont, "Timezone");
        lv_obj_t *dd = lv_dropdown_create(row);
        char tz_opts[768] = "";
        for (int i = 0; i < TZ_COUNT; i++) {
            if (i > 0) strcat(tz_opts, "\n");
            strcat(tz_opts, TZ_TABLE[i].iana);
        }
        lv_dropdown_set_options(dd, tz_opts);
        lv_dropdown_set_selected(dd, g_cfg.tz_index);
        lv_obj_set_width(dd, 195);
        lv_obj_add_event_cb(dd, [](lv_event_t *e) {
            ui_wake_screen();
            g_cfg.tz_index = lv_dropdown_get_selected(lv_event_get_target(e));
            app_config_apply_timezone();
            ui_request_clock_redraw();
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    // --- Section: Wi-Fi Network ---
    make_section_label(cont, "Wi-Fi Network");

    s_lbl_wifi_status = lv_label_create(cont);
    ui_refresh_wifi_status_label();
    lv_obj_set_style_text_color(s_lbl_wifi_status, lv_color_hex(0x94A3B8), 0);

    {
        lv_obj_t *row = make_row(cont, "Network");
        s_dd_wifi_ssid = lv_dropdown_create(row);
        lv_dropdown_set_options(s_dd_wifi_ssid, g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "Tap Scan Below");
        lv_obj_set_width(s_dd_wifi_ssid, 185);
        lv_obj_add_event_cb(s_dd_wifi_ssid, [](lv_event_t *e) {
            ui_wake_screen();
            char buf[64] = "";
            lv_dropdown_get_selected_str(lv_event_get_target(e), buf, sizeof(buf));
            if (buf[0] != '\0' && strcmp(buf, "Tap Scan Below") != 0) {
                strncpy(g_cfg.wifi_ssid, buf, sizeof(g_cfg.wifi_ssid) - 1);
            }
        }, LV_EVENT_VALUE_CHANGED, NULL);
    }

    {
        lv_obj_t *row = make_row(cont, "Password");
        s_ta_wifi_pass = lv_textarea_create(row);
        lv_textarea_set_one_line(s_ta_wifi_pass, true);
        lv_textarea_set_password_mode(s_ta_wifi_pass, true);
        lv_textarea_set_text(s_ta_wifi_pass, g_cfg.wifi_pass);
        lv_obj_set_width(s_ta_wifi_pass, 185);
        lv_obj_add_event_cb(s_ta_wifi_pass, [](lv_event_t *e) {
            ui_wake_screen();
            lv_event_code_t code = lv_event_get_code(e);
            if (code == LV_EVENT_FOCUSED && s_kb_wifi) {
                lv_keyboard_set_textarea(s_kb_wifi, s_ta_wifi_pass);
                lv_obj_clear_flag(s_kb_wifi, LV_OBJ_FLAG_HIDDEN);
            } else if (code == LV_EVENT_DEFOCUSED && s_kb_wifi) {
                lv_obj_add_flag(s_kb_wifi, LV_OBJ_FLAG_HIDDEN);
            }
        }, LV_EVENT_ALL, NULL);
    }

    {
        lv_obj_t *row_btns = lv_obj_create(cont);
        lv_obj_set_size(row_btns, 320, 48);
        lv_obj_set_style_bg_opa(row_btns, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row_btns, 0, 0);
        lv_obj_set_flex_flow(row_btns, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row_btns, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *btn_scan = lv_btn_create(row_btns);
        lv_obj_set_size(btn_scan, 135, 42);
        lv_obj_set_style_bg_color(btn_scan, lv_color_hex(0x334155), 0);
        lv_obj_t *l1 = lv_label_create(btn_scan);
        lv_label_set_text(l1, LV_SYMBOL_REFRESH " Scan Wi-Fi");
        lv_obj_center(l1);
        lv_obj_add_event_cb(btn_scan, [](lv_event_t *e) {
            ui_wake_screen();
            s_status_hold_until_us = esp_timer_get_time() + 10000000LL;
            if (s_lbl_wifi_status) lv_label_set_text(s_lbl_wifi_status, "Scanning Wi-Fi...");
            net_time_start_scan();
        }, LV_EVENT_CLICKED, NULL);

        lv_obj_t *btn_conn = lv_btn_create(row_btns);
        lv_obj_set_size(btn_conn, 135, 42);
        lv_obj_set_style_bg_color(btn_conn, lv_color_hex(0x10B981), 0);
        lv_obj_t *l2 = lv_label_create(btn_conn);
        lv_label_set_text(l2, LV_SYMBOL_WIFI " Connect");
        lv_obj_center(l2);
        lv_obj_add_event_cb(btn_conn, [](lv_event_t *e) {
            ui_wake_screen();
            char ssid_buf[64] = "";
            if (s_dd_wifi_ssid) {
                lv_dropdown_get_selected_str(s_dd_wifi_ssid, ssid_buf, sizeof(ssid_buf));
                if (ssid_buf[0] != '\0' && strcmp(ssid_buf, "Tap Scan Below") != 0) {
                    strncpy(g_cfg.wifi_ssid, ssid_buf, sizeof(g_cfg.wifi_ssid) - 1);
                }
            }
            if (s_ta_wifi_pass) {
                strncpy(g_cfg.wifi_pass, lv_textarea_get_text(s_ta_wifi_pass), sizeof(g_cfg.wifi_pass) - 1);
            }
            app_config_save();
            if (s_lbl_wifi_status) {
                lv_label_set_text_fmt(s_lbl_wifi_status, "Connecting to %s...", g_cfg.wifi_ssid);
            }
            net_time_reconnect();
        }, LV_EVENT_CLICKED, NULL);
    }

    s_kb_wifi = lv_keyboard_create(s_scr_settings);
    lv_obj_set_size(s_kb_wifi, 420, 190);
    lv_obj_align(s_kb_wifi, LV_ALIGN_BOTTOM_MID, 0, -25);
    lv_obj_add_flag(s_kb_wifi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_kb_wifi, [](lv_event_t *e) {
        lv_event_code_t code = lv_event_get_code(e);
        if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
            lv_obj_add_flag(s_kb_wifi, LV_OBJ_FLAG_HIDDEN);
        }
    }, LV_EVENT_ALL, NULL);

    clock_renderer_render_now();
    if (s_cont_settings) {
        lv_obj_scroll_to_y(s_cont_settings, 0, LV_ANIM_OFF);
    }
    lv_scr_load(s_scr_clock);
}
