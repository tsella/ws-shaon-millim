#include "hebrew_clock.h"
#include "hal_board.h"
#include "app_config.h"

#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const heb_token_id_t HOUR_1_TO_10[11] = {
    TOK_CHATZOT, // 0 unused
    TOK_ACHAT,   // 1
    TOK_SHTAYIM, // 2
    TOK_SHALOSH, // 3
    TOK_ARBA,    // 4
    TOK_CHAMESH, // 5
    TOK_SHESH,   // 6
    TOK_SHEVA,   // 7
    TOK_SHMONEH, // 8
    TOK_TESHA,   // 9
    TOK_ESER     // 10
};

static const heb_token_id_t ONES_AND_TOKS[10] = {
    TOK_CHATZOT,   // 0 unused
    TOK_VEACHAT,   // 1
    TOK_USHTAYIM,  // 2
    TOK_VESHALOSH, // 3
    TOK_VEARBA,    // 4
    TOK_VECHAMESH, // 5
    TOK_VASHESH,   // 6
    TOK_VASHEVA,   // 7
    TOK_USHMONEH,  // 8
    TOK_VATESHA    // 9
};

static const heb_token_id_t TEENS_PREFIX_TOKS[10] = {
    TOK_CHATZOT,   // 10 unused
    TOK_VEACHAT,   // 11: וְאַחַת עֶשְׂרֵה
    TOK_USHTEIM,   // 12: וּשְׁתֵּים עֶשְׂרֵה
    TOK_USHLOSH,   // 13: וּשְׁלוֹשׁ עֶשְׂרֵה
    TOK_VEARBA,    // 14: וְאַרְבַּע עֶשְׂרֵה
    TOK_VAREVA,    // 15: וָרֶבַע (handled separately)
    TOK_VESHESH,   // 16: וְשֵׁשׁ עֶשְׂרֵה
    TOK_USHVA,     // 17: וּשְׁבַע עֶשְׂרֵה
    TOK_USHMONEH,  // 18: וּשְׁמוֹנֶה עֶשְׂרֵה
    TOK_UTSHA      // 19: וּתְשַׁע עֶשְׂרֵה
};

int hebrew_clock_build_phrase(int hour, int minute, phrase_tok_t *out_tokens) {
    int n = 0;

    // Special case: 00:00 -> חֲצוֹת
    if (hour == 0 && minute == 0) {
        out_tokens[n++] = (phrase_tok_t){ TOK_CHATZOT, false };
        return n;
    }

    // 1. Hour word(s)
    int h12 = hour % 12;
    if (h12 == 0) h12 = 12;
    if (h12 <= 10) {
        out_tokens[n++] = (phrase_tok_t){ HOUR_1_TO_10[h12], false };
    } else if (h12 == 11) {
        out_tokens[n++] = (phrase_tok_t){ TOK_ACHAT, false };
        out_tokens[n++] = (phrase_tok_t){ TOK_ESREH, false };
    } else {
        out_tokens[n++] = (phrase_tok_t){ TOK_SHTEIM, false };
        out_tokens[n++] = (phrase_tok_t){ TOK_ESREH, false };
    }

    // 2. Minute word(s) — exact port of shaon_millim.py
    if (minute == 0) {
        // exact hour: no minute words
    } else if (minute == 1) {
        out_tokens[n++] = (phrase_tok_t){ TOK_VEDAKA, false };
    } else if (minute == 2) {
        out_tokens[n++] = (phrase_tok_t){ TOK_USHTEI, false };
        out_tokens[n++] = (phrase_tok_t){ TOK_DAKOT, false };
    } else if (minute >= 3 && minute <= 9) {
        out_tokens[n++] = (phrase_tok_t){ ONES_AND_TOKS[minute], false };
        out_tokens[n++] = (phrase_tok_t){ TOK_DAKOT, false };
    } else if (minute == 10) {
        out_tokens[n++] = (phrase_tok_t){ TOK_VEESER, false };
        out_tokens[n++] = (phrase_tok_t){ TOK_DAKOT, false };
    } else if (minute == 15) {
        out_tokens[n++] = (phrase_tok_t){ TOK_VAREVA, false };
    } else if (minute >= 11 && minute <= 19) {
        out_tokens[n++] = (phrase_tok_t){ TEENS_PREFIX_TOKS[minute - 10], false };
        out_tokens[n++] = (phrase_tok_t){ TOK_ESREH, false };
    } else if (minute == 20) {
        out_tokens[n++] = (phrase_tok_t){ TOK_VEESRIM, false };
    } else if (minute == 30) {
        out_tokens[n++] = (phrase_tok_t){ TOK_VACHETZI, false };
    } else if (minute == 40) {
        out_tokens[n++] = (phrase_tok_t){ TOK_VEARBAIM, false };
    } else if (minute == 50) {
        out_tokens[n++] = (phrase_tok_t){ TOK_VACHAMISHIM, false };
    } else {
        int ten = (minute / 10) * 10;
        int one = minute % 10;
        heb_token_id_t ten_tok = TOK_VEESRIM;
        if (ten == 30) ten_tok = TOK_USHLOSHIM;
        else if (ten == 40) ten_tok = TOK_VEARBAIM;
        else if (ten == 50) ten_tok = TOK_VACHAMISHIM;
        out_tokens[n++] = (phrase_tok_t){ ten_tok, false };
        out_tokens[n++] = (phrase_tok_t){ ONES_AND_TOKS[one], false };
    }

    // 3. Day period suffix
    if (hour == 0) {
        // midnight hour (00:01..00:59) has no suffix
    } else if (hour >= 1 && hour < 5) {
        out_tokens[n++] = (phrase_tok_t){ TOK_BALAYLA, true };
    } else if (hour >= 5 && hour < 12) {
        out_tokens[n++] = (phrase_tok_t){ TOK_BABOKER, true };
    } else if (hour == 12) {
        out_tokens[n++] = (phrase_tok_t){ TOK_BATZAHORAYIM, true };
    } else if (hour >= 13 && hour < 17) {
        if (n >= 3) {
            out_tokens[n++] = (phrase_tok_t){ TOK_ACHATZ_ABBR, true };
        } else {
            out_tokens[n++] = (phrase_tok_t){ TOK_ACHAR, true };
            out_tokens[n++] = (phrase_tok_t){ TOK_HATZAHORAYIM, true };
        }
    } else if (hour >= 17 && hour < 21) {
        out_tokens[n++] = (phrase_tok_t){ TOK_BAEREV, true };
    } else {
        out_tokens[n++] = (phrase_tok_t){ TOK_BALAYLA, true };
    }

    return n;
}

static void speak_time_task(void *arg) {
    if (g_cfg.voice_id == 2) {
        hal_audio_play_chime(g_cfg.volume);
        vTaskDelete(NULL);
        return;
    }

    time_t now;
    time(&now);
    struct tm tm_loc;
    localtime_r(&now, &tm_loc);

    phrase_tok_t phrase[10];
    int n_toks = hebrew_clock_build_phrase(tm_loc.tm_hour, tm_loc.tm_min, phrase);

    heb_token_id_t seq[12];
    int seq_len = 0;
    seq[seq_len++] = TOK_HASHA_A; // Prepend "הַשָּׁעָה..."
    for (int i = 0; i < n_toks; i++) {
        seq[seq_len++] = phrase[i].id;
    }

    hal_audio_play_tokens(seq, seq_len, g_cfg.voice_id, g_cfg.volume);
    vTaskDelete(NULL);
}

static void play_chime_task(void *arg) {
    hal_audio_play_chime(g_cfg.volume);
    vTaskDelete(NULL);
}

void hebrew_clock_speak_current_time(void) {
    xTaskCreatePinnedToCore(speak_time_task, "speak_time", 6144, NULL, 5, NULL, 1);
}

void hebrew_clock_play_chime_async(void) {
    xTaskCreatePinnedToCore(play_chime_task, "play_chime", 6144, NULL, 5, NULL, 1);
}
