#pragma once

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Allocates the 466x466 RGB565 PSRAM framebuffer and binds it to an LVGL canvas on parent_scr
lv_obj_t *clock_renderer_init_canvas(lv_obj_t *parent_scr);

// Renders the current Hebrew time phrase and HUD icons (Wi-Fi, color-graded battery, settings cog).
// When the displayed minute changes, automatically triggers a smooth ease-out / ease-in transition.
void clock_renderer_render_now(void);

// Forces an immediate non-animated render of the current time and HUD (e.g. when changing theme/nikkud in settings)
void clock_renderer_render_immediate(void);

#ifdef __cplusplus
}
#endif
