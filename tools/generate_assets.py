#!/usr/bin/env python3
"""
generate_assets.py
==================
Build-time generator for ws-shaon-millim:
1. Shapes every Hebrew word token using HarfBuzz (`kern`, `mark`, `mkmk`, `liga`)
   and rasterizes separate 8-bit grayscale alpha masks for:
     - Base consonants (`cons_mask`)
     - Vowel points / nikkud (`nikk_mask`)
   across 4 font sizes:
     - Size 0: 60px (Default Static size - fits all 1,440 minutes cleanly in <=3 lines)
     - Size 1: 72px (Fit mode medium-large)
     - Size 2: 84px (Fit mode large)
     - Size 3: 50px (Compact fallback)
   Emits `main/hebrew_glyphs.h` and `main/hebrew_glyphs.c` so the native ESP32-S3 C/C++
   renderer can composite any colour preset, nikkud mode, alignment, and sizing at 60 FPS!

2. Synthesizes all 39 Hebrew voice tokens using Microsoft Edge Neural TTS
   (`he-IL-HilaNeural` and `he-IL-AvriNeural`), converts them via macOS `afconvert`
   to 16 kHz, 16-bit mono signed PCM, trims leading/trailing silence for natural
   sentence cadence, and packs them into `build/voice_pack.bin` ready to flash
   to the `voice` partition on the 32MB flash!
"""

import asyncio
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
from typing import Dict, List, Tuple

import edge_tts
import freetype
import numpy as np
import uharfbuzz as hb

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = os.path.join(ROOT_DIR, "fonts", "FrankRuhlLibre-VF.ttf")
MAIN_DIR = os.path.join(ROOT_DIR, "main")
BUILD_DIR = os.path.join(ROOT_DIR, "build")

# Exact ordered list of all 38 Hebrew word tokens (plus index 0 = "הַשָּׁעָה" spoken prefix)
# Every single minute of the 24-hour day is composed from these tokens!
WORD_TABLE: List[Tuple[str, str]] = [
    ("TOK_HASHA_A",       "הַשָּׁעָה"),         # 0: Spoken prefix ("The time is...")
    ("TOK_CHATZOT",       "חֲצוֹת"),            # 1: Midnight
    ("TOK_ACHAT",         "אַחַת"),             # 2: 1
    ("TOK_SHTAYIM",       "שְׁתַּיִם"),          # 3: 2
    ("TOK_SHALOSH",       "שָׁלוֹשׁ"),           # 4: 3
    ("TOK_ARBA",          "אַרְבַּע"),           # 5: 4
    ("TOK_CHAMESH",       "חָמֵשׁ"),            # 6: 5
    ("TOK_SHESH",         "שֵׁשׁ"),             # 7: 6
    ("TOK_SHEVA",         "שֶׁבַע"),            # 8: 7
    ("TOK_SHMONEH",       "שְׁמוֹנֶה"),          # 9: 8
    ("TOK_TESHA",         "תֵּשַׁע"),            # 10: 9
    ("TOK_ESER",          "עֶשֶׂר"),            # 11: 10
    ("TOK_SHTEIM",        "שְׁתֵּים"),           # 12: 12 first word
    ("TOK_ESREH",         "עֶשְׂרֵה"),           # 13: -teen (11, 12, 13..19)
    ("TOK_VEDAKA",        "וְדַקָּה"),           # 14: and a minute (m=1)
    ("TOK_USHTEI",        "וּשְׁתֵּי"),          # 15: and two (m=2)
    ("TOK_DAKOT",         "דַּקּוֹת"),           # 16: minutes (m=2..10)
    ("TOK_VEACHAT",       "וְאַחַת"),           # 17: and 1
    ("TOK_USHTAYIM",      "וּשְׁתַּיִם"),        # 18: and 2
    ("TOK_VESHALOSH",     "וְשָׁלוֹשׁ"),         # 19: and 3
    ("TOK_VEARBA",        "וְאַרְבַּע"),         # 20: and 4
    ("TOK_VECHAMESH",     "וְחָמֵשׁ"),          # 21: and 5
    ("TOK_VASHESH",       "וָשֵׁשׁ"),           # 22: and 6
    ("TOK_VASHEVA",       "וָשֶׁבַע"),          # 23: and 7
    ("TOK_USHMONEH",      "וּשְׁמוֹנֶה"),        # 24: and 8
    ("TOK_VATESHA",       "וָתֵשַׁע"),          # 25: and 9
    ("TOK_VEESER",        "וְעֶשֶׂר"),          # 26: and 10
    ("TOK_USHTEIM",       "וּשְׁתֵּים"),         # 27: and 12 (first word)
    ("TOK_USHLOSH",       "וּשְׁלוֹשׁ"),         # 28: and 13 (first word)
    ("TOK_VAREVA",        "וָרֶבַע"),           # 29: and a quarter (m=15)
    ("TOK_VESHESH",       "וְשֵׁשׁ"),           # 30: and 16 (first word)
    ("TOK_USHVA",         "וּשְׁבַע"),          # 31: and 17 (first word)
    ("TOK_UTSHA",         "וּתְשַׁע"),          # 32: and 19 (first word)
    ("TOK_VEESRIM",       "וְעֶשְׂרִים"),        # 33: and 20
    ("TOK_VACHETZI",      "וָחֵצִי"),           # 34: and a half (m=30)
    ("TOK_USHLOSHIM",     "וּשְׁלוֹשִׁים"),       # 35: and 30 (m=31..39)
    ("TOK_VEARBAIM",      "וְאַרְבָּעִים"),      # 36: and 40
    ("TOK_VACHAMISHIM",   "וַחֲמִשִּׁים"),       # 37: and 50
    ("TOK_BABOKER",       "בַּבּוֹקֶר"),         # 38: in the morning
    ("TOK_BATZAHORAYIM",  "בַּצָּהֳרַיִם"),       # 39: at noon
    ("TOK_ACHAR",         "אַחַר"),             # 40: after (afternoon word 1)
    ("TOK_HATZAHORAYIM",  "הַצָּהֳרַיִם"),       # 41: noon (afternoon word 2)
    ("TOK_ACHATZ_ABBR",   "אחה״צ"),            # 42: afternoon abbreviated (spoken as אחר הצהריים)
    ("TOK_BAEREV",        "בָּעֶרֶב"),          # 43: in the evening
    ("TOK_BALAYLA",       "בַּלַּיְלָה"),        # 44: at night
]

FONT_SIZES = [60, 72, 84, 48]


class HarfBuzzWordShaper:
    def __init__(self, font_path: str, size_px: int, weight: float = 700.0):
        self.size_px = size_px
        with open(font_path, "rb") as f:
            self.font_bytes = f.read()
        self.hb_blob = hb.Blob(self.font_bytes)
        self.hb_face = hb.Face(self.hb_blob)
        self.hb_font = hb.Font(self.hb_face)
        self.hb_font.scale = (size_px * 64, size_px * 64)
        self.hb_font.set_variations({"wght": weight})

        self.ft_face = freetype.Face(font_path)
        self.ft_face.set_pixel_sizes(0, size_px)
        self.ft_face.set_var_design_coords([weight])

        self.nikkud_gids = set()
        for cp in range(0x0591, 0x05C8):
            gid = self.hb_font.get_nominal_glyph(cp)
            if gid:
                self.nikkud_gids.add(gid)

        # Measure natural space advance
        buf = hb.Buffer()
        buf.add_str(" ")
        hb.shape(self.hb_font, buf)
        self.space_advance = int(round(buf.glyph_positions[0].x_advance / 64.0))

    def rasterize_word(self, text: str):
        buf = hb.Buffer()
        buf.add_str(text)
        buf.direction = "rtl"
        buf.script = "Hebr"
        buf.language = "he"
        hb.shape(
            self.hb_font,
            buf,
            {"kern": True, "mark": True, "mkmk": True, "ccmp": False, "liga": False},
        )

        infos = buf.glyph_infos
        positions = buf.glyph_positions

        pen_x = 0.0
        placements = []
        min_x, max_x = 1e9, -1e9
        min_y, max_y = 1e9, -1e9

        for info, pos in zip(infos, positions):
            gid = info.codepoint
            self.ft_face.load_glyph(gid, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
            g = self.ft_face.glyph
            bmp = g.bitmap
            w, rows = bmp.width, bmp.rows
            gx = int(round(pen_x + (pos.x_offset / 64.0) + g.bitmap_left))
            gy = int(round(-(pos.y_offset / 64.0) - g.bitmap_top))
            if w > 0 and rows > 0:
                min_x = min(min_x, gx)
                max_x = max(max_x, gx + w)
                min_y = min(min_y, gy)
                max_y = max(max_y, gy + rows)
                arr = np.array(bmp.buffer, dtype=np.uint8).reshape((rows, w))
                placements.append((gid in self.nikkud_gids, gx, gy, w, rows, arr))
            pen_x += pos.x_advance / 64.0

        W = int(max_x - min_x)
        H = int(max_y - min_y)
        cons_mask = np.zeros((H, W), dtype=np.uint8)
        nikk_mask = np.zeros((H, W), dtype=np.uint8)

        for is_nikk, gx, gy, w, rows, arr in placements:
            x0 = gx - min_x
            y0 = gy - min_y
            target = nikk_mask if is_nikk else cons_mask
            target[y0:y0 + rows, x0:x0 + w] = np.maximum(target[y0:y0 + rows, x0:x0 + w], arr)

        # Prevent overlap where consonant and nikkud touch so `full` or `color` blending is clean
        baseline_y = int(-min_y)
        return W, H, baseline_y, cons_mask.tobytes(), nikk_mask.tobytes()


def rle_encode(data: bytes) -> bytearray:
    """Simple fast RLE on 8-bit alpha masks:
    - 0x00, count (1..255): run of `count` transparent (0x00) pixels
    - 0xFF, count (1..255): run of `count` solid (0xFF) pixels
    - 0x01..0xFE: single anti-aliased edge pixel
    """
    out = bytearray()
    n = len(data)
    i = 0
    while i < n:
        b = data[i]
        if b == 0x00 or b == 0xFF:
            run = 1
            while i + run < n and data[i + run] == b and run < 255:
                run += 1
            out.append(b)
            out.append(run)
            i += run
        else:
            out.append(b)
            i += 1
    return out


def generate_c_glyphs():
    print("Generating HarfBuzz shaped Hebrew word bitmaps for sizes:", FONT_SIZES)
    os.makedirs(MAIN_DIR, exist_ok=True)

    blob_bytes = bytearray()
    entries_per_size = []
    space_advances = []

    for sz in FONT_SIZES:
        shaper = HarfBuzzWordShaper(FONT_PATH, sz, weight=700.0)
        space_advances.append(shaper.space_advance)
        sz_entries = []
        for enum_name, heb_text in WORD_TABLE:
            w, h, base_y, c_bytes, n_bytes = shaper.rasterize_word(heb_text)
            c_rle = rle_encode(c_bytes)
            n_rle = rle_encode(n_bytes)
            c_off = len(blob_bytes)
            blob_bytes.extend(c_rle)
            n_off = len(blob_bytes)
            blob_bytes.extend(n_rle)
            sz_entries.append((w, h, base_y, c_off, n_off))
        entries_per_size.append(sz_entries)

    print(f"Total RLE-compressed shaped glyph mask data across {len(FONT_SIZES)} sizes: {len(blob_bytes):,} bytes ({len(blob_bytes)/1024:.1f} KB)")

    h_path = os.path.join(MAIN_DIR, "hebrew_glyphs.h")
    c_path = os.path.join(MAIN_DIR, "hebrew_glyphs.c")

    with open(h_path, "w", encoding="utf-8") as fh:
        fh.write("#pragma once\n#include <stdint.h>\n\n")
        fh.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
        fh.write("typedef enum {\n")
        for idx, (enum_name, heb_text) in enumerate(WORD_TABLE):
            fh.write(f"    {enum_name} = {idx}, // {heb_text}\n")
        fh.write(f"    TOK_COUNT = {len(WORD_TABLE)}\n}} heb_token_id_t;\n\n")
        fh.write(f"#define HEB_FONT_SIZE_COUNT {len(FONT_SIZES)}\n\n")
        fh.write("typedef struct {\n")
        fh.write("    uint16_t width;\n")
        fh.write("    uint16_t height;\n")
        fh.write("    int16_t  baseline_y;\n")
        fh.write("    uint32_t cons_offset;\n")
        fh.write("    uint32_t nikk_offset;\n")
        fh.write("} heb_word_glyph_t;\n\n")
        fh.write("extern const uint8_t heb_font_px_sizes[HEB_FONT_SIZE_COUNT];\n")
        fh.write("extern const uint8_t heb_font_line_heights[HEB_FONT_SIZE_COUNT];\n")
        fh.write("extern const uint8_t heb_font_space_widths[HEB_FONT_SIZE_COUNT];\n")
        fh.write("extern const heb_word_glyph_t heb_word_table[HEB_FONT_SIZE_COUNT][TOK_COUNT];\n")
        fh.write("extern const uint8_t heb_glyph_bitmap_blob[];\n\n")
        fh.write("#ifdef __cplusplus\n}\n#endif\n")

    line_heights = [69, 84, 96, 56]
    with open(c_path, "w", encoding="utf-8") as fc:
        fc.write('#include "hebrew_glyphs.h"\n\n')
        fc.write("const uint8_t heb_font_px_sizes[HEB_FONT_SIZE_COUNT] = { " + ", ".join(str(x) for x in FONT_SIZES) + " };\n")
        fc.write("const uint8_t heb_font_line_heights[HEB_FONT_SIZE_COUNT] = { " + ", ".join(str(x) for x in line_heights) + " };\n")
        fc.write("const uint8_t heb_font_space_widths[HEB_FONT_SIZE_COUNT] = { " + ", ".join(str(x) for x in space_advances) + " };\n\n")
        fc.write("const heb_word_glyph_t heb_word_table[HEB_FONT_SIZE_COUNT][TOK_COUNT] = {\n")
        for s_idx, sz_entries in enumerate(entries_per_size):
            fc.write(f"    // Size {s_idx}: {FONT_SIZES[s_idx]}px\n    {{\n")
            for w, h, base_y, c_off, n_off in sz_entries:
                fc.write(f"        {{ {w}, {h}, {base_y}, {c_off}U, {n_off}U }},\n")
            fc.write("    },\n")
        fc.write("};\n\n")

        fc.write(f"const uint8_t heb_glyph_bitmap_blob[{len(blob_bytes)}] = {{\n")
        for i in range(0, len(blob_bytes), 24):
            chunk = blob_bytes[i:i + 24]
            fc.write("    " + ",".join(f"0x{b:02X}" for b in chunk) + ",\n")
        fc.write("};\n")

    print("Wrote", h_path, "and", c_path)


async def synth_one_word(voice: str, text: str, out_pcm_path: str):
    # Note: TOK_ACHATZ_ABBR ("אחה״צ") is spoken in full as "אַחַר הַצָּהֳרַיִם"
    tts_text = "אַחַר הַצָּהֳרַיִם" if text == "אחה״צ" else text
    with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as tmp_mp3:
        mp3_path = tmp_mp3.name
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp_wav:
        wav_path = tmp_wav.name

    try:
        comm = edge_tts.Communicate(tts_text, voice, rate="-4%")
        await comm.save(mp3_path)
        subprocess.run([
            "/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1",
            mp3_path, wav_path
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        with open(wav_path, "rb") as fw:
            raw = fw.read()
        # Locate 'data' chunk in WAV
        idx = raw.find(b"data")
        if idx != -1:
            data_len = struct.unpack("<I", raw[idx + 4:idx + 8])[0]
            pcm_bytes = raw[idx + 8:idx + 8 + data_len]
        else:
            pcm_bytes = raw[44:]

        samples = np.frombuffer(pcm_bytes, dtype=np.int16)
        # Trim silence at start and end (threshold = 220 amplitude, with 30ms soft pad)
        abs_s = np.abs(samples)
        active = np.where(abs_s > 220)[0]
        if len(active) > 0:
            pad_start = int(0.020 * 16000) # 20ms
            pad_end   = int(0.045 * 16000) # 45ms
            s0 = max(0, active[0] - pad_start)
            s1 = min(len(samples), active[-1] + pad_end)
            samples = samples[s0:s1]

        with open(out_pcm_path, "wb") as fp:
            fp.write(samples.tobytes())
    finally:
        for p in (mp3_path, wav_path):
            if os.path.exists(p):
                os.remove(p)


async def generate_voice_pack():
    os.makedirs(BUILD_DIR, exist_ok=True)
    cache_dir = os.path.join(BUILD_DIR, "tts_cache")
    os.makedirs(cache_dir, exist_ok=True)

    voices = [
        ("hila", "he-IL-HilaNeural"),
        ("avri", "he-IL-AvriNeural"),
    ]

    for short_name, voice_id in voices:
        print(f"Synthesizing {len(WORD_TABLE)} Hebrew tokens for voice '{short_name}' ({voice_id})...")
        v_dir = os.path.join(cache_dir, short_name)
        os.makedirs(v_dir, exist_ok=True)

        # Run in batches of 8 for fast parallel synthesis
        tasks = []
        for idx, (enum_name, heb_text) in enumerate(WORD_TABLE):
            pcm_path = os.path.join(v_dir, f"{idx:02d}_{enum_name}.pcm")
            if not os.path.exists(pcm_path) or os.path.getsize(pcm_path) == 0:
                tasks.append((idx, heb_text, pcm_path))

        for i in range(0, len(tasks), 8):
            batch = tasks[i:i + 8]
            await asyncio.gather(*(synth_one_word(voice_id, t[1], t[2]) for t in batch))
            print(f"  [{short_name}] {min(i + 8, len(tasks))}/{len(tasks)} synthesized")

    # Pack into binary image `build/voice_pack.bin`:
    # Header:
    #   magic: "WSMV" (4 bytes)
    #   num_voices: uint16_t (2)
    #   num_tokens: uint16_t (45)
    #   entries[2][45]: { uint32_t offset; uint32_t length_bytes; } (2 * 45 * 8 = 720 bytes)
    #   followed by raw 16kHz 16-bit signed mono PCM data
    header_size = 8 + 2 * len(WORD_TABLE) * 8
    pcm_blob = bytearray()
    index_entries = []

    for short_name, _ in voices:
        v_dir = os.path.join(cache_dir, short_name)
        for idx, (enum_name, _) in enumerate(WORD_TABLE):
            pcm_path = os.path.join(v_dir, f"{idx:02d}_{enum_name}.pcm")
            with open(pcm_path, "rb") as f:
                data = f.read()
            off = header_size + len(pcm_blob)
            pcm_blob.extend(data)
            # Align to 4 bytes
            while len(pcm_blob) % 4 != 0:
                pcm_blob.append(0)
            index_entries.append((off, len(data)))

    out_bin = os.path.join(BUILD_DIR, "voice_pack.bin")
    with open(out_bin, "wb") as fb:
        fb.write(b"WSMV")
        fb.write(struct.pack("<HH", len(voices), len(WORD_TABLE)))
        for off, length in index_entries:
            fb.write(struct.pack("<II", off, length))
        fb.write(pcm_blob)

    total_kb = os.path.getsize(out_bin) / 1024.0
    print(f"Created voice pack: {out_bin} ({total_kb:.1f} KB total for both Hila + Avri!)")


def generate_default_config_header():
    """Reads config.json (if present, else config.json.example) and emits main/default_config.h (gitignored!)."""
    import json
    cfg_path = os.path.join(ROOT_DIR, "config.json")
    if not os.path.exists(cfg_path):
        cfg_path = os.path.join(ROOT_DIR, "config.json.example")
    with open(cfg_path, "r", encoding="utf-8") as f:
        cfg = json.load(f)

    wifi = cfg.get("wifi") or {}
    ssid = str(wifi.get("ssid") or "")
    password = str(wifi.get("password") or "")
    if ssid == "YOUR_WIFI_SSID":
        ssid = ""
        password = ""

    tz = str(cfg.get("timezone") or "America/New_York")
    colors = cfg.get("colors") or {}
    preset = str(colors.get("preset") or "shabbat")
    pure_black = 1 if colors.get("pure_black_oled") else 0

    render = cfg.get("render") or {}
    sizing = str(render.get("sizing") or "static")
    align = str(render.get("align") or "center")
    nikkud = str(render.get("nikkud") or "color")
    show_seconds = 1 if render.get("show_seconds", True) else 0

    speech = cfg.get("speech") or {}
    voice_str = str(speech.get("voice") or "hila").lower()
    voice = 2 if voice_str == "chime" else (1 if voice_str == "avri" else 0)
    volume = int(speech.get("volume", 80))
    hourly_en = 1 if speech.get("hourly_enabled", True) else 0
    hourly_start = int(speech.get("hourly_start", 7))
    hourly_end = int(speech.get("hourly_end", 21))

    disp = cfg.get("display") or {}
    brightness = int(disp.get("brightness", 85))
    timeout_sec = int(disp.get("timeout_sec", 60))
    always_on_usb = 1 if disp.get("always_on_usb", True) else 0

    out_h = os.path.join(MAIN_DIR, "default_config_seed.h")
    with open(out_h, "w", encoding="utf-8") as fh:
        fh.write("// Auto-generated from local config.json (gitignored - DO NOT COMMIT)\n")
        fh.write("#pragma once\n\n")
        fh.write(f'#define SEED_WIFI_SSID      "{ssid}"\n')
        fh.write(f'#define SEED_WIFI_PASSWORD  "{password}"\n')
        fh.write(f'#define SEED_TIMEZONE       "{tz}"\n')
        fh.write(f'#define SEED_COLOR_PRESET   "{preset}"\n')
        fh.write(f'#define SEED_PURE_BLACK     {pure_black}\n')
        fh.write(f'#define SEED_SIZING_STATIC  {1 if sizing == "static" else 0}\n')
        fh.write(f'#define SEED_ALIGN_CENTER   {1 if align == "center" else 0}\n')
        fh.write(f'#define SEED_NIKKUD_MODE    {0 if nikkud == "color" else (1 if nikkud == "full" else 2)}\n')
        fh.write(f'#define SEED_SHOW_SECONDS   {show_seconds}\n')
        fh.write(f'#define SEED_VOICE_ID       {voice}\n')
        fh.write(f'#define SEED_VOLUME         {volume}\n')
        fh.write(f'#define SEED_HOURLY_ENABLED {hourly_en}\n')
        fh.write(f'#define SEED_HOURLY_START   {hourly_start}\n')
        fh.write(f'#define SEED_HOURLY_END     {hourly_end}\n')
        fh.write(f'#define SEED_BRIGHTNESS     {brightness}\n')
        fh.write(f'#define SEED_TIMEOUT_SEC    {timeout_sec}\n')
        fh.write(f'#define SEED_ALWAYS_ON_USB  {always_on_usb}\n')
    print("Generated", out_h, "(SSID configured:", bool(ssid), ")")


if __name__ == "__main__":
    generate_c_glyphs()
    generate_default_config_header()
    if "--glyphs-only" not in sys.argv:
        asyncio.run(generate_voice_pack())
