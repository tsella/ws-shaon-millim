#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOK_HASHA_A = 0, // הַשָּׁעָה
    TOK_CHATZOT = 1, // חֲצוֹת
    TOK_ACHAT = 2, // אַחַת
    TOK_SHTAYIM = 3, // שְׁתַּיִם
    TOK_SHALOSH = 4, // שָׁלוֹשׁ
    TOK_ARBA = 5, // אַרְבַּע
    TOK_CHAMESH = 6, // חָמֵשׁ
    TOK_SHESH = 7, // שֵׁשׁ
    TOK_SHEVA = 8, // שֶׁבַע
    TOK_SHMONEH = 9, // שְׁמוֹנֶה
    TOK_TESHA = 10, // תֵּשַׁע
    TOK_ESER = 11, // עֶשֶׂר
    TOK_SHTEIM = 12, // שְׁתֵּים
    TOK_ESREH = 13, // עֶשְׂרֵה
    TOK_VEDAKA = 14, // וְדַקָּה
    TOK_USHTEI = 15, // וּשְׁתֵּי
    TOK_DAKOT = 16, // דַּקּוֹת
    TOK_VEACHAT = 17, // וְאַחַת
    TOK_USHTAYIM = 18, // וּשְׁתַּיִם
    TOK_VESHALOSH = 19, // וְשָׁלוֹשׁ
    TOK_VEARBA = 20, // וְאַרְבַּע
    TOK_VECHAMESH = 21, // וְחָמֵשׁ
    TOK_VASHESH = 22, // וָשֵׁשׁ
    TOK_VASHEVA = 23, // וָשֶׁבַע
    TOK_USHMONEH = 24, // וּשְׁמוֹנֶה
    TOK_VATESHA = 25, // וָתֵשַׁע
    TOK_VEESER = 26, // וְעֶשֶׂר
    TOK_USHTEIM = 27, // וּשְׁתֵּים
    TOK_USHLOSH = 28, // וּשְׁלוֹשׁ
    TOK_VAREVA = 29, // וָרֶבַע
    TOK_VESHESH = 30, // וְשֵׁשׁ
    TOK_USHVA = 31, // וּשְׁבַע
    TOK_UTSHA = 32, // וּתְשַׁע
    TOK_VEESRIM = 33, // וְעֶשְׂרִים
    TOK_VACHETZI = 34, // וָחֵצִי
    TOK_USHLOSHIM = 35, // וּשְׁלוֹשִׁים
    TOK_VEARBAIM = 36, // וְאַרְבָּעִים
    TOK_VACHAMISHIM = 37, // וַחֲמִשִּׁים
    TOK_BABOKER = 38, // בַּבּוֹקֶר
    TOK_BATZAHORAYIM = 39, // בַּצָּהֳרַיִם
    TOK_ACHAR = 40, // אַחַר
    TOK_HATZAHORAYIM = 41, // הַצָּהֳרַיִם
    TOK_ACHATZ_ABBR = 42, // אחה״צ
    TOK_BAEREV = 43, // בָּעֶרֶב
    TOK_BALAYLA = 44, // בַּלַּיְלָה
    TOK_COUNT = 45
} heb_token_id_t;

#define HEB_FONT_SIZE_COUNT 4

typedef struct {
    uint16_t width;
    uint16_t height;
    int16_t  baseline_y;
    uint32_t cons_offset;
    uint32_t nikk_offset;
} heb_word_glyph_t;

extern const uint8_t heb_font_px_sizes[HEB_FONT_SIZE_COUNT];
extern const uint8_t heb_font_line_heights[HEB_FONT_SIZE_COUNT];
extern const uint8_t heb_font_space_widths[HEB_FONT_SIZE_COUNT];
extern const heb_word_glyph_t heb_word_table[HEB_FONT_SIZE_COUNT][TOK_COUNT];
extern const uint8_t heb_glyph_bitmap_blob[];

#ifdef __cplusplus
}
#endif
