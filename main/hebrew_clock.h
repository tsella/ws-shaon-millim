#pragma once

#include <stdbool.h>
#include "hebrew_glyphs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    heb_token_id_t id;
    bool is_period;
} phrase_tok_t;

// Builds the exact Hebrew word sequence for (hour 0..23, minute 0..59), matching shaon_millim.py
int hebrew_clock_build_phrase(int hour, int minute, phrase_tok_t *out_tokens);

// Non-blocking FreeRTOS task trigger to speak "הַשָּׁעָה..." + current time in Hebrew
void hebrew_clock_speak_current_time(void);

// Non-blocking FreeRTOS task trigger to play the mechanical winding clock chime
void hebrew_clock_play_chime_async(void);

#ifdef __cplusplus
}
#endif
