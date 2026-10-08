#pragma once

/*
 * [CYD port] 界面读取网关运行状态的只读快照接口（实现在 gateway_main.c）
 *
 * 界面不直接碰网关的内部结构，只通过这里拿一份拷贝，避免两个任务同时改同一份数据。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GW_MAX_NODES 4

typedef struct {
    uint8_t  index;              /* 展示编号 1..N */
    bool     used;               /* 槽位已分配给某个节点 */
    bool     ready;              /* 服务发现完成、已订阅通知 */
    bool     have_data;          /* 收到过遥测 */

    char     mac[18];            /* AA:BB:CC:DD:EE:FF */
    char     tag[16];            /* 节点编号，如 N01 / 厨房 */
    char     model[16];          /* 传感器型号，如 MQ-2 */
    char     fw[16];             /* 节点固件版本 */

    int8_t   rssi;               /* 蓝牙信号强度 dBm */
    uint16_t adc;                /* ADC 原始值 0~4095 */
    uint16_t ao;                 /* 模块 AO 电压 mV */
    uint32_t rs;                 /* 传感器等效电阻 Ω */
    int16_t  dpct;               /* Δ% ×10，例如 -523 表示 -52.3% */

    uint8_t  alarm;
    uint8_t  warmup;
    uint8_t  do_level;
    uint8_t  muted;
    uint8_t  probe_fault;        /* 1 = 探头疑似未接/故障（ADC 长时间贴轨且零抖动） */

    uint16_t warmup_remain;      /* [产品化 阶段2] 预热剩余秒数（0 = 不在预热） */

    uint32_t r0_ohm;             /* [阶段3] 清洁空气标定值 R0；环境类用它算 Rs/R0 */
    uint8_t  cal_ok;             /* [阶段3] 1 = 已标定 */

    uint32_t tele_count;         /* 累计收到多少帧遥测 */
    int64_t  last_rx_us;         /* 最后一帧遥测的时刻（esp_timer_get_time） */
} gw_node_view_t;

/* 取所有槽位的快照，返回写入条数（含空槽位） */
int gw_nodes_snapshot(gw_node_view_t *out, int max_count);

/* 当前已就绪节点数 / 最大节点数 */
int gw_nodes_ready_count(void);
int gw_nodes_max(void);

/* 向指定编号的节点下发一条 ASCII 命令（走和云端下发完全相同的路径） */
bool gw_send_cmd_to_index(int index, const char *text);

/* [阶段3] 把指定节点当前的 Rs 记为 R0（清洁空气标定），写进 NVS。
 * 成功/失败都会把一句中文说明写进 msg，界面直接显示。 */
bool gw_calibrate_node(int index, char *msg, size_t msg_len);
