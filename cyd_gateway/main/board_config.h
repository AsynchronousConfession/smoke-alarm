#pragma once

/*
 * 硬件配置：ESP32-WROOM-32E (N4, 4MB) + 2.8 inch 240x320 LCD + 电阻触摸
 * 板型对应 ESP32-2432S028R（俗称 Cheap Yellow Display / CYD）
 * 引脚来源：https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/PINS.md
 *
 * 如果屏是黑屏/花屏/颜色反相，先改下面 LCD_SWAP_XY / LCD_MIRROR_X / LCD_MIRROR_Y /
 * LCD_INVERT_COLOR 四个开关，不需要动 main.c。
 */

/* ---------- LCD (SPI2 / HSPI) ---------- */
#define LCD_H_RES               240
#define LCD_V_RES               320
#define LCD_PIXEL_CLOCK_HZ      (40 * 1000 * 1000)
#define LCD_SPI_HOST            SPI2_HOST
#define LCD_PIN_SCLK            14
#define LCD_PIN_MOSI            13
#define LCD_PIN_MISO            12
#define LCD_PIN_CS              15
#define LCD_PIN_DC              2
#define LCD_PIN_RST             (-1)     /* 与 ESP32 的 EN 复用，软件不需要管 */
#define LCD_PIN_BL              21       /* 背光，高电平点亮 */

/* 方向 / 颜色微调开关（1 = 打开） */
#define LCD_SWAP_XY             0
#define LCD_MIRROR_X            1        /* CYD 批次差异：默认需要左右镜像，否则画面水平翻转 */
#define LCD_MIRROR_Y            0
#define LCD_INVERT_COLOR        0        /* 实测本板不需要反相；置 1 会变成"底片"效果 */

/* ---------- 触摸 XPT2046（独立 SPI3 / VSPI 总线） ---------- */
#define TOUCH_SPI_HOST          SPI3_HOST
#define TOUCH_PIN_SCLK          25
#define TOUCH_PIN_MOSI          32
#define TOUCH_PIN_MISO          39       /* 输入专用脚 */
#define TOUCH_PIN_CS            33
#define TOUCH_PIN_IRQ           36       /* 输入专用脚 */
#define TOUCH_X_MAX             LCD_H_RES
#define TOUCH_Y_MAX             LCD_V_RES
#define TOUCH_SWAP_XY           0
#define TOUCH_MIRROR_X          1        /* 实测：准星与手指水平方向相反，需要镜像 X */
#define TOUCH_MIRROR_Y          0

/* ---------- 板上其它外设 ---------- */
#define BOARD_PIN_BTN_BOOT      0        /* BOOT 按键，按下为低 */
#define BOARD_PIN_LED_R         4        /* RGB LED，低电平点亮 */
#define BOARD_PIN_LED_G         16
#define BOARD_PIN_LED_B         17
#define BOARD_PIN_SPEAKER_DAC   26       /* 经功放接到扬声器座 */
#define BOARD_PIN_LDR           34       /* 光敏电阻，输入专用脚 */

/* ---------- SD 卡（SPI3，与触摸共总线，本模板暂不初始化） ---------- */
#define SD_PIN_CS               5
#define SD_PIN_SCLK             18
#define SD_PIN_MISO             19
#define SD_PIN_MOSI             23

/* ---------- LVGL 缓冲 ---------- */
/* 缓冲区高度（行）。注意：本工程同时跑 BLE + WiFi，内存比纯显示工程紧张，
 * 所以用 24 行（双缓冲 240*24*2*2 ≈ 23KB），纯显示工程里用的是 40 行。 */
#define LVGL_BUF_LINES          24
