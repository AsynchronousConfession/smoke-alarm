#pragma once

/*
 * 16px 中文点阵字体（Noto Sans SC，OFL 许可）
 * 由 tools/gen_lvgl_font.py 生成：ASCII + GB2312 一级字库(3755 字) + 工程内出现的所有汉字
 *
 * 需要新增/修改界面文字后，重新生成一遍即可（命令见 README）：
 *   python gen_lvgl_font.py --font NotoSansSC-Regular.otf --size 16 \
 *       --name lv_font_noto_sc_16 --extra-scan ../main --out ../main/fonts/lv_font_noto_sc_16.c
 */

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_noto_sc_16)
