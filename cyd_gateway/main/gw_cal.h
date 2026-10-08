#pragma once

/*
 * [产品化 阶段3] 清洁空气标定 R0（存在 NVS，按节点 MAC 索引）
 *
 * 为什么需要：环境类传感器（MQ-135/MQ-137）如果只看"相对自适应基线的 Δ%"，
 * 空气慢慢变差时基线会跟着漂，屏幕永远显示"优"。要么看绝对值 —— 即
 *     比值 = Rs / R0     （R0 = 这个探头在清洁空气里的电阻）
 * 这个比值只跟气体浓度有关，与模块的负载电阻 RL、供电电压的误差都无关（同一条公式里约掉）。
 *
 * 标定流程：把节点放在清洁空气里稳定几分钟 → 在屏幕控制页点"标定" →
 * 网关把此刻的 Rs 记成 R0 写进 NVS（掉电不丢）。以后换探头/换模块要重新标定。
 */

#include <stdbool.h>
#include <stdint.h>

/* 打开 NVS 命名空间（首次上电没有数据也正常） */
void gw_cal_init(void);

/* 读某个节点的 R0（mac12 = 12 位十六进制、无冒号）；没有标定过返回 false */
bool gw_cal_get(const char *mac12, uint32_t *r0_ohm);

/* 写/删某个节点的 R0 */
bool gw_cal_set(const char *mac12, uint32_t r0_ohm);
bool gw_cal_clear(const char *mac12);
