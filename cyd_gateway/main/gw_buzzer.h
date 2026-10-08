#pragma once

/*
 * [产品化 阶段2] 网关本机报警声
 *
 * 硬件：CYD 板载功放输入 IO26（BOARD_PIN_SPEAKER_DAC），需要在扬声器座插上喇叭。
 * 实现：用 LEDC 在 IO26 上输出 2.7kHz 方波（方波经功放推喇叭就是"嘀"声），
 *       节奏由 50ms 的 esp_timer 状态机驱动，不占任务、不阻塞。
 *
 * 用法：界面每 500ms 调一次 gw_buzzer_set_alarm(有节点报警, 已消音)。
 */

#include <stdbool.h>

/* 初始化 LEDC 通道与节奏定时器（在 app_main 里调一次） */
void gw_buzzer_init(void);

/* on=1 时按"嘀 嘀 嘀 — 停"循环，on=0 立即静音 */
void gw_buzzer_set(bool on);

/* 自检：单独"嘀"约 0.6 秒（控制页点"自检"时调用，用来确认板载喇叭是通的） */
void gw_buzzer_test(void);
