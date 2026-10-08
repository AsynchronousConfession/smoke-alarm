#pragma once

/*
 * 板级外设：背光（LEDC PWM）与 RGB 指示灯（LEDC PWM）
 */

/* 上电初始化：配置 LEDC 定时器与 4 个通道（背光 + RGB） */
void board_init(void);

/* 背光亮度 0..100 */
void board_backlight_set(int percent);

/* RGB 指示灯亮度 0..100（板上是低电平点亮，内部已做反相） */
void board_rgb_set(int r, int g, int b);
