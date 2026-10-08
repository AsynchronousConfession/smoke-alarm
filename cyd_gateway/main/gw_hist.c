#include "gw_hist.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static int16_t       s_data[GW_HIST_NODES][GW_H_SER_COUNT][GW_HIST_POINTS];
static uint16_t      s_head[GW_HIST_NODES];
static uint16_t      s_count[GW_HIST_NODES];
static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;

void gw_hist_push(uint8_t node_index, int16_t dpct_x10, uint16_t adc, uint32_t rs_ohm, int8_t rssi)
{
    if (node_index < 1 || node_index > GW_HIST_NODES) {
        return;
    }
    const int n = node_index - 1;

    int32_t rs10 = (int32_t)(rs_ohm / 10);          /* 10Ω 为单位 */
    if (rs10 > 32767) {
        rs10 = 32767;
    }

    taskENTER_CRITICAL(&s_lock);
    const uint16_t h = s_head[n];
    s_data[n][GW_H_SER_DPCT][h] = dpct_x10;
    s_data[n][GW_H_SER_RS][h]   = (int16_t)rs10;
    s_data[n][GW_H_SER_ADC][h]  = (int16_t)adc;
    s_data[n][GW_H_SER_RSSI][h] = rssi;
    s_head[n] = (uint16_t)((h + 1) % GW_HIST_POINTS);
    if (s_count[n] < GW_HIST_POINTS) {
        s_count[n]++;
    }
    taskEXIT_CRITICAL(&s_lock);
}

int gw_hist_count(uint8_t node_index)
{
    if (node_index < 1 || node_index > GW_HIST_NODES) {
        return 0;
    }
    return (int)s_count[node_index - 1];
}

int gw_hist_get(uint8_t node_index, int series, int16_t *out, int max_count)
{
    if (node_index < 1 || node_index > GW_HIST_NODES ||
        series < 0 || series >= GW_H_SER_COUNT || out == NULL || max_count <= 0) {
        return 0;
    }
    const int n = node_index - 1;
    int got = 0;

    taskENTER_CRITICAL(&s_lock);
    const uint16_t cnt = s_count[n];
    const uint16_t head = s_head[n];
    /* 最老的一点位置 */
    const uint16_t start = (uint16_t)((head + GW_HIST_POINTS - cnt) % GW_HIST_POINTS);
    for (uint16_t i = 0; i < cnt && got < max_count; i++) {
        out[got++] = s_data[n][series][(start + i) % GW_HIST_POINTS];
    }
    taskEXIT_CRITICAL(&s_lock);
    return got;
}

void gw_hist_clear(uint8_t node_index)
{
    if (node_index < 1 || node_index > GW_HIST_NODES) {
        return;
    }
    const int n = node_index - 1;
    taskENTER_CRITICAL(&s_lock);
    s_head[n]  = 0;
    s_count[n] = 0;
    taskEXIT_CRITICAL(&s_lock);
}
