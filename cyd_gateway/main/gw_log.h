#pragma once

/*
 * [CYD port] 事件记录（环形缓冲，界面“记录”页用）
 *
 * 记录报警/解除/上线/离线/下发/回执六类事件，掉电不保存（需要持久化可以再加 SPIFFS）。
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define GW_LOG_MAX 64

typedef enum {
    GW_EV_ALARM = 0,     /* 报警 */
    GW_EV_CLEAR,         /* 报警解除 */
    GW_EV_ONLINE,        /* 节点上线 */
    GW_EV_OFFLINE,       /* 节点离线 */
    GW_EV_CMD,           /* 下发指令 */
    GW_EV_ACK,           /* 指令回执 */
} gw_event_type_t;

typedef struct {
    time_t   ts;         /* 墙上时间（SNTP 对时后有效） */
    uint8_t  node;       /* 节点编号 1..N */
    uint8_t  type;       /* gw_event_type_t */
    int16_t  dpct;       /* 报警/解除时的 Δ%×10 */
    char     text[20];   /* 附加文本（命令内容等） */
} gw_event_t;

/* 记录一条事件（任务安全） */
void gw_log_push(uint8_t node, gw_event_type_t type, int16_t dpct, const char *text);

/* 取最新的事件（最新在前），返回写入条数 */
int gw_log_snapshot(gw_event_t *out, int max_count);

/* 累计条数（含被覆盖的）与今日报警次数 */
int gw_log_total(void);
int gw_log_alarm_today(void);
int gw_log_alarm_total(void);

void gw_log_clear(void);
