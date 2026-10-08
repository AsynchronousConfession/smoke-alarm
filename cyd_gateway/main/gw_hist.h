#pragma once

/*
 * [CYD port] 节点参数历史（每节点一个环形缓冲，供监控页的曲线图使用）
 *
 * 每收到一帧遥测就记一点：正常 2 秒一帧、报警 0.5 秒一帧，最多保留 60 点。
 * 内存占用：4 节点 × 4 参数 × 60 点 × 2 字节 ≈ 1.9 KB
 */

#include <stdint.h>

#define GW_HIST_NODES   4
#define GW_HIST_POINTS  60

/* 记录的参数 */
enum {
    GW_H_SER_DPCT = 0,   /* Δ% ×10（例如 -523 表示 -52.3%） */
    GW_H_SER_RS,         /* 传感器等效电阻，单位 10Ω（避免超过 int16） */
    GW_H_SER_ADC,        /* ADC 原始值 */
    GW_H_SER_RSSI,       /* 蓝牙信号强度 dBm */
    GW_H_SER_COUNT,
};

/* 记录一点（节点编号 1..N） */
void gw_hist_push(uint8_t node_index, int16_t dpct_x10, uint16_t adc, uint32_t rs_ohm, int8_t rssi);

/* 已有的点数 */
int gw_hist_count(uint8_t node_index);

/* 按时间顺序（旧 → 新）取某个参数，返回实际点数；不足 max 时只写前面的 n 个 */
int gw_hist_get(uint8_t node_index, int series, int16_t *out, int max_count);

/* 清空某节点的历史 */
void gw_hist_clear(uint8_t node_index);
