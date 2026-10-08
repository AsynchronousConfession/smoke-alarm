#include "gw_log.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static gw_event_t   s_buf[GW_LOG_MAX];
static int          s_head;        /* 下一个写入位置 */
static int          s_count;       /* 当前有效条数 */
static uint32_t     s_total;       /* 累计写入条数 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

void gw_log_push(uint8_t node, gw_event_type_t type, int16_t dpct, const char *text)
{
    /* 先取时间再进临界区：time() 可能走 libc 锁，不该在关中断的状态下调用 */
    const time_t now = time(NULL);

    taskENTER_CRITICAL(&s_lock);

    gw_event_t *e = &s_buf[s_head];
    memset(e, 0, sizeof(*e));
    e->ts   = now;
    e->node = node;
    e->type = (uint8_t)type;
    e->dpct = dpct;
    if (text != NULL) {
        strncpy(e->text, text, sizeof(e->text) - 1);
    }

    s_head = (s_head + 1) % GW_LOG_MAX;
    if (s_count < GW_LOG_MAX) {
        s_count++;
    }
    s_total++;

    taskEXIT_CRITICAL(&s_lock);
}

int gw_log_snapshot(gw_event_t *out, int max_count)
{
    if (out == NULL || max_count <= 0) {
        return 0;
    }

    int n = 0;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_count && n < max_count; i++) {
        /* 从最新往回取 */
        int idx = (s_head - 1 - i + GW_LOG_MAX * 2) % GW_LOG_MAX;
        out[n++] = s_buf[idx];
    }
    taskEXIT_CRITICAL(&s_lock);
    return n;
}

int gw_log_total(void)
{
    return (int)s_total;
}

static int count_by_type(int want_type, bool today_only)
{
    /* 关键：所有涉锁的 libc 调用（time/localtime/mktime）都必须在临界区之外完成。
     * 之前把 localtime_r() 写在临界区里，newlib 的 lock_acquire_generic() 会检测到
     * "关中断状态下申请锁"并直接 abort() —— 表现为"一报警就重启"。
     * 这里先算出今天 00:00 的时间戳，临界区里只做数值比较。 */
    time_t today_start = 0;
    if (today_only) {
        const time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        tm_now.tm_hour = 0;
        tm_now.tm_min  = 0;
        tm_now.tm_sec  = 0;
        today_start = mktime(&tm_now);
    }

    int cnt = 0;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_count; i++) {
        const gw_event_t *e = &s_buf[i];
        if (e->type != want_type) {
            continue;
        }
        if (today_only && e->ts < today_start) {
            continue;
        }
        cnt++;
    }
    taskEXIT_CRITICAL(&s_lock);
    return cnt;
}

int gw_log_alarm_today(void)
{
    return count_by_type(GW_EV_ALARM, true);
}

int gw_log_alarm_total(void)
{
    return count_by_type(GW_EV_ALARM, false);
}

void gw_log_clear(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_head  = 0;
    s_count = 0;
    taskEXIT_CRITICAL(&s_lock);
}
