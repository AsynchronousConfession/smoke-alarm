#pragma once

/*
 * 22px 数字/符号字体（只有 0-9 . % + - 空格），用于监控页的 Δ% 大字。
 * 行高被压紧到 26（默认会按字体 ascent+descent 变成 33），这样能在卡片里
 * 只占一行、不与上下两行文字挤在一起。
 * 由 tools/gen_lvgl_font.py --only-chars --line-height 生成，体积约 1 KB。
 */

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_digit_22)
