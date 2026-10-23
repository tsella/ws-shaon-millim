#include "clock_renderer.h"
#include "app_config.h"
#include "hal_board.h"
#include "hebrew_clock.h"
#include "net_time.h"
#include "ui_screens.h"

#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

static uint16_t   *s_clock_fb     = NULL;
static lv_obj_t   *s_canvas_clock = NULL;
static lv_timer_t *s_anim_timer   = NULL;

// Smooth ease-out / ease-in transition state when clock minute changes
static int     s_prev_hour     = -1;
static int     s_prev_min      = -1;
static int     s_cur_hour      = -1;
static int     s_cur_min       = -1;
static int64_t s_anim_start_us = 0;
static bool    s_anim_active   = false;

// Cached background/nikkud colors and dirty rectangle for the 60-second perimeter dot
static uint16_t  s_last_bg565     = 0;
static rgb_t     s_last_bg_rgb    = {0, 0, 0};
static rgb_t     s_last_nikk_rgb  = {0xF5, 0x9E, 0x0B};
static bool      s_prev_dot_valid = false;
static lv_area_t s_prev_dot_area  = {0, 0, 0, 0};

static constexpr float   SEC_DOT_RADIUS       = 3.5f; // Matches 60px Frank Ruhl Libre nikkud dot size
static constexpr float   SEC_ORBIT_RADIUS     = 233.0f - 2.0f - SEC_DOT_RADIUS; // 2px from outer circular edge
static constexpr int64_t CROSSFADE_DURATION_US = 600000LL; // 600 ms smooth crossfade intersecting at 50% at t = 0.5

static inline uint8_t clamp_u8(int v) {
    return (v < 0) ? 0 : ((v > 255) ? 255 : (uint8_t)v);
}

static inline float clamp_f(float v, float lo, float hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static inline rgb_t scale_rgb(rgb_t c, float factor) {
    return {
        clamp_u8((int)roundf(c.r * factor)),
        clamp_u8((int)roundf(c.g * factor)),
        clamp_u8((int)roundf(c.b * factor)),
    };
}

static inline uint16_t rgb_to_565(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
#if LV_COLOR_16_SWAP
    return (uint16_t)((v >> 8) | (v << 8));
#else
    return v;
#endif
}

static inline void blend_pixel_fb(uint16_t *fb, int x, int y, rgb_t bg_hint, rgb_t fg, uint8_t alpha) {
    (void)bg_hint;
    if (alpha == 0 || x < 0 || x >= 466 || y < 0 || y >= 466) return;
    if (alpha == 255) {
        fb[y * 466 + x] = rgb_to_565(fg.r, fg.g, fg.b);
        return;
    }
    uint16_t raw = fb[y * 466 + x];
#if LV_COLOR_16_SWAP
    raw = (uint16_t)((raw >> 8) | (raw << 8));
#endif
    uint8_t br = (raw >> 8) & 0xF8; br |= (br >> 5);
    uint8_t bg = (raw >> 3) & 0xFC; bg |= (bg >> 6);
    uint8_t bb = (raw << 3) & 0xF8; bb |= (bb >> 5);

    uint8_t r = (uint8_t)((fg.r * alpha + br * (255 - alpha)) / 255);
    uint8_t g = (uint8_t)((fg.g * alpha + bg * (255 - alpha)) / 255);
    uint8_t b = (uint8_t)((fg.b * alpha + bb * (255 - alpha)) / 255);
    fb[y * 466 + x] = rgb_to_565(r, g, b);
}

static void blit_rle_mask(uint16_t *fb, int dst_x, int dst_y, int w, int h,
                          uint32_t rle_offset, rgb_t bg, rgb_t fg, uint8_t master_alpha) {
    if (master_alpha == 0) return;
    const uint8_t *p = &heb_glyph_bitmap_blob[rle_offset];
    int total = w * h;
    int idx = 0;
    while (idx < total) {
        uint8_t b = *p++;
        if (b == 0x00) {
            uint8_t run = *p++;
            idx += run;
        } else if (b == 0xFF) {
            uint8_t run = *p++;
            if (master_alpha == 255) {
                uint16_t c565 = rgb_to_565(fg.r, fg.g, fg.b);
                for (uint8_t k = 0; k < run && idx < total; k++, idx++) {
                    int py = dst_y + (idx / w);
                    int px = dst_x + (idx % w);
                    if (px >= 0 && px < 466 && py >= 0 && py < 466) {
                        fb[py * 466 + px] = c565;
                    }
                }
            } else {
                for (uint8_t k = 0; k < run && idx < total; k++, idx++) {
                    int py = dst_y + (idx / w);
                    int px = dst_x + (idx % w);
                    blend_pixel_fb(fb, px, py, bg, fg, master_alpha);
                }
            }
        } else {
            int py = dst_y + (idx / w);
            int px = dst_x + (idx % w);
            uint8_t eff_a = (master_alpha == 255) ? b : (uint8_t)(((uint16_t)b * master_alpha) / 255);
            blend_pixel_fb(fb, px, py, bg, fg, eff_a);
            idx++;
        }
    }
}

typedef struct {
    phrase_tok_t toks[6];
    int count;
    int width_px;
    bool is_period_line;
} wrapped_line_t;

static inline int effective_space_width(int size_idx) {
    // Add natural glyph left+right sidebearings (~px_size/5) stripped by tight ink-box cropping
    return (int)heb_font_space_widths[size_idx] + ((int)heb_font_px_sizes[size_idx] / 5);
}

static int wrap_phrase_for_size(const phrase_tok_t *tokens, int n_tokens, int size_idx,
                                int max_w, wrapped_line_t *out_lines) {
    int line_cnt = 0;
    int sp_w = effective_space_width(size_idx);
    wrapped_line_t cur = {};

    for (int i = 0; i < n_tokens; i++) {
        heb_token_id_t tid = tokens[i].id;
        int w_word = heb_word_table[size_idx][tid].width;
        bool role_break = (cur.count > 0) && (cur.is_period_line != tokens[i].is_period);
        int trial_w = (cur.count == 0) ? w_word : (cur.width_px + sp_w + w_word);

        if (cur.count > 0 && (role_break || trial_w > max_w)) {
            out_lines[line_cnt++] = cur;
            memset(&cur, 0, sizeof(cur));
            cur.toks[0] = tokens[i];
            cur.count = 1;
            cur.width_px = w_word;
            cur.is_period_line = tokens[i].is_period;
        } else {
            if (cur.count == 0) cur.is_period_line = tokens[i].is_period;
            cur.toks[cur.count++] = tokens[i];
            cur.width_px = trial_w;
        }
    }
    if (cur.count > 0) {
        out_lines[line_cnt++] = cur;
    }
    return line_cnt;
}

static void draw_filled_circle_aa(uint16_t *fb, float cx, float cy, float r, rgb_t bg, rgb_t col, uint8_t master_alpha = 255) {
    if (master_alpha == 0) return;
    int x0 = (int)floorf(cx - r - 1.5f);
    int x1 = (int)ceilf(cx + r + 1.5f);
    int y0 = (int)floorf(cy - r - 1.5f);
    int y1 = (int)ceilf(cy + r + 1.5f);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
            float cov = r + 0.5f - d;
            if (cov > 0.0f) {
                uint8_t a = (cov >= 1.0f) ? 255 : (uint8_t)(cov * 255.0f);
                if (master_alpha < 255) {
                    a = (uint8_t)(((uint16_t)a * master_alpha) / 255);
                }
                blend_pixel_fb(fb, x, y, bg, col, a);
            }
        }
    }
}

static bool sec_dot_intersects_hud(float sx, float sy, float r_dot) {
    // 1. Wi-Fi icon bounding box (x: 191..230, y: 18..41)
    if (sx + r_dot >= 190.0f && sx - r_dot <= 231.0f &&
        sy + r_dot >= 17.0f  && sy - r_dot <= 42.0f) {
        return true;
    }
    // 2. Battery icon bounding box (x: 239..280, y: 22..42)
    if (sx + r_dot >= 238.0f && sx - r_dot <= 281.0f &&
        sy + r_dot >= 21.0f  && sy - r_dot <= 43.0f) {
        return true;
    }
    // 3. Settings Cog bounding box (x: 220..246, y: 421..447)
    if (sx + r_dot >= 219.0f && sx - r_dot <= 247.0f &&
        sy + r_dot >= 420.0f && sy - r_dot <= 448.0f) {
        return true;
    }
    return false;
}

static void compute_sec_dot_state(float *out_sx, float *out_sy, uint8_t *out_alpha) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_loc;
    localtime_r(&tv.tv_sec, &tm_loc);

    int sec = tm_loc.tm_sec % 60;
    float ang = (float)sec * (2.0f * (float)M_PI / 60.0f);
    *out_sx = 233.0f + SEC_ORBIT_RADIUS * sinf(ang);
    *out_sy = 233.0f - SEC_ORBIT_RADIUS * cosf(ang);

    // Sub-second phase f in [0.0, 1.0): ease-in -> hold -> ease-out before easing in at next second location
    float f = clamp_f((float)tv.tv_usec / 1000000.0f, 0.0f, 1.0f);
    float env = 0.0f;
    if (f < 0.42f) {
        float u = f / 0.42f;
        env = u * u * (3.0f - 2.0f * u); // Smooth ease-in
    } else if (f < 0.56f) {
        env = 1.0f; // Peak hold
    } else if (f < 0.96f) {
        float u = (f - 0.56f) / 0.40f;
        env = 1.0f - u * u * (3.0f - 2.0f * u); // Smooth ease-out to 0 before next second
    } else {
        env = 0.0f;
    }
    *out_alpha = clamp_u8((int)roundf(env * 255.0f));
}

static void draw_seconds_dot_on_fb(bool invalidate_dirty_rect) {
    if (!s_clock_fb || !s_canvas_clock) return;

    // Erase previous seconds dot patch if valid
    if (s_prev_dot_valid) {
        for (int y = s_prev_dot_area.y1; y <= s_prev_dot_area.y2; y++) {
            for (int x = s_prev_dot_area.x1; x <= s_prev_dot_area.x2; x++) {
                if (x >= 0 && x < 466 && y >= 0 && y < 466) {
                    s_clock_fb[y * 466 + x] = s_last_bg565;
                }
            }
        }
        if (invalidate_dirty_rect) {
            lv_obj_invalidate_area(s_canvas_clock, &s_prev_dot_area);
        }
        s_prev_dot_valid = false;
    }

    if (!g_cfg.show_seconds) return;

    float sx = 0.0f, sy = 0.0f;
    uint8_t dot_alpha = 0;
    compute_sec_dot_state(&sx, &sy, &dot_alpha);

    if (dot_alpha == 0 || sec_dot_intersects_hud(sx, sy, SEC_DOT_RADIUS)) {
        return;
    }

    draw_filled_circle_aa(s_clock_fb, sx, sy, SEC_DOT_RADIUS, s_last_bg_rgb, s_last_nikk_rgb, dot_alpha);

    s_prev_dot_area.x1 = (lv_coord_t)fmaxf(0.0f, floorf(sx - SEC_DOT_RADIUS - 2.0f));
    s_prev_dot_area.y1 = (lv_coord_t)fmaxf(0.0f, floorf(sy - SEC_DOT_RADIUS - 2.0f));
    s_prev_dot_area.x2 = (lv_coord_t)fminf(465.0f, ceilf(sx + SEC_DOT_RADIUS + 2.0f));
    s_prev_dot_area.y2 = (lv_coord_t)fminf(465.0f, ceilf(sy + SEC_DOT_RADIUS + 2.0f));
    s_prev_dot_valid   = true;

    if (invalidate_dirty_rect) {
        lv_obj_invalidate_area(s_canvas_clock, &s_prev_dot_area);
    }
}

// Signed Distance Field (SDF) for a 2D box with rounded corners
static inline float sd_rounded_box(float px, float py, float cx, float cy, float half_w, float half_h, float r) {
    float qx = fabsf(px - cx) - (half_w - r);
    float qy = fabsf(py - cy) - (half_h - r);
    float ox = fmaxf(qx, 0.0f);
    float oy = fmaxf(qy, 0.0f);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
}

// Signed Distance Field (SDF) to a 2D line segment (ax, ay) -> (bx, by)
static inline float sd_segment(float px, float py, float ax, float ay, float bx, float by) {
    float pax = px - ax, pay = py - ay;
    float bax = bx - ax, bay = by - ay;
    float h = clamp_f((pax * bax + pay * bay) / (bax * bax + bay * bay), 0.0f, 1.0f);
    float dx = pax - bax * h;
    float dy = pay - bay * h;
    return sqrtf(dx * dx + dy * dy);
}

// Signed Distance Field (SDF) for a crisp 'Z' glyph centered at (cx, cy) with half-width hw and half-height hh
static inline float sd_letter_z(float px, float py, float cx, float cy, float hw, float hh) {
    float d_top  = sd_segment(px, py, cx - hw, cy - hh, cx + hw, cy - hh);
    float d_diag = sd_segment(px, py, cx + hw, cy - hh, cx - hw, cy + hh);
    float d_bot  = sd_segment(px, py, cx - hw, cy + hh, cx + hw, cy + hh);
    return fminf(d_top, fminf(d_diag, d_bot));
}

// Smooth minimum (polynomial smin) for organic root fillets on the Settings gear teeth
static inline float smin_f(float a, float b, float k) {
    float h = clamp_f(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return b * (1.0f - h) + a * h - k * h * (1.0f - h);
}

// Exact Signed Distance Field (SDF) for a upward-facing symmetric circular arc with rounded end-caps
static inline float sd_wifi_arc(float px, float py, float cx, float cy, float radius, float half_thick) {
    // sin(38 deg), cos(38 deg) for a 76-degree symmetric fan
    const float sc_x = 0.6156615f;
    const float sc_y = 0.7880108f;
    float qx = fabsf(px - cx);
    float qy = cy - py; // positive upward
    float d_center;
    if (sc_y * qx > sc_x * qy) {
        float dx = qx - radius * sc_x;
        float dy = qy - radius * sc_y;
        d_center = sqrtf(dx * dx + dy * dy);
    } else {
        d_center = fabsf(sqrtf(qx * qx + qy * qy) - radius);
    }
    return d_center - half_thick;
}

static void draw_hud_icons(uint16_t *fb, rgb_t bg) {
    // 1. Sub-pixel Anti-Aliased Color-Graded Battery Icon at Top-Right of 12 o'clock
    // Outer body center (258.0, 32.0), dimensions 34.0 x 16.0 (half_w=17.0, half_h=8.0, r=4.2)
    const float bcx = 258.0f, bcy = 32.0f;
    const float out_hw = 17.0f, out_hh = 8.0f, out_r = 4.2f;
    const float in_hw  = 15.2f, in_hh  = 6.2f, in_r  = 2.5f;
    rgb_t frame_col = { 185, 200, 225 };

    // A. Smooth anti-aliased rounded battery shell + positive terminal cap
    for (int y = 22; y <= 42; y++) {
        for (int x = 239; x <= 280; x++) {
            float d_out = sd_rounded_box((float)x, (float)y, bcx, bcy, out_hw, out_hh, out_r);
            float d_in  = sd_rounded_box((float)x, (float)y, bcx, bcy, in_hw,  in_hh,  in_r);
            float d_ring = fmaxf(d_out, -d_in);

            // Smooth rounded positive terminal nub on the right edge
            float d_nub = sd_rounded_box((float)x, (float)y, bcx + out_hw + 1.2f, bcy, 1.6f, 3.6f, 1.3f);
            d_nub = fmaxf(d_nub, -d_out - 0.2f); // Attach cleanly outside the right wall

            float d = fminf(d_ring, d_nub);
            float cov = clamp_f(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_pixel_fb(fb, x, y, bg, frame_col, (uint8_t)(cov * 230.0f));
            }
        }
    }

    // B. Smooth anti-aliased inner fill bar with subtle vertical sheen
    rgb_t batt_col = { 0x22, 0xC5, 0x5E }; // Emerald Green (>50%)
    if (g_batt_percent < 20) {
        batt_col = (rgb_t){ 0xEF, 0x44, 0x44 }; // Crimson Red (<20%)
    } else if (g_batt_percent <= 50) {
        batt_col = (rgb_t){ 0xF5, 0x9E, 0x0B }; // Amber (20..50%)
    }

    if (g_batt_percent > 0) {
        const float max_fill_w = 26.8f; // spans x = 244.6 .. 271.4
        float fill_w = (max_fill_w * clamp_f((float)g_batt_percent, 0.0f, 100.0f)) / 100.0f;
        if (fill_w < 3.4f) fill_w = 3.4f;
        float fill_hw = fill_w * 0.5f;
        float fill_hh = 4.4f; // height 8.8 px
        float fill_cx = (bcx - 13.4f) + fill_hw;
        float fill_r  = fminf(1.8f, fill_hw);

        rgb_t batt_hi = scale_rgb(batt_col, 1.14f);
        for (int y = 26; y <= 38; y++) {
            rgb_t row_col = (y < (int)bcy) ? batt_hi : batt_col;
            for (int x = 243; x <= 273; x++) {
                float d_fill = sd_rounded_box((float)x, (float)y, fill_cx, bcy, fill_hw, fill_hh, fill_r);
                float cov = clamp_f(0.5f - d_fill, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    blend_pixel_fb(fb, x, y, bg, row_col, (uint8_t)(cov * 248.0f));
                }
            }
        }
    }

    // C. Anti-aliased Charging Lightning Bolt when charging on USB
    if (g_is_charging) {
        rgb_t bolt_col  = { 255, 255, 255 };
        rgb_t halo_col  = { 12,  18,  28  };
        for (int y = 25; y <= 39; y++) {
            for (int x = 252; x <= 264; x++) {
                float d1 = sd_segment((float)x, (float)y, bcx + 1.5f, bcy - 4.5f, bcx - 2.0f, bcy + 0.3f);
                float d2 = sd_segment((float)x, (float)y, bcx - 2.0f, bcy + 0.3f, bcx + 1.8f, bcy - 0.3f);
                float d3 = sd_segment((float)x, (float)y, bcx + 1.8f, bcy - 0.3f, bcx - 1.5f, bcy + 4.5f);
                float d_bolt = fminf(d1, fminf(d2, d3));
                float cov_halo = clamp_f(1.65f - d_bolt, 0.0f, 1.0f);
                if (cov_halo > 0.0f) {
                    blend_pixel_fb(fb, x, y, bg, halo_col, (uint8_t)(cov_halo * 165.0f));
                }
                float cov_core = clamp_f(0.95f - d_bolt, 0.0f, 1.0f);
                if (cov_core > 0.0f) {
                    blend_pixel_fb(fb, x, y, bg, bolt_col, (uint8_t)(cov_core * 255.0f));
                }
            }
        }
    }

    // 2. Sub-pixel Anti-Aliased Wi-Fi Signal Symbol at Top-Left of 12 o'clock (apex at x=208, y=38.5)
    const float wx = 208.0f, wy = 38.5f;
    bool wifi_ok = g_wifi_connected || g_wifi_sleeping;
    int bars = 0;
    if (g_wifi_connected) {
        if (g_wifi_rssi >= -60) bars = 3;
        else if (g_wifi_rssi >= -72) bars = 2;
        else bars = 1;
    } else if (g_wifi_sleeping) {
        bars = 3;
    }
    rgb_t wifi_active = g_wifi_sleeping ? rgb_t{ 92, 108, 134 } : rgb_t{ 185, 200, 225 };
    rgb_t wifi_dim    = { 65,  76,  96  };
    rgb_t wifi_off    = { 239, 68,  68  };
    rgb_t wifi_zzz    = { 125, 211, 252 }; // Sky-blue 'zzz' sleep badge
    rgb_t halo_col    = { 9,   13,  26  };

    // Base dot at (wx, wy)
    draw_filled_circle_aa(fb, wx, wy, 2.0f, bg, wifi_ok ? wifi_active : wifi_dim);

    // Three concentric rounded-cap SDF arcs (radii 6.0, 10.3, 14.6, half-thickness 1.15px)
    const float radii[3] = { 6.0f, 10.3f, 14.6f };
    for (int b_idx = 0; b_idx < 3; b_idx++) {
        float r_arc = radii[b_idx];
        rgb_t c_arc = (!wifi_ok) ? wifi_dim : ((b_idx < bars) ? wifi_active : wifi_dim);
        int x0 = (int)floorf(wx - r_arc - 3.0f);
        int x1 = (int)ceilf(wx + r_arc + 3.0f);
        int y0 = (int)floorf(wy - r_arc - 3.0f);
        int y1 = (int)ceilf(wy + 2.0f);
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float d_arc = sd_wifi_arc((float)x, (float)y, wx, wy, r_arc, 1.12f);
                float cov = clamp_f(0.5f - d_arc, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    blend_pixel_fb(fb, x, y, bg, c_arc, (uint8_t)(cov * 238.0f));
                }
            }
        }
    }

    // Smooth anti-aliased ascending 'zzz' sleep badge when Wi-Fi radio is idling/sleeping between 6h NTP syncs
    if (g_wifi_sleeping) {
        for (int y = 17; y <= 40; y++) {
            for (int x = 208; x <= 231; x++) {
                float px = (float)x, py = (float)y;
                float dz1 = sd_letter_z(px, py, 212.5f, 35.2f, 1.8f, 1.8f);
                float dz2 = sd_letter_z(px, py, 218.0f, 29.2f, 2.4f, 2.4f);
                float dz3 = sd_letter_z(px, py, 224.8f, 22.2f, 3.1f, 3.1f);
                float d_zzz = fminf(dz1, fminf(dz2, dz3));

                float cov_halo = clamp_f(2.15f - d_zzz, 0.0f, 1.0f);
                if (cov_halo > 0.0f) {
                    blend_pixel_fb(fb, x, y, bg, halo_col, (uint8_t)(cov_halo * 220.0f));
                }
                float cov_core = clamp_f(0.88f - d_zzz, 0.0f, 1.0f);
                if (cov_core > 0.0f) {
                    blend_pixel_fb(fb, x, y, bg, wifi_zzz, (uint8_t)(cov_core * 255.0f));
                }
            }
        }
    } else if (!wifi_ok) {
        // Smooth anti-aliased slash with contrast halo when disconnected
        for (int y = (int)wy - 17; y <= (int)wy + 3; y++) {
            for (int x = (int)wx - 12; x <= (int)wx + 12; x++) {
                float d_slash = sd_segment((float)x, (float)y, wx - 7.8f, wy - 14.2f, wx + 7.8f, wy + 0.4f);
                float cov_halo = clamp_f(2.0f - d_slash, 0.0f, 1.0f);
                if (cov_halo > 0.0f) {
                    blend_pixel_fb(fb, x, y, bg, halo_col, (uint8_t)(cov_halo * 190.0f));
                }
                float cov_core = clamp_f(0.95f - d_slash, 0.0f, 1.0f);
                if (cov_core > 0.0f) {
                    blend_pixel_fb(fb, x, y, bg, wifi_off, (uint8_t)(cov_core * 245.0f));
                }
            }
        }
    }

    // 3. Sub-pixel Anti-Aliased Mechanical Settings Cog at 6 o'clock (center x=233, y=434)
    const float gx = 233.0f, gy = 434.0f;
    rgb_t cog_hi   = { 195, 210, 235 };
    rgb_t cog_base = { 160, 176, 202 };
    const float cos45 = 0.70710678f;

    for (int y = 421; y <= 447; y++) {
        rgb_t row_col = (y < (int)gy) ? cog_hi : cog_base;
        for (int x = 220; x <= 246; x++) {
            float dx = (float)x - gx;
            float dy = (float)y - gy;
            float r  = sqrtf(dx * dx + dy * dy);

            // 4 rotated rounded bars (0, 90, +45, -45 deg) forming 8 symmetric tapered teeth
            float d_t0 = sd_rounded_box(dx, dy, 0.0f, 0.0f, 10.2f, 2.05f, 1.05f);
            float d_t1 = sd_rounded_box(dy, dx, 0.0f, 0.0f, 10.2f, 2.05f, 1.05f);
            float rx1  = (dx + dy) * cos45;
            float ry1  = (dy - dx) * cos45;
            float d_t2 = sd_rounded_box(rx1, ry1, 0.0f, 0.0f, 10.2f, 2.05f, 1.05f);
            float d_t3 = sd_rounded_box(ry1, rx1, 0.0f, 0.0f, 10.2f, 2.05f, 1.05f);
            float d_teeth = fminf(fminf(d_t0, d_t1), fminf(d_t2, d_t3));

            // Smoothly fillet the 8 teeth into the main wheel body (r = 7.7) and cut out the center bore (r = 3.8)
            float d_wheel = r - 7.7f;
            float d_outer = smin_f(d_wheel, d_teeth, 0.85f);
            float d_bore  = r - 3.8f;
            float d_gear  = fmaxf(d_outer, -d_bore);

            float cov = clamp_f(0.5f - d_gear, 0.0f, 1.0f);
            if (cov > 0.0f) {
                // Subtle inner rim shading around the axle bore
                float rim_diff = fabsf(r - 4.7f);
                rgb_t px_col = (rim_diff < 0.9f) ? cog_hi : row_col;
                blend_pixel_fb(fb, x, y, bg, px_col, (uint8_t)(cov * 230.0f));
            }
        }
    }
}

static preset_spec_t resolve_preset_for_hour(int hour) {
    preset_spec_t p = PRESETS[g_cfg.preset_index % PRESET_COUNT];
    if (strcasecmp(p.name, "dynamic") == 0) {
        if (hour >= 5 && hour < 11) {
            p = { "dyn", {0x04,0x08,0x0E}, {0xF0,0xF9,0xFF}, {0x38,0xBD,0xF8}, {0xFB,0xBF,0x24} };
        } else if (hour >= 11 && hour < 17) {
            p = { "dyn", {0x05,0x06,0x08}, {0xFF,0xF8,0xEB}, {0xF5,0x9E,0x0B}, {0x94,0xA3,0xB8} };
        } else if (hour >= 17 && hour < 22) {
            p = { "dyn", {0x0A,0x05,0x08}, {0xFF,0xED,0xD5}, {0xFB,0x71,0x85}, {0xF9,0x73,0x16} };
        } else {
            p = { "dyn", {0x05,0x02,0x03}, {0xFC,0xA5,0xA5}, {0xDC,0x26,0x26}, {0x7F,0x1D,0x1D} };
        }
    }
    return p;
}

static void composite_phrase_layer(int hour, int minute, rgb_t bg_rgb, const preset_spec_t &p, uint8_t word_alpha) {
    if (word_alpha == 0) return;

    rgb_t letter_rgb = p.letter;
    rgb_t nikkud_rgb = p.nikkud;

    float p_fac = 0.38f, n_fac = 0.45f;
    if (hour >= 5 && hour < 12)       { p_fac = 1.40f; n_fac = 1.25f; }
    else if (hour >= 12 && hour < 17) { p_fac = 1.15f; n_fac = 1.00f; }
    else if (hour >= 17 && hour < 21) { p_fac = 0.52f; n_fac = 0.58f; }
    rgb_t period_rgb      = scale_rgb(p.period, p_fac);
    rgb_t period_nikk_rgb = scale_rgb(p.nikkud, n_fac);

    phrase_tok_t tokens[10];
    int n_toks = hebrew_clock_build_phrase(hour, minute, tokens);

    int chosen_sz_idx = 0;
    wrapped_line_t lines[6];
    int n_lines = 0;

    if (g_cfg.sizing_static) {
        chosen_sz_idx = 0; // 60px static default
        n_lines = wrap_phrase_for_size(tokens, n_toks, chosen_sz_idx, 380, lines);
    } else {
        const int try_order[3] = { 2, 1, 0 }; // 84px -> 72px -> 60px
        for (int k = 0; k < 3; k++) {
            int s_idx = try_order[k];
            int l_cnt = wrap_phrase_for_size(tokens, n_toks, s_idx, 380, lines);
            int lh = heb_font_line_heights[s_idx];
            if (l_cnt <= 3 && (l_cnt * lh) <= 320) {
                chosen_sz_idx = s_idx;
                n_lines = l_cnt;
                break;
            }
            if (k == 2) {
                chosen_sz_idx = 0;
                n_lines = l_cnt;
            }
        }
    }

    int lh = heb_font_line_heights[chosen_sz_idx];
    int px_sz = heb_font_px_sizes[chosen_sz_idx];
    int sp_w = effective_space_width(chosen_sz_idx);
    int span = (n_lines - 1) * lh;
    int start_baseline_y = (int)roundf(233.0f - span / 2.0f + px_sz * 0.28f);

    for (int i = 0; i < n_lines; i++) {
        const wrapped_line_t *ln = &lines[i];
        int base_y = start_baseline_y + i * lh;

        int line_left_x;
        if (g_cfg.align_center) {
            line_left_x = (466 - ln->width_px) / 2;
        } else {
            float dy = fabsf((float)(base_y - px_sz * 0.28f) - 233.0f);
            float chord_half = (dy < 195.0f) ? sqrtf(195.0f * 195.0f - dy * dy) : 120.0f;
            int right_anchor = 233 + (int)chord_half;
            if (right_anchor > 418) right_anchor = 418;
            line_left_x = right_anchor - ln->width_px;
        }

        int cur_right_x = line_left_x + ln->width_px;
        rgb_t fg_c = ln->is_period_line ? period_rgb : letter_rgb;
        rgb_t nk_c = ln->is_period_line ? period_nikk_rgb : nikkud_rgb;

        for (int w_idx = 0; w_idx < ln->count; w_idx++) {
            heb_token_id_t tid = ln->toks[w_idx].id;
            const heb_word_glyph_t *g = &heb_word_table[chosen_sz_idx][tid];
            int wx = cur_right_x - g->width;
            int wy = base_y - g->baseline_y;

            if (g_cfg.nikkud_mode == 0) {
                blit_rle_mask(s_clock_fb, wx, wy, g->width, g->height, g->cons_offset, bg_rgb, fg_c, word_alpha);
                blit_rle_mask(s_clock_fb, wx, wy, g->width, g->height, g->nikk_offset, bg_rgb, nk_c, word_alpha);
            } else if (g_cfg.nikkud_mode == 1) {
                blit_rle_mask(s_clock_fb, wx, wy, g->width, g->height, g->nikk_offset, bg_rgb, fg_c, word_alpha);
                blit_rle_mask(s_clock_fb, wx, wy, g->width, g->height, g->cons_offset, bg_rgb, fg_c, word_alpha);
            } else {
                blit_rle_mask(s_clock_fb, wx, wy, g->width, g->height, g->cons_offset, bg_rgb, fg_c, word_alpha);
            }
            cur_right_x -= (g->width + sp_w);
        }
    }
}

static void render_crossfade_frame(int prev_h, int prev_m, uint8_t out_alpha,
                                   int cur_h,  int cur_m,  uint8_t in_alpha) {
    if (!s_clock_fb || !s_canvas_clock) return;

    preset_spec_t p_cur = resolve_preset_for_hour(cur_h);
    rgb_t bg_rgb = g_cfg.pure_black_oled ? (rgb_t){0, 0, 0} : p_cur.bg;
    uint16_t bg565 = rgb_to_565(bg_rgb.r, bg_rgb.g, bg_rgb.b);
    for (int i = 0; i < 466 * 466; i++) {
        s_clock_fb[i] = bg565;
    }

    s_last_bg565     = bg565;
    s_last_bg_rgb    = bg_rgb;
    s_last_nikk_rgb  = p_cur.nikkud;
    s_prev_dot_valid = false;

    if (out_alpha > 0 && prev_h >= 0) {
        preset_spec_t p_prev = resolve_preset_for_hour(prev_h);
        composite_phrase_layer(prev_h, prev_m, bg_rgb, p_prev, out_alpha);
    }
    if (in_alpha > 0 && cur_h >= 0) {
        composite_phrase_layer(cur_h, cur_m, bg_rgb, p_cur, in_alpha);
    }

    draw_hud_icons(s_clock_fb, bg_rgb);
    draw_seconds_dot_on_fb(false);
    lv_obj_invalidate(s_canvas_clock);
}

static void on_clock_anim_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!s_clock_fb || !s_canvas_clock) return;

    // Never animate or draw seconds indicator while asleep or on the Settings screen
    if (ui_is_screen_asleep() || lv_scr_act() != lv_obj_get_parent(s_canvas_clock)) {
        return;
    }

    if (s_anim_active) {
        int64_t elapsed_us = esp_timer_get_time() - s_anim_start_us;
        if (elapsed_us >= CROSSFADE_DURATION_US) {
            s_anim_active = false;
            render_crossfade_frame(s_prev_hour, s_prev_min, 0, s_cur_hour, s_cur_min, 255);
            return;
        }

        // Normalized progress u in [0, 1], smooth S-curve s = u^2 * (3 - 2u)
        // Outgoing phrase eases out as (1 - s), incoming phrase eases in as s, intersecting at 50% at u = 0.5!
        float u = clamp_f((float)elapsed_us / (float)CROSSFADE_DURATION_US, 0.0f, 1.0f);
        float s = u * u * (3.0f - 2.0f * u);
        uint8_t out_alpha = clamp_u8((int)roundf((1.0f - s) * 255.0f));
        uint8_t in_alpha  = clamp_u8((int)roundf(s * 255.0f));
        render_crossfade_frame(s_prev_hour, s_prev_min, out_alpha, s_cur_hour, s_cur_min, in_alpha);
        return;
    }

    // Check for minute rollover right at :00.000 so the phrase crossfade starts immediately
    time_t now;
    time(&now);
    struct tm tm_loc;
    localtime_r(&now, &tm_loc);
    if (s_cur_hour >= 0 && (tm_loc.tm_hour != s_cur_hour || tm_loc.tm_min != s_cur_min)) {
        clock_renderer_render_now();
        return;
    }

    // Update the 60-second perimeter nikkud dot using fast dirty-rectangle invalidation
    if (g_cfg.show_seconds || s_prev_dot_valid) {
        draw_seconds_dot_on_fb(true);
    }
}

lv_obj_t *clock_renderer_init_canvas(lv_obj_t *parent_scr) {
    s_clock_fb = (uint16_t *)heap_caps_malloc(466 * 466 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    assert(s_clock_fb != NULL);

    s_canvas_clock = lv_canvas_create(parent_scr);
    lv_canvas_set_buffer(s_canvas_clock, s_clock_fb, 466, 466, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_canvas_clock);

    // Run 40 FPS timer continuously for the seconds dot ease-in/ease-out and minute crossfades
    s_anim_timer = lv_timer_create(on_clock_anim_timer_cb, 25, NULL);

    return s_canvas_clock;
}

void clock_renderer_render_immediate(void) {
    if (!s_clock_fb || !s_canvas_clock) return;

    time_t now;
    time(&now);
    struct tm tm_loc;
    localtime_r(&now, &tm_loc);

    s_cur_hour    = tm_loc.tm_hour;
    s_cur_min     = tm_loc.tm_min;
    s_prev_hour   = s_cur_hour;
    s_prev_min    = s_cur_min;
    s_anim_active = false;

    render_crossfade_frame(s_prev_hour, s_prev_min, 0, s_cur_hour, s_cur_min, 255);
}

void clock_renderer_render_now(void) {
    if (!s_clock_fb || !s_canvas_clock) return;

    time_t now;
    time(&now);
    struct tm tm_loc;
    localtime_r(&now, &tm_loc);
    int hour = tm_loc.tm_hour;
    int minute = tm_loc.tm_min;

    if (s_cur_hour < 0) {
        // Initial boot render: draw immediately
        s_cur_hour  = hour;
        s_cur_min   = minute;
        s_prev_hour = hour;
        s_prev_min  = minute;
        render_crossfade_frame(hour, minute, 0, hour, minute, 255);
        return;
    }

    if (hour != s_cur_hour || minute != s_cur_min) {
        // Clock minute changed -> start simultaneous ease-out / ease-in crossfade intersecting at 50%!
        s_prev_hour     = s_cur_hour;
        s_prev_min      = s_cur_min;
        s_cur_hour      = hour;
        s_cur_min       = minute;
        s_anim_start_us = esp_timer_get_time();
        s_anim_active   = true;
        on_clock_anim_timer_cb(s_anim_timer);
        return;
    }

    if (!s_anim_active) {
        render_crossfade_frame(hour, minute, 0, hour, minute, 255);
    }
}
