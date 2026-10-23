#include "screencap.h"
#include "hal_board.h"
#include "ui_screens.h"
#include "hebrew_clock.h"
#include "net_time.h"
#include "clock_renderer.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"

static const char B64_TAB[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_print_line(const uint8_t *data, size_t len) {
    char out[132];
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) v |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) v |= (uint32_t)data[i + 2];

        out[j++] = B64_TAB[(v >> 18) & 0x3F];
        out[j++] = B64_TAB[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? B64_TAB[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? B64_TAB[v & 0x3F] : '=';

        if (j >= 120) {
            out[j] = '\0';
            printf("B64:%s\n", out);
            j = 0;
        }
    }
    if (j > 0) {
        out[j] = '\0';
        printf("B64:%s\n", out);
    }
}

static void dump_vram_shadow_rle(void) {
    if (!g_vram_shadow) {
        printf("<<<SCREENCAP_ERR no_vram>>>\n");
        return;
    }

    printf("\n<<<SCREENCAP_BEGIN 466 466>>>\n");
    fflush(stdout);

    const int total = 466 * 466;
    uint8_t chunk[90]; // multiple of 3 for base64 (90 raw bytes -> 120 base64 chars)
    size_t c_len = 0;
    int line_cnt = 0;

    int i = 0;
    while (i < total) {
        uint16_t px = g_vram_shadow[i];
        int run = 1;
        while (i + run < total && run < 255 && g_vram_shadow[i + run] == px) {
            run++;
        }
        chunk[c_len++] = (uint8_t)run;
        chunk[c_len++] = (uint8_t)(px & 0xFF);
        chunk[c_len++] = (uint8_t)((px >> 8) & 0xFF);

        if (c_len == sizeof(chunk)) {
            b64_print_line(chunk, c_len);
            c_len = 0;
            if (++line_cnt % 64 == 0) {
                fflush(stdout);
            }
        }
        i += run;
    }
    if (c_len > 0) {
        b64_print_line(chunk, c_len);
    }
    printf("<<<SCREENCAP_END>>>\n");
    fflush(stdout);
}

static void screencap_task(void *arg) {
    char line[64];
    int pos = 0;

    while (true) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                pos = 0;
                if (strcmp(line, "SCREENCAP") == 0) {
                    dump_vram_shadow_rle();
                } else if (strcmp(line, "SHOW_SETTINGS") == 0) {
                    if (bsp_display_lock(500)) {
                        ui_show_settings_screen();
                        bsp_display_unlock();
                    }
                    printf("<<<OK SHOW_SETTINGS>>>\n");
                } else if (strcmp(line, "SHOW_CLOCK") == 0) {
                    if (bsp_display_lock(500)) {
                        ui_show_clock_screen();
                        bsp_display_unlock();
                    }
                    printf("<<<OK SHOW_CLOCK>>>\n");
                } else if (strcmp(line, "SPEAK") == 0) {
                    hebrew_clock_speak_current_time();
                    printf("<<<OK SPEAK>>>\n");
                } else if (strcmp(line, "CHIME") == 0) {
                    hebrew_clock_play_chime_async();
                    printf("<<<OK CHIME>>>\n");
                } else if (strcmp(line, "PREVIEW_WIFI_SLEEP") == 0) {
                    g_wifi_sleeping = true;
                    if (bsp_display_lock(500)) {
                        clock_renderer_render_now();
                        bsp_display_unlock();
                    }
                    printf("<<<OK PREVIEW_WIFI_SLEEP>>>\n");
                } else if (strcmp(line, "PREVIEW_WIFI_NORM") == 0) {
                    g_wifi_sleeping = false;
                    if (bsp_display_lock(500)) {
                        clock_renderer_render_now();
                        bsp_display_unlock();
                    }
                    printf("<<<OK PREVIEW_WIFI_NORM>>>\n");
                }
            }
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}

void screencap_init(void) {
    xTaskCreatePinnedToCore(screencap_task, "screencap", 4096, NULL, 3, NULL, 0);
}
