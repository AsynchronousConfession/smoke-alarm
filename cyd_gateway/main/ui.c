/*
 * [CYD port] 界面：清新浅色主题 + 三个页签
 *
 *   监控：顶部状态条（一切正常 / N 个探头异常 / 报警时整条红闪）+ 4 张节点卡片
 *         每张卡片三行：
 *           第1行  节点N 房间名 · 传感器      |  等级徽标（优/良/轻度/中度/重度 或 正常/注意/报警）
 *           第2行  指数 0~100 + 等级色条 + 趋势（↑变差 / →平稳 / ↓好转）
 *           第3行  Δ% + 更新时间；报警时右边多一个"消音"按钮
 *         换算规则在 s_sensors[] 和 level_word()；探头没接好会显示"故障"而不是"正常"
 *   记录：今日报警次数 + 最近事件流（报警/解除/上线/离线/下发/回执）
 *   控制：选节点 → 消音 60s / 自检 / 复位（走和云端下发完全相同的路径）
 *
 * 数据来源：gw_view.h 的节点快照 + gw_hist.h 的 Δ% 历史 + gw_log.h 的事件环形缓冲，刷新 500 ms。
 * 阶段2：顶栏时钟（SNTP，没对上显示 --:--）、预热倒计时、报警声（gw_buzzer.c，IO26 功放）、
 *        夜间 22:00~07:00 自动降背光（报警时保持全亮）。
 * 阶段3：环境类（MQ-135/137）改用 Rs/R0 绝对基准（要先用控制页"标定"记录清洁空气 R0，
 *        存在 gw_cal.c 的 NVS 里）；安全类（MQ-2）仍用 Δ% 抓突发。
 * 阶段4：第 4 个页签「设置」（背光滑块 + 夜间自动开关 + 网关信息）、
 *        开机自检页（3.5 秒后自动删除回收内存）、报警时自动弹全屏大屏（含消音/关闭）。
 * 板上 RGB 灯：绿常亮=正常、红闪=有节点报警、蓝闪=网络断开。
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

#include <time.h>

#include "app_config.h"

#include "board.h"
#include "gw_buzzer.h"
#include "gw_hist.h"
#include "gw_log.h"
#include "gw_view.h"
#include "wifi_mqtt.h"
#include "fonts/lv_font_noto_sc_16.h"
#include "fonts/lv_font_digit_22.h"

static const char *TAG = "UI";

#define UI_FONT  (&lv_font_noto_sc_16)     /* 中文（含 ASCII） */
#define BIG_FONT (&lv_font_digit_22)       /* Δ% 大字：只有数字和符号，很小 */

/* ---------------- 清新浅色配色 ---------------- */
#define C_BG         lv_color_hex(0xF2F7FB)   /* 页面底色：淡蓝白 */
#define C_CARD       lv_color_hex(0xFFFFFF)   /* 卡片：白 */
#define C_BORDER     lv_color_hex(0xDCE7F0)
#define C_HDR        lv_color_hex(0xD6EAF7)   /* 顶栏：淡蓝 */
#define C_TEXT       lv_color_hex(0x2C3E50)   /* 主文字：深蓝灰 */
#define C_DIM        lv_color_hex(0x8FA3B4)   /* 次要文字 */
#define C_ACCENT     lv_color_hex(0x2E86C1)   /* 强调：蓝 */
#define C_OK         lv_color_hex(0x27AE60)   /* 正常：绿 */
#define C_WARN       lv_color_hex(0xE67E22)   /* 预热/消音：橙 */
#define C_ALARM      lv_color_hex(0xC0392B)   /* 报警：红 */
#define C_ALARM_BG   lv_color_hex(0xFBDDD8)   /* 报警卡片底 */
#define C_ALARM_BG2  lv_color_hex(0xF6C1B9)
#define C_BTN        lv_color_hex(0xE8F2FA)   /* 按钮底色 */
#define C_TRACK      lv_color_hex(0xE3ECF3)   /* 等级色条轨道 */
#define BAR_W        106                      /* 等级色条最大宽度(px)，卡片第 2 行 */

#define TAB_H        32

typedef struct {
    lv_obj_t *card;
    lv_obj_t *name;     /* 第 1 行：节点N 房间名 · 传感器名 */
    lv_obj_t *badge;    /* 第 1 行右：等级词（优/良/轻度/中度/重度 或 正常/注意/报警/预热/离线/故障） */
    lv_obj_t *metric;   /* 第 2 行："指数" */
    lv_obj_t *big;      /* 第 2 行：指数数值（22px 数字字体） */
    lv_obj_t *bar_bg;   /* 第 2 行：等级色条轨道 */
    lv_obj_t *bar_fill; /*          色条本体，长度 = 指数，颜色 = 等级 */
    lv_obj_t *trend;    /* 第 2 行右：趋势  ↑ 变差 / → 平稳 / ↓ 好转 */
    lv_obj_t *info;     /* 第 3 行：Δ% + 更新时间，或故障提示 */
    lv_obj_t *mute;     /* 第 3 行右：报警时出现的"消音"按钮 */
} card_t;

static lv_obj_t *s_hdr;
static lv_obj_t *s_hdr_lbl;      /* 顶栏状态文字（报警 / 探头异常 / 一切正常） */
static lv_obj_t *s_hdr_time;     /* 顶栏左侧时钟（SNTP 没对上时 --:--） */
static int64_t   s_local_mute_until_us;   /* 本机"消音"后暂时不响的截止时刻 */
static card_t    s_card[GW_MAX_NODES];
static lv_obj_t *s_tab_monitor;
static lv_obj_t *s_tab_records;
static lv_obj_t *s_tab_control;
static lv_obj_t *s_tab_settings;

#define REC_LINES 10
static lv_obj_t *s_rec_stat;
static lv_obj_t *s_rec_lines[REC_LINES];
static int       s_rec_rendered = -1;

static lv_obj_t *s_ctl_btn[GW_MAX_NODES];
static lv_obj_t *s_ctl_status;
static int       s_ctl_sel = 1;

/* ---------------- [阶段4] 设置页 ---------------- */
static lv_obj_t *s_set_slider;
static lv_obj_t *s_set_bright_lbl;
static lv_obj_t *s_set_auto_sw;
static lv_obj_t *s_set_info[6];
static int       s_bright     = 100;    /* 手动亮度 0~100 */
static bool      s_night_auto = true;   /* 夜间（22:00~07:00）自动降到 25% */

/* ---------------- [阶段4] 开机自检页（显示几秒后自己删掉，回收内存） ---------------- */
static lv_obj_t *s_splash;
static lv_obj_t *s_splash_note[3];
static int       s_splash_ticks;

/* ---------------- [阶段4] 报警自动弹大屏 ---------------- */
static lv_obj_t *s_alarm_pop;
static lv_obj_t *s_alarm_box;
static lv_obj_t *s_alarm_band;
static lv_obj_t *s_alarm_band_lbl;
static lv_obj_t *s_alarm_room;
static lv_obj_t *s_alarm_val;
static int       s_alarm_pop_node;
static bool      s_alarm_pop_suppressed;   /* 用户点了"关闭/消音"后，本次报警不再自动弹 */

static uint32_t  s_blink;

/* ---------------- 节点详情（覆盖全屏的曲线浮层） ---------------- */
static lv_obj_t          *s_detail;          /* 浮层容器，默认隐藏 */
static lv_obj_t          *s_detail_title;
static lv_obj_t          *s_detail_vals;
static lv_obj_t          *s_detail_vals2;
static lv_obj_t          *s_detail_ch1_lbl;   /* Δ% 曲线标题（含传感器含义） */
static lv_obj_t          *s_detail_ch2_lbl;   /* Rs 曲线标题 */
static lv_obj_t          *s_detail_note;      /* 底部：该传感器的数据含义说明 */
static lv_obj_t          *s_chart_dpct;
static lv_obj_t          *s_chart_rs;
static lv_chart_series_t *s_ser_dpct;
static lv_chart_series_t *s_ser_rs;
static lv_coord_t         s_arr_dpct[GW_HIST_POINTS];   /* 图表直接引用这两个数组 */
static lv_coord_t         s_arr_rs[GW_HIST_POINTS];
static int                s_detail_node;      /* 0 = 未打开 */
static int                s_detail_pts = -1;  /* 上次画了多少点，避免重复刷新 */

/* 前置声明（曲线浮层在文件后面定义） */
static void card_click_cb(lv_event_t *e);
static void detail_open(int node_index);
static void refresh_detail(void);
void ui_check_overflow(lv_obj_t *root, const char *tag);

/* ------------------------------------------------------------------ */
/* 布局自检：遍历所有标签，把超出屏幕或超出父容器的直接报出来          */
/* （改布局时很有用；开机和打开详情页各跑一次）                        */
/* ------------------------------------------------------------------ */
void ui_check_overflow(lv_obj_t *root, const char *tag)
{
    lv_area_t root_a;
    lv_obj_get_coords(root, &root_a);
    const uint32_t cnt = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *ch = lv_obj_get_child(root, i);
        if (lv_obj_check_type(ch, &lv_label_class)) {
            lv_area_t a;
            lv_obj_get_coords(ch, &a);
            const char *txt = lv_label_get_text(ch);
            /* 与"直接父容器"比较：既不会把未激活页签算成越界，也能抓到卡片内的挤压 */
            if (a.x2 > root_a.x2 || a.y2 > root_a.y2 ||
                a.x1 < root_a.x1 || a.y1 < root_a.y1) {
                ESP_LOGW(TAG, "[%s] 文字超出容器: x=%d..%d y=%d..%d (容器 %d..%d,%d..%d) \"%s\"",
                         tag, (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2,
                         (int)root_a.x1, (int)root_a.x2, (int)root_a.y1, (int)root_a.y2, txt);
            }
        }
        ui_check_overflow(ch, tag);
    }
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static void fmt_rs(uint32_t rs, char *out, size_t n)
{
    if (rs >= 10000) {
        snprintf(out, n, "%.1fk", (double)rs / 1000.0);
    } else if (rs >= 1000) {
        snprintf(out, n, "%.2fk", (double)rs / 1000.0);
    } else {
        snprintf(out, n, "%lu", (unsigned long)rs);
    }
}

static void fmt_dpct(int16_t dpct_x10, char *out, size_t n)
{
    const int a = dpct_x10 < 0 ? -dpct_x10 : dpct_x10;
    snprintf(out, n, "%s%d.%d%%", dpct_x10 < 0 ? "-" : "+", a / 10, a % 10);
}

/* ------------------------------------------------------------------ */
/* 传感器型号 -> 屏幕上的"数据含义"                                     */
/*                                                                     */
/* 型号来自节点的信息帧（node_cfg.h 的 NODE_MODEL），这里把它翻译成人话：*/
/*   short_name : 通俗名，卡片第 1 行显示（空气质量 / 氨气 / 可燃气）    */
/*   gas        : 详情页 Δ% 曲线标题里的被测对象                        */
/*   desc       : 详情页底部的说明（<= 14 个汉字，240px 一行放得下）     */
/*   is_safety  : 安全类走 正常/注意/报警；环境类走 优/良/轻度/中度/重度  */
/*   thr_x10    : 【安全类】指数 100 对应的 |Δ%|×10（250 = 25%，与节点 NODE_ALARM_PCT 一致） */
/*   ratio_min_x100 : 【环境类】指数 100 对应的 Rs/R0（×100），30 = Rs 掉到清洁空气的 30% */
/*                                                                     */
/* 为什么两类要用不同的量：                                              */
/*   安全类（MQ-2）要抓的是"几秒内突然出现可燃气"，所以看 Δ%（相对自适应  */
/*   基线）最灵敏；                                                     */
/*   环境类（MQ-135/137）看的是"空气长期有多差"，如果也用 Δ%，慢慢变差的   */
/*   空气会被基线跟着漂掉，屏幕永远显示"优"。所以环境类看 Rs/R0 这个绝对   */
/*   比值（R0 = 清洁空气标定值，见 gw_cal.c），它跟模块的 RL/供电误差无关。 */
/* 两种情况的共同点还是：Rs 越小 -> 被测气体越浓。换新型号时加一行即可。  */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *model;           /* 型号前缀，对应节点的 NODE_MODEL */
    const char *short_name;      /* 通俗名：空气质量 / 氨气 / 可燃气 */
    const char *gas;             /* 详情页曲线标题里的被测对象 */
    const char *desc;            /* 详情页说明 */
    uint8_t     is_safety;       /* 1 = 安全类（Δ%）；0 = 环境类（Rs/R0） */
    int16_t     thr_x10;         /* 安全类：指数 100 对应的 |Δ%|×10 */
    int16_t     ratio_min_x100;  /* 环境类：指数 100 对应的 Rs/R0 ×100 */
} sensor_info_t;

static const sensor_info_t s_sensors[] = {
    { "MQ-135", "空气质量", "空气质量", "浓度越高，Rs/R0 越小",       0, 250, 30 },
    { "MQ-137", "氨气",     "氨气 NH3", "氨气 NH3 越浓，Rs/R0 越小",  0, 250, 30 },
    { "MQ-2",   "可燃气",   "可燃气",   "可燃气/烟雾越浓，Δ% 越负",   1, 250,  0 },
    { "MQ-7",   "一氧化碳", "CO 浓度",  "一氧化碳越浓，Δ% 越负",      1, 250,  0 },
    { NULL,     "气体",     "气体浓度", "浓度越高，读数越低",         0, 250, 30 },  /* 兜底，放最后 */
};

/* 按型号前缀查表；型号不认识（或还没收到信息帧）就返回最后那行兜底文案 */
static const sensor_info_t *sensor_lookup(const char *model)
{
    const int n = (int)(sizeof(s_sensors) / sizeof(s_sensors[0]));
    if (model != NULL && model[0] != '\0') {
        for (int i = 0; i < n; i++) {
            if (s_sensors[i].model == NULL) {
                continue;
            }
            if (strncmp(model, s_sensors[i].model, strlen(s_sensors[i].model)) == 0) {
                return &s_sensors[i];
            }
        }
    }
    return &s_sensors[n - 1];
}

/* ---- 产品化：把工程数据翻译成用户能看懂的"指数 + 等级 + 趋势" ---- */

/* 指数 0~100：
 *   安全类（MQ-2/MQ-7）：|Δ%| 映射，100 = 节点的报警线（抓突发）；
 *   环境类（MQ-135/137）：Rs/R0 映射，100 = Rs 掉到 R0 的 ratio_min（默认 30%）。
 * 环境类没标定过就没法算指数，此时 need_cal 置 1，界面显示"未标定"。 */
static int sensor_index(const gw_node_view_t *v, const sensor_info_t *si, bool *need_cal)
{
    if (need_cal != NULL) {
        *need_cal = false;
    }

    if (!si->is_safety) {
        if (!v->cal_ok || v->r0_ohm == 0) {
            if (need_cal != NULL) {
                *need_cal = true;
            }
            return 0;
        }
        const int ratio = (int)((uint64_t)v->rs * 100 / v->r0_ohm);   /* Rs/R0 ×100 */
        const int span  = 100 - si->ratio_min_x100;
        if (span <= 0) {
            return 0;
        }
        int idx = (100 - ratio) * 100 / span;
        if (idx > 100) idx = 100;
        if (idx < 0)   idx = 0;
        return idx;
    }

    const int a = (v->dpct < 0) ? -v->dpct : v->dpct;   /* Δ%×10 的绝对值 */
    if (si->thr_x10 <= 0) {
        return 0;
    }
    int idx = a * 100 / si->thr_x10;
    if (idx > 100) idx = 100;
    if (idx < 0)   idx = 0;
    return idx;
}

/* 等级词 + 颜色：安全类 正常/注意/报警；环境类 优/良/轻度/中度/重度 */
static const char *level_word(const sensor_info_t *si, int index, uint8_t alarm, lv_color_t *color)
{
    if (alarm) {
        *color = C_ALARM;
        return "报警";
    }
    if (si->is_safety) {
        if (index >= 80) { *color = C_ALARM; return "报警"; }
        if (index >= 50) { *color = C_WARN;  return "注意"; }
        *color = C_OK;
        return "正常";
    }
    if (index >= 80) { *color = C_ALARM;  return "重度"; }
    if (index >= 60) { *color = C_WARN;   return "中度"; }
    if (index >= 40) { *color = C_WARN;   return "轻度"; }
    if (index >= 20) { *color = C_ACCENT; return "良"; }
    *color = C_OK;
    return "优";
}

/* 从 Δ% 历史里算趋势和峰值：返回 -1 好转 / 0 平稳 / +1 变差 / +2 数据不足 */
static int hist_trend_peak(int node, int *peak_x10)
{
    int16_t a[GW_HIST_POINTS];
    const int got = gw_hist_get((uint8_t)node, GW_H_SER_DPCT, a, GW_HIST_POINTS);
    int peak = 0;
    for (int i = 0; i < got; i++) {
        const int t = (a[i] < 0) ? -a[i] : a[i];
        if (t > peak) {
            peak = t;
        }
    }
    if (peak_x10 != NULL) {
        *peak_x10 = peak;
    }
    if (got < 6) {
        return 2;
    }
    int recent = 0, older = 0;
    for (int i = got - 1; i >= got - 3; i--) recent += (a[i] < 0) ? -a[i] : a[i];
    for (int i = got - 4; i >= got - 6; i--) older  += (a[i] < 0) ? -a[i] : a[i];
    recent /= 3;
    older  /= 3;
    if (recent > older + 10) return 1;      /* 平均差 1.0% 以上才算"变差/好转" */
    if (recent < older - 10) return -1;
    return 0;
}

/* "刚刚 / N 秒前 / N 分钟前 / N 小时前" */
static void fmt_age(int64_t age_us, char *out, size_t n)
{
    const int s = (int)(age_us / 1000000);
    if (s <= 3) {
        snprintf(out, n, "刚刚");
    } else if (s < 60) {
        snprintf(out, n, "%d 秒前", s);
    } else if (s < 3600) {
        snprintf(out, n, "%d 分钟前", s / 60);
    } else {
        snprintf(out, n, "%d 小时前", s / 3600);
    }
}

/* 报警卡上的"消音"按钮：走和云端下发完全相同的路径 */
static void card_mute_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const bool ok = gw_send_cmd_to_index(idx, "M");
    if (ok) {
        /* 本机立刻闭嘴，不必等 MUTED 标志从节点回传（最多要 2 秒） */
        s_local_mute_until_us = esp_timer_get_time() + 60LL * 1000000;
    }
    ESP_LOGI(TAG, "卡片[消音] 节点%d %s", idx, ok ? "已下发" : "失败（节点未就绪）");
}

static const char *ev_name(uint8_t type)
{
    switch (type) {
    case GW_EV_ALARM:   return "报警";
    case GW_EV_CLEAR:   return "解除";
    case GW_EV_ONLINE:  return "上线";
    case GW_EV_OFFLINE: return "离线";
    case GW_EV_CMD:     return "下发";
    default:            return "回执";
    }
}

static lv_color_t ev_color(uint8_t type)
{
    switch (type) {
    case GW_EV_ALARM:   return C_ALARM;
    case GW_EV_CLEAR:   return C_OK;
    case GW_EV_ONLINE:  return C_ACCENT;
    case GW_EV_OFFLINE: return C_DIM;
    case GW_EV_CMD:     return C_WARN;
    default:            return C_DIM;
    }
}

/* 页面统一风格 */
static lv_obj_t *page_style(lv_obj_t *tab)
{
    lv_obj_set_style_bg_color(tab, C_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(tab, UI_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(tab, C_TEXT, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tab, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
    return tab;
}

/* ------------------------------------------------------------------ */
/* 监控页                                                              */
/* ------------------------------------------------------------------ */

static void card_create(int i)
{
    lv_obj_t *parent = s_hdr;              /* s_hdr 就是监控页本身 */

    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    /* 内容区高 288 = 状态条 24 + 4×64(卡片) + 4×2(间隔)
     * 卡内三行（中文字体行高已压到 20）：
     *   第1行 名称 + 等级徽标(20) → 第2行 指数 + 等级色条 + 趋势(24) → 第3行 Δ%/更新时间 + 消音(20) */
    lv_obj_set_size(c, 232, 64);
    lv_obj_set_pos(c, 4, 26 + i * 66);
    lv_obj_set_style_radius(c, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(c, C_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(c, C_BORDER, LV_PART_MAIN);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);          /* 可点击 → 进详情曲线 */
    lv_obj_add_event_cb(c, card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    s_card[i].card = c;

    s_card[i].name = lv_label_create(c);
    lv_label_set_text(s_card[i].name, "节点");
    lv_obj_set_style_text_color(s_card[i].name, C_TEXT, LV_PART_MAIN);
    lv_obj_align(s_card[i].name, LV_ALIGN_TOP_LEFT, 8, 0);

    s_card[i].badge = lv_label_create(c);
    lv_label_set_text(s_card[i].badge, "等待");
    lv_obj_set_style_text_color(s_card[i].badge, C_DIM, LV_PART_MAIN);
    lv_obj_align(s_card[i].badge, LV_ALIGN_TOP_RIGHT, -8, 0);

    /* 第 2 行：指数 = 数字（22px）+ 等级色条 + 趋势。位置全部固定，避免数字位数变化时抖动 */
    s_card[i].metric = lv_label_create(c);
    lv_label_set_text(s_card[i].metric, "指数");
    lv_obj_set_style_text_color(s_card[i].metric, C_DIM, LV_PART_MAIN);
    lv_obj_align(s_card[i].metric, LV_ALIGN_TOP_LEFT, 8, 21);

    s_card[i].big = lv_label_create(c);
    lv_obj_set_style_text_font(s_card[i].big, BIG_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_card[i].big, C_DIM, LV_PART_MAIN);
    lv_label_set_text(s_card[i].big, "--");
    lv_obj_align(s_card[i].big, LV_ALIGN_TOP_LEFT, 44, 20);

    s_card[i].bar_bg = lv_obj_create(c);
    lv_obj_remove_style_all(s_card[i].bar_bg);
    lv_obj_set_size(s_card[i].bar_bg, BAR_W, 8);
    lv_obj_set_style_radius(s_card[i].bar_bg, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_card[i].bar_bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_card[i].bar_bg, C_TRACK, LV_PART_MAIN);
    lv_obj_align(s_card[i].bar_bg, LV_ALIGN_TOP_LEFT, 86, 28);
    lv_obj_clear_flag(s_card[i].bar_bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_card[i].bar_bg, LV_OBJ_FLAG_SCROLLABLE);

    s_card[i].bar_fill = lv_obj_create(s_card[i].bar_bg);
    lv_obj_remove_style_all(s_card[i].bar_fill);
    lv_obj_set_size(s_card[i].bar_fill, 0, 8);
    lv_obj_set_style_radius(s_card[i].bar_fill, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_card[i].bar_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_card[i].bar_fill, C_OK, LV_PART_MAIN);
    lv_obj_set_pos(s_card[i].bar_fill, 0, 0);
    lv_obj_clear_flag(s_card[i].bar_fill, LV_OBJ_FLAG_CLICKABLE);

    s_card[i].trend = lv_label_create(c);
    lv_label_set_text(s_card[i].trend, "");
    lv_obj_set_style_text_color(s_card[i].trend, C_DIM, LV_PART_MAIN);
    lv_obj_align(s_card[i].trend, LV_ALIGN_TOP_RIGHT, -8, 22);

    /* 第 3 行：Δ% + 更新时间；报警时右边出现"消音"按钮 */
    s_card[i].info = lv_label_create(c);
    lv_obj_set_style_text_color(s_card[i].info, C_DIM, LV_PART_MAIN);
    lv_label_set_text(s_card[i].info, "");
    /* 卡片有 1px 边框，子控件会整体内缩 1px，所以这里用 42 而不是 44 */
    lv_obj_align(s_card[i].info, LV_ALIGN_TOP_LEFT, 8, 42);

    s_card[i].mute = lv_btn_create(c);
    lv_obj_set_size(s_card[i].mute, 56, 20);
    lv_obj_set_style_radius(s_card[i].mute, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_card[i].mute, C_ALARM, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_card[i].mute, 0, LV_PART_MAIN);
    lv_obj_align(s_card[i].mute, LV_ALIGN_BOTTOM_RIGHT, -6, -2);
    lv_obj_add_event_cb(s_card[i].mute, card_mute_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
    lv_obj_add_flag(s_card[i].mute, LV_OBJ_FLAG_HIDDEN);
    {
        lv_obj_t *ml = lv_label_create(s_card[i].mute);
        lv_label_set_text(ml, "消音");
        lv_obj_set_style_text_color(ml, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(ml);
    }

    /* 子标签不接收点击，保证点卡片任意位置都能进详情（消音按钮自己收点击） */
    lv_obj_clear_flag(s_card[i].name,   LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_card[i].badge,  LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_card[i].metric, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_card[i].big,    LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_card[i].trend,  LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_card[i].info,   LV_OBJ_FLAG_CLICKABLE);
}

static void build_monitor_page(lv_obj_t *tab)
{
    page_style(tab);

    s_hdr = tab;                            /* 卡片直接挂在页面上 */

    /* 顶部状态条（报警时整条变红闪） */
    lv_obj_t *bar = lv_obj_create(tab);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 24);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, C_HDR, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    /* 左侧：时钟 */
    s_hdr_time = lv_label_create(bar);
    lv_obj_set_style_text_color(s_hdr_time, C_TEXT, LV_PART_MAIN);
    lv_label_set_text(s_hdr_time, "--:--");
    lv_obj_align(s_hdr_time, LV_ALIGN_LEFT_MID, 6, 0);

    /* 时钟后面：整体状态（左对齐，别和时钟打架） */
    s_hdr_lbl = lv_label_create(bar);
    lv_obj_set_style_text_color(s_hdr_lbl, C_TEXT, LV_PART_MAIN);
    lv_label_set_text(s_hdr_lbl, "启动中");
    lv_obj_align(s_hdr_lbl, LV_ALIGN_LEFT_MID, 52, 0);

    /* 卡片区的父对象：为了 4 张卡片的绝对定位，换成一个从 y=27 开始的容器 */
    for (int i = 0; i < GW_MAX_NODES; i++) {
        card_create(i);
    }
}

/* ------------------------------------------------------------------ */
/* 记录页                                                              */
/* ------------------------------------------------------------------ */

static void build_records_page(lv_obj_t *tab)
{
    page_style(tab);

    s_rec_stat = lv_label_create(tab);
    lv_label_set_text(s_rec_stat, "今日报警 0 次");
    lv_obj_set_style_text_color(s_rec_stat, C_ACCENT, LV_PART_MAIN);
    lv_obj_set_pos(s_rec_stat, 10, 6);

    lv_obj_t *line = lv_obj_create(tab);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 220, 1);
    lv_obj_set_pos(line, 10, 32);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(line, C_BORDER, LV_PART_MAIN);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < REC_LINES; i++) {
        s_rec_lines[i] = lv_label_create(tab);
        lv_label_set_text(s_rec_lines[i], "");
        lv_obj_set_style_text_color(s_rec_lines[i], C_DIM, LV_PART_MAIN);
        lv_obj_set_pos(s_rec_lines[i], 10, 38 + i * 24);
    }
}

/* ------------------------------------------------------------------ */
/* 控制页                                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 节点详情：曲线浮层                                                   */
/* ------------------------------------------------------------------ */

static lv_obj_t *chart_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                              lv_coord_t w, lv_coord_t h, lv_coord_t min, lv_coord_t max)
{
    lv_obj_t *ch = lv_chart_create(parent);
    lv_obj_set_size(ch, w, h);
    lv_obj_set_pos(ch, x, y);
    lv_chart_set_type(ch, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ch, GW_HIST_POINTS);
    lv_chart_set_range(ch, LV_CHART_AXIS_PRIMARY_Y, min, max);
    lv_chart_set_div_line_count(ch, 4, 6);
    lv_chart_set_update_mode(ch, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_color(ch, C_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(ch, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(ch, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_radius(ch, 6, LV_PART_MAIN);
    lv_obj_set_style_line_width(ch, 2, LV_PART_ITEMS);      /* 折线粗细 */
    lv_obj_set_style_width(ch, 0, LV_PART_INDICATOR);       /* 不画数据点 */
    lv_obj_set_style_height(ch, 0, LV_PART_INDICATOR);
    return ch;
}

static lv_obj_t *detail_label(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, C_DIM, LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void detail_close(void)
{
    ESP_LOGI(TAG, "关闭 节点%d 曲线", s_detail_node);
    s_detail_node = 0;
    s_detail_pts  = -1;
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}

static void detail_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        detail_close();
    }
}

static void card_click_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    detail_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void build_detail(void)
{
    s_detail = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_detail);
    lv_obj_set_size(s_detail, 240, 320);
    lv_obj_set_pos(s_detail, 0, 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_detail, C_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_detail, UI_FONT, LV_PART_MAIN);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);

    /* 返回按钮 + 标题 */
    lv_obj_t *back = lv_btn_create(s_detail);
    lv_obj_set_size(back, 62, 26);
    lv_obj_set_pos(back, 4, 2);
    lv_obj_set_style_bg_color(back, C_HDR, LV_PART_MAIN);
    lv_obj_set_style_radius(back, 6, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(back, detail_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "返回");
    lv_obj_set_style_text_color(bl, C_TEXT, LV_PART_MAIN);
    lv_obj_center(bl);

    s_detail_title = lv_label_create(s_detail);
    lv_label_set_text(s_detail_title, "节点");
    lv_obj_set_style_text_color(s_detail_title, C_ACCENT, LV_PART_MAIN);
    lv_obj_set_pos(s_detail_title, 74, 6);

    s_detail_vals = lv_label_create(s_detail);
    lv_label_set_text(s_detail_vals, "");
    lv_obj_set_style_text_color(s_detail_vals, C_TEXT, LV_PART_MAIN);
    lv_obj_set_pos(s_detail_vals, 8, 32);

    s_detail_vals2 = lv_label_create(s_detail);
    lv_label_set_text(s_detail_vals2, "");
    lv_obj_set_style_text_color(s_detail_vals2, C_TEXT, LV_PART_MAIN);
    lv_obj_set_pos(s_detail_vals2, 8, 52);

    /* Δ% 曲线（标题随传感器型号变化，见 refresh_detail） */
    s_detail_ch1_lbl = detail_label(s_detail, 8, 76, "Δ% 曲线");
    s_chart_dpct = chart_create(s_detail, 8, 94, 224, 88, -500, 500);
    s_ser_dpct = lv_chart_add_series(s_chart_dpct, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_ext_y_array(s_chart_dpct, s_ser_dpct, s_arr_dpct);
    lv_chart_cursor_t *cu;
    lv_point_t pt;
    cu = lv_chart_add_cursor(s_chart_dpct, C_ALARM, LV_DIR_HOR);
    pt.x = LV_CHART_POINT_NONE; pt.y = 250;
    lv_chart_set_cursor_pos(s_chart_dpct, cu, &pt);
    cu = lv_chart_add_cursor(s_chart_dpct, C_ALARM, LV_DIR_HOR);
    pt.x = LV_CHART_POINT_NONE; pt.y = -250;
    lv_chart_set_cursor_pos(s_chart_dpct, cu, &pt);

    /* Rs 曲线（自动量程），标题同样随传感器型号变化 */
    s_detail_ch2_lbl = detail_label(s_detail, 8, 186, "Rs 曲线（kΩ）");
    s_chart_rs = chart_create(s_detail, 8, 204, 224, 88, 0, 3000);
    s_ser_rs = lv_chart_add_series(s_chart_rs, C_OK, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_ext_y_array(s_chart_rs, s_ser_rs, s_arr_rs);

    s_detail_note = detail_label(s_detail, 8, 296, "");
}

static void detail_open(int node_index)
{
    s_detail_node = node_index;
    s_detail_pts  = -1;

    char t[32];
    snprintf(t, sizeof(t), "节点%d", node_index);
    gw_node_view_t nodes[GW_MAX_NODES];
    const int n = gw_nodes_snapshot(nodes, GW_MAX_NODES);
    for (int i = 0; i < n; i++) {
        if (nodes[i].index != node_index) {
            continue;
        }
        /* 标题带上"编号 + 传感器型号"，一眼看出这个节点测的是什么 */
        if (nodes[i].tag[0] && nodes[i].model[0]) {
            snprintf(t, sizeof(t), "节点%d %.8s · %.8s", node_index, nodes[i].tag, nodes[i].model);
        } else if (nodes[i].tag[0] || nodes[i].model[0]) {
            snprintf(t, sizeof(t), "节点%d %.8s%.8s", node_index, nodes[i].tag, nodes[i].model);
        }
        break;
    }
    lv_label_set_text(s_detail_title, t);

    ESP_LOGI(TAG, "打开 节点%d 曲线（已有 %d 点历史）", node_index, gw_hist_count((uint8_t)node_index));
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_detail);
    ui_check_overflow(s_detail, "详情页");     /* 自检：文字有没有超出边界 */
}

/* 刷新曲线（只在浮层打开、且点数变化时重画） */
static void refresh_detail(void)
{
    if (s_detail_node == 0) {
        return;
    }

    gw_node_view_t nodes[GW_MAX_NODES];
    const int n = gw_nodes_snapshot(nodes, GW_MAX_NODES);
    const gw_node_view_t *v = NULL;
    for (int i = 0; i < n; i++) {
        if (nodes[i].index == s_detail_node) {
            v = &nodes[i];
            break;
        }
    }
    /* 曲线标题与底部说明按当前节点的传感器型号刷新；型号没变就不动，避免每 500ms 重绘 */
    static char last_model[16];
    const char *now_model = (v != NULL) ? v->model : "";
    if (strncmp(last_model, now_model, sizeof(last_model)) != 0) {
        snprintf(last_model, sizeof(last_model), "%s", now_model);
        const sensor_info_t *si = sensor_lookup(now_model);
        lv_label_set_text_fmt(s_detail_ch1_lbl, "Δ%% 曲线（%s ±25%%）", si->gas);
        lv_label_set_text(s_detail_ch2_lbl, "Rs 曲线（越低浓度越高）");
        lv_label_set_text(s_detail_note, si->desc);
        ESP_LOGI(TAG, "详情页文案按型号刷新: 型号=%s 含义=%s",
                 now_model[0] ? now_model : "(未知)", si->short_name);
    }

    if (v != NULL && v->have_data) {
        char dpct_s[16], rs_s[16];
        fmt_dpct(v->dpct, dpct_s, sizeof(dpct_s));
        fmt_rs(v->rs, rs_s, sizeof(rs_s));
        /* 两行显示：实测单行最宽 197px（屏幕 240，左右各留 8） */
        lv_label_set_text_fmt(s_detail_vals,  "Δ%% %s    ADC %u", dpct_s, (unsigned)v->adc);
        lv_label_set_text_fmt(s_detail_vals2, "Rs %sΩ    RSSI %ddBm", rs_s, (int)v->rssi);
    } else {
        lv_label_set_text(s_detail_vals, "该节点暂无数据");
        lv_label_set_text(s_detail_vals2, "");
    }

    /* Δ% 曲线：用固定 ±50% 量程，前面不足的点用第一个值补平 */
    int got = gw_hist_get((uint8_t)s_detail_node, GW_H_SER_DPCT, s_arr_dpct, GW_HIST_POINTS);
    for (int i = got; i < GW_HIST_POINTS; i++) {
        s_arr_dpct[i] = (got > 0) ? s_arr_dpct[got - 1] : 0;
    }

    /* Rs 曲线：先取数，再按实际范围设量程（单位 10Ω） */
    int got_rs = gw_hist_get((uint8_t)s_detail_node, GW_H_SER_RS, s_arr_rs, GW_HIST_POINTS);
    int16_t mn = 32767, mx = -32768;
    for (int i = 0; i < got_rs; i++) {
        if (s_arr_rs[i] < mn) mn = s_arr_rs[i];
        if (s_arr_rs[i] > mx) mx = s_arr_rs[i];
    }
    for (int i = got_rs; i < GW_HIST_POINTS; i++) {
        s_arr_rs[i] = (got_rs > 0) ? s_arr_rs[got_rs - 1] : 0;
    }
    if (got_rs > 0) {
        const int span = (mx > mn) ? (mx - mn) : 100;
        const int pad  = span / 4 + 20;
        lv_chart_set_range(s_chart_rs, LV_CHART_AXIS_PRIMARY_Y, mn - pad, mx + pad);
    }

    /* 点数没变就不重画 */
    if (got != s_detail_pts) {
        s_detail_pts = got;
        lv_chart_refresh(s_chart_dpct);
        lv_chart_refresh(s_chart_rs);
        if (got > 0) {
            ESP_LOGI(TAG, "曲线刷新: 节点%d 共 %d 点（Δ%% 最新 %d、Rs 最新 %d ×10Ω）",
                     s_detail_node, got,
                     (int)s_arr_dpct[got - 1], (int)s_arr_rs[got_rs > 0 ? got_rs - 1 : 0]);
        }
    }
}

static void ctl_node_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    s_ctl_sel = (int)(intptr_t)lv_event_get_user_data(e);
    lv_label_set_text_fmt(s_ctl_status, "已选择 节点%d，请选择操作", s_ctl_sel);
}

static void ctl_action_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const char *cmd  = (const char *)lv_event_get_user_data(e);
    const char *name = (cmd[0] == 'M') ? "消音" : (cmd[0] == 'T') ? "自检" : "复位";

    if (cmd[0] == 'T') {
        /* [阶段2] 自检时网关自己也"嘀"一声：不依赖节点是否在线，专门用来验证板载喇叭 */
        gw_buzzer_test();
    }

    if (gw_send_cmd_to_index(s_ctl_sel, cmd)) {
        lv_label_set_text_fmt(s_ctl_status, "已下发 %s → 节点%d", name, s_ctl_sel);
        ESP_LOGI(TAG, "界面下发 %s(%s) 到 节点%d", name, cmd, s_ctl_sel);
    } else {
        lv_label_set_text_fmt(s_ctl_status, "节点%d 未就绪，下发失败", s_ctl_sel);
    }
}

/* [阶段3] 清洁空气标定：把选中节点此刻的 Rs 记为 R0（写进 NVS，掉电不丢） */
static void ctl_cal_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    char msg[48];
    gw_calibrate_node(s_ctl_sel, msg, sizeof(msg));
    lv_label_set_text_fmt(s_ctl_status, "节点%d %s", s_ctl_sel, msg);
    ESP_LOGI(TAG, "标定 节点%d: %s", s_ctl_sel, msg);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h,
                          lv_coord_t x, lv_coord_t y, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, C_BTN, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, C_TEXT, LV_PART_MAIN);
    lv_obj_center(l);
    return b;
}

static void build_control_page(lv_obj_t *tab)
{
    page_style(tab);

    lv_obj_t *t1 = lv_label_create(tab);
    lv_label_set_text(t1, "选择节点");
    lv_obj_set_style_text_color(t1, C_ACCENT, LV_PART_MAIN);
    lv_obj_set_pos(t1, 10, 4);

    /* 2×2 节点按钮（40 高，留出下面 2×2 操作按钮的位置） */
    for (int i = 0; i < GW_MAX_NODES; i++) {
        const lv_coord_t x = 10 + (i % 2) * 114;
        const lv_coord_t y = 26 + (i / 2) * 46;
        s_ctl_btn[i] = make_btn(tab, "节点", 110, 40, x, y,
                                ctl_node_cb, (void *)(intptr_t)(i + 1));
    }

    lv_obj_t *t2 = lv_label_create(tab);
    lv_label_set_text(t2, "操作");
    lv_obj_set_style_text_color(t2, C_ACCENT, LV_PART_MAIN);
    lv_obj_set_pos(t2, 10, 122);

    make_btn(tab, "消音60s", 110, 38, 10,  142, ctl_action_cb, (void *)"M");
    make_btn(tab, "自检",    110, 38, 120, 142, ctl_action_cb, (void *)"T");
    make_btn(tab, "复位",    110, 38, 10,  184, ctl_action_cb, (void *)"R");
    make_btn(tab, "标定",    110, 38, 120, 184, ctl_cal_cb,    NULL);

    s_ctl_status = lv_label_create(tab);
    lv_label_set_text_fmt(s_ctl_status, "已选择 节点%d，请选择操作", s_ctl_sel);
    lv_obj_set_style_text_color(s_ctl_status, C_DIM, LV_PART_MAIN);
    lv_obj_set_pos(s_ctl_status, 10, 230);

    lv_obj_t *hint = lv_label_create(tab);
    lv_label_set_text(hint, "标定：清洁空气里记录 R0");
    lv_obj_set_style_text_color(hint, C_DIM, LV_PART_MAIN);
    lv_obj_set_pos(hint, 10, 258);
}

/* ================================================================== */
/* [阶段4] 设置页                                                     */
/* ================================================================== */

static void bright_slider_cb(lv_event_t *e)
{
    s_bright = (int)lv_slider_get_value(lv_event_get_target(e));
    if (s_set_bright_lbl != NULL) {
        lv_label_set_text_fmt(s_set_bright_lbl, "%d%%", s_bright);
    }
    /* 立刻生效给用户反馈；夜间自动的修正会在下一次 500ms 刷新时补上 */
    board_backlight_set(s_bright);
}

static void night_switch_cb(lv_event_t *e)
{
    s_night_auto = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ESP_LOGI(TAG, "夜间自动变暗: %s", s_night_auto ? "开" : "关");
}

static void build_settings_page(lv_obj_t *tab)
{
    page_style(tab);

    lv_obj_t *t1 = lv_label_create(tab);
    lv_label_set_text(t1, "屏幕");
    lv_obj_set_style_text_color(t1, C_ACCENT, LV_PART_MAIN);
    lv_obj_set_pos(t1, 10, 4);

    lv_obj_t *bl = lv_label_create(tab);
    lv_label_set_text(bl, "背光");
    lv_obj_set_pos(bl, 10, 32);

    s_set_slider = lv_slider_create(tab);
    lv_obj_set_size(s_set_slider, 106, 12);
    lv_obj_set_pos(s_set_slider, 54, 36);
    lv_slider_set_range(s_set_slider, 10, 100);
    lv_slider_set_value(s_set_slider, s_bright, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_set_slider, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_set_slider, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_set_slider, C_ACCENT, LV_PART_KNOB);
    lv_obj_add_event_cb(s_set_slider, bright_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_set_bright_lbl = lv_label_create(tab);
    lv_label_set_text_fmt(s_set_bright_lbl, "%d%%", s_bright);
    lv_obj_set_style_text_color(s_set_bright_lbl, C_DIM, LV_PART_MAIN);
    lv_obj_set_pos(s_set_bright_lbl, 170, 32);

    lv_obj_t *nl = lv_label_create(tab);
    lv_label_set_text(nl, "夜间自动变暗");
    lv_obj_set_pos(nl, 10, 60);

    s_set_auto_sw = lv_switch_create(tab);
    lv_obj_set_size(s_set_auto_sw, 44, 22);
    lv_obj_set_pos(s_set_auto_sw, 150, 58);
    if (s_night_auto) {
        lv_obj_add_state(s_set_auto_sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(s_set_auto_sw, night_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *nl2 = lv_label_create(tab);
    lv_label_set_text(nl2, "22:00~07:00 降到 25%");
    lv_obj_set_style_text_color(nl2, C_DIM, LV_PART_MAIN);
    lv_obj_set_pos(nl2, 10, 86);

    lv_obj_t *line = lv_obj_create(tab);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 220, 1);
    lv_obj_set_pos(line, 10, 112);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(line, C_BORDER, LV_PART_MAIN);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t2 = lv_label_create(tab);
    lv_label_set_text(t2, "网关信息");
    lv_obj_set_style_text_color(t2, C_ACCENT, LV_PART_MAIN);
    lv_obj_set_pos(t2, 10, 120);

    for (int i = 0; i < 6; i++) {
        s_set_info[i] = lv_label_create(tab);
        lv_label_set_text(s_set_info[i], "");
        lv_obj_set_style_text_color(s_set_info[i], C_TEXT, LV_PART_MAIN);
        lv_obj_set_pos(s_set_info[i], 10, 146 + i * 22);
    }
}

static void refresh_settings(void)
{
    /* 这些信息 2 秒刷一次足够，没必要每 500ms 重新格式化 6 个字符串 */
    static int tick = 0;
    if (++tick < 4) {
        return;
    }
    tick = 0;

    char ip[20];
    WifiMqtt_GetIp(ip, sizeof(ip));

    lv_label_set_text_fmt(s_set_info[0], "网关 %s · 固件 %s", APP_GATEWAY_ID, APP_GW_FW_VER);
    lv_label_set_text_fmt(s_set_info[1], "WiFi %s", APP_WIFI_SSID);
    lv_label_set_text_fmt(s_set_info[2], "IP %s  %ddBm", ip, WifiMqtt_GetWifiRssi());
    lv_label_set_text_fmt(s_set_info[3], "MQTT %s · WiFi %s",
                          WifiMqtt_IsConnected() ? "已连接" : "未连接",
                          WifiMqtt_IsWifiReady() ? "已连" : "未连");
    lv_label_set_text_fmt(s_set_info[4], "在线节点 %d / %d",
                          gw_nodes_ready_count(), gw_nodes_max());
    const int up = (int)(esp_timer_get_time() / 1000000);
    lv_label_set_text_fmt(s_set_info[5], "运行 %d时%d分 · 空闲 %uKB",
                          up / 3600, (up / 60) % 60,
                          (unsigned)(esp_get_free_heap_size() / 1024));
}

/* ================================================================== */
/* [阶段4] 开机自检页（只开机用一次，关掉时直接删除对象回收内存）      */
/* ================================================================== */

static void splash_close(void)
{
    if (s_splash == NULL) {
        return;
    }
    lv_obj_del(s_splash);
    s_splash = NULL;
    for (int i = 0; i < 3; i++) {
        s_splash_note[i] = NULL;
    }
    ESP_LOGI(TAG, "开机自检页关闭，对象已回收");
}

static void splash_click_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    splash_close();
}

static void build_splash(void)
{
    s_splash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, 240, 320);
    lv_obj_set_pos(s_splash, 0, 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_splash, C_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_splash, UI_FONT, LV_PART_MAIN);
    lv_obj_clear_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_splash, LV_OBJ_FLAG_CLICKABLE);      /* 点一下也能跳过 */
    lv_obj_add_event_cb(s_splash, splash_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *band = lv_obj_create(s_splash);
    lv_obj_remove_style_all(band);
    lv_obj_set_size(band, 240, 88);
    lv_obj_set_pos(band, 0, 0);
    lv_obj_set_style_bg_opa(band, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(band, C_ACCENT, LV_PART_MAIN);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t1 = lv_label_create(band);
    lv_label_set_text(t1, "家庭安全网关");
    lv_obj_set_style_text_color(t1, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *t2 = lv_label_create(band);
    lv_label_set_text(t2, "烟雾 · 可燃气 · 空气质量");
    lv_obj_set_style_text_color(t2, lv_color_hex(0xE4F1FB), LV_PART_MAIN);
    lv_obj_align(t2, LV_ALIGN_TOP_MID, 0, 46);

    static const char *items[3] = {"屏幕 / 触摸", "蓝牙 / 网络", "在线节点"};
    for (int i = 0; i < 3; i++) {
        s_splash_note[i] = lv_label_create(s_splash);
        lv_label_set_text_fmt(s_splash_note[i], "%s   --", items[i]);
        lv_obj_set_style_text_color(s_splash_note[i], C_TEXT, LV_PART_MAIN);
        lv_obj_set_pos(s_splash_note[i], 30, 120 + i * 30);
    }

    lv_obj_t *f = lv_label_create(s_splash);
    lv_label_set_text_fmt(f, "固件 CYD %s", APP_GW_FW_VER);
    lv_obj_set_style_text_color(f, C_DIM, LV_PART_MAIN);
    lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, -34);

    lv_obj_t *f2 = lv_label_create(s_splash);
    lv_label_set_text(f2, "正在启动…（点屏幕跳过）");
    lv_obj_set_style_text_color(f2, C_DIM, LV_PART_MAIN);
    lv_obj_align(f2, LV_ALIGN_BOTTOM_MID, 0, -12);

    s_splash_ticks = 0;
}

static void refresh_splash(void)
{
    if (s_splash == NULL) {
        return;
    }
    s_splash_ticks++;

    if (s_splash_note[0] != NULL) {
        lv_label_set_text(s_splash_note[0], "屏幕 / 触摸   就绪");
    }
    if (s_splash_note[1] != NULL) {
        lv_label_set_text_fmt(s_splash_note[1], "蓝牙 / 网络   %s",
                              WifiMqtt_IsConnected() ? "就绪"
                              : (WifiMqtt_IsWifiReady() ? "云端连接中" : "等网络"));
    }
    if (s_splash_note[2] != NULL) {
        lv_label_set_text_fmt(s_splash_note[2], "在线节点      %d / %d",
                              gw_nodes_ready_count(), gw_nodes_max());
    }

    if (s_splash_ticks >= 7) {       /* 约 3.5 秒后自动进入主界面 */
        splash_close();
    }
}

/* ================================================================== */
/* [阶段4] 报警自动弹大屏                                              */
/* ================================================================== */

static void alarm_pop_hide(bool suppressed)
{
    if (s_alarm_pop == NULL) {
        return;
    }
    s_alarm_pop_suppressed = suppressed;
    lv_obj_add_flag(s_alarm_pop, LV_OBJ_FLAG_HIDDEN);
}

static void alarm_pop_mute_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    const bool ok = gw_send_cmd_to_index(s_alarm_pop_node, "M");
    if (ok) {
        s_local_mute_until_us = esp_timer_get_time() + 60LL * 1000000;
    }
    ESP_LOGI(TAG, "报警弹窗[消音] 节点%d %s", s_alarm_pop_node, ok ? "已下发" : "失败");
    alarm_pop_hide(true);
}

static void alarm_pop_close_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ESP_LOGI(TAG, "报警弹窗[关闭] 节点%d", s_alarm_pop_node);
    alarm_pop_hide(true);
}

static void build_alarm_popup(void)
{
    s_alarm_pop = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_alarm_pop);
    lv_obj_set_size(s_alarm_pop, 240, 320);
    lv_obj_set_pos(s_alarm_pop, 0, 0);
    lv_obj_set_style_bg_opa(s_alarm_pop, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_alarm_pop, lv_color_hex(0x2C3E50), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_alarm_pop, UI_FONT, LV_PART_MAIN);
    lv_obj_clear_flag(s_alarm_pop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_alarm_pop, LV_OBJ_FLAG_HIDDEN);

    s_alarm_box = lv_obj_create(s_alarm_pop);
    lv_obj_remove_style_all(s_alarm_box);
    lv_obj_set_size(s_alarm_box, 216, 186);
    lv_obj_align(s_alarm_box, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_radius(s_alarm_box, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_alarm_box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_alarm_box, C_CARD, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_alarm_box, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_alarm_box, C_ALARM, LV_PART_MAIN);
    lv_obj_clear_flag(s_alarm_box, LV_OBJ_FLAG_SCROLLABLE);

    s_alarm_band = lv_obj_create(s_alarm_box);
    lv_obj_remove_style_all(s_alarm_band);
    lv_obj_set_size(s_alarm_band, 216, 40);
    lv_obj_set_pos(s_alarm_band, 0, 0);
    lv_obj_set_style_radius(s_alarm_band, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_alarm_band, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_alarm_band, C_ALARM, LV_PART_MAIN);
    lv_obj_clear_flag(s_alarm_band, LV_OBJ_FLAG_CLICKABLE);

    s_alarm_band_lbl = lv_label_create(s_alarm_band);
    lv_label_set_text(s_alarm_band_lbl, "报 警");
    lv_obj_set_style_text_color(s_alarm_band_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(s_alarm_band_lbl);

    s_alarm_room = lv_label_create(s_alarm_box);
    lv_label_set_text(s_alarm_room, "");
    lv_obj_set_style_text_color(s_alarm_room, C_TEXT, LV_PART_MAIN);
    lv_obj_set_pos(s_alarm_room, 12, 50);

    s_alarm_val = lv_label_create(s_alarm_box);
    lv_label_set_text(s_alarm_val, "");
    lv_obj_set_style_text_color(s_alarm_val, C_ALARM, LV_PART_MAIN);
    lv_obj_set_pos(s_alarm_val, 12, 76);

    lv_obj_t *hint = lv_label_create(s_alarm_box);
    lv_label_set_text(hint, "请立即查看并通风");
    lv_obj_set_style_text_color(hint, C_DIM, LV_PART_MAIN);
    lv_obj_set_pos(hint, 12, 102);

    lv_obj_t *mb = lv_btn_create(s_alarm_box);
    lv_obj_set_size(mb, 90, 34);
    lv_obj_set_pos(mb, 12, 134);
    lv_obj_set_style_radius(mb, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mb, C_ALARM, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(mb, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(mb, alarm_pop_mute_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ml = lv_label_create(mb);
    lv_label_set_text(ml, "消音");
    lv_obj_set_style_text_color(ml, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(ml);

    lv_obj_t *cb = lv_btn_create(s_alarm_box);
    lv_obj_set_size(cb, 90, 34);
    lv_obj_set_pos(cb, 114, 134);
    lv_obj_set_style_radius(cb, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cb, C_BTN, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(cb, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(cb, alarm_pop_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = lv_label_create(cb);
    lv_label_set_text(cl, "关闭");
    lv_obj_set_style_text_color(cl, C_TEXT, LV_PART_MAIN);
    lv_obj_center(cl);
}

/* ------------------------------------------------------------------ */
/* 定时刷新                                                            */
/* ------------------------------------------------------------------ */

static void refresh_monitor(void)
{
    gw_node_view_t nodes[GW_MAX_NODES];
    const int n = gw_nodes_snapshot(nodes, GW_MAX_NODES);
    const int64_t now = esp_timer_get_time();

    /* [CYD port] 临时诊断：开机打印一次 1 号卡片各控件的真实坐标，改布局时很有用 */
    static int dbg_dump = 0;
    if (dbg_dump < 2 && ++dbg_dump == 2) {
        const lv_obj_t *objs[6] = {s_card[0].name, s_card[0].badge, s_card[0].big,
                                   s_card[0].bar_bg, s_card[0].trend, s_card[0].info};
        const char *names[6] = {"name", "badge", "big", "bar", "trend", "info"};
        for (int k = 0; k < 6; k++) {
            lv_area_t a;
            lv_obj_get_coords(objs[k], &a);
            ESP_LOGI(TAG, "布局: %-5s x=%d..%d y=%d..%d (w=%d h=%d)",
                     names[k], (int)a.x1, (int)a.x2, (int)a.y1, (int)a.y2,
                     (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1));
        }
        lv_area_t ca;
        lv_obj_get_coords(s_card[0].card, &ca);
        ESP_LOGI(TAG, "布局: 卡片 x=%d..%d y=%d..%d", (int)ca.x1, (int)ca.x2, (int)ca.y1, (int)ca.y2);

        /* 文字自检：三个页面 + 曲线详情各查一遍 */
        ui_check_overflow(s_tab_monitor, "监控页");
        ui_check_overflow(s_tab_records, "记录页");
        ui_check_overflow(s_tab_control, "控制页");
        ui_check_overflow(s_detail, "详情页");
    }

    int  alarm_idx = -1;
    int  alarm_peak_x10 = 0;
    int  fault_cnt = 0;
    char alarm_txt[64] = "";

    for (int i = 0; i < GW_MAX_NODES; i++) {
        card_t *c = &s_card[i];

        /* 默认收起"消音"按钮，下面只在报警时放出来 */
        lv_obj_add_flag(c->mute, LV_OBJ_FLAG_HIDDEN);

        if (i >= n) {
            lv_obj_set_style_bg_color(c->card, C_CARD, LV_PART_MAIN);
            lv_obj_set_style_border_color(c->card, C_BORDER, LV_PART_MAIN);
            lv_label_set_text_fmt(c->name, "节点%d", i + 1);
            lv_label_set_text(c->badge, "空闲");
            lv_obj_set_style_text_color(c->badge, C_DIM, LV_PART_MAIN);
            lv_label_set_text(c->big, "--");
            lv_obj_set_style_text_color(c->big, C_DIM, LV_PART_MAIN);
            lv_obj_set_width(c->bar_fill, 0);
            lv_label_set_text(c->trend, "");
            lv_label_set_text(c->info, "");
            continue;
        }

        const gw_node_view_t *v = &nodes[i];
        const sensor_info_t *si = sensor_lookup(v->model);
        const bool stale = v->have_data && (now - v->last_rx_us > 8 * 1000000LL);

        /* 第 1 行：节点N 房间名 · 传感器（说人话，不是 MQ-135 这种型号） */
        if (v->tag[0]) {
            lv_label_set_text_fmt(c->name, "节点%d %.8s · %.8s", v->index, v->tag, si->short_name);
        } else {
            lv_label_set_text_fmt(c->name, "节点%d · %.8s", v->index, si->short_name);
        }

        /* ---- 先把这一格要显示的东西全算出来，最后一次性写控件 ---- */
        const char *word  = "等待";
        lv_color_t  wcol  = C_DIM;
        const char *arrow = "";
        lv_color_t  acol  = C_DIM;
        char        info[64] = "";      /* 留足余量：%d 的最坏情况 GCC 按 11 位算，太小会 -Werror */
        int         shown = -1;              /* 指数；-1 = 显示 "--" */
        bool        blink = false;
        bool        mute  = false;

        if (!v->used) {
            snprintf(info, sizeof(info), "等待节点上线");
        } else if (!v->ready) {
            word = "连接中";
            wcol = C_WARN;
            snprintf(info, sizeof(info), "%s", v->mac);
        } else if (!v->have_data) {
            snprintf(info, sizeof(info), "%s · 等数据", v->mac);
        } else if (v->probe_fault) {
            /* 探头没接：先保安全，再谈数据（不能显示"正常"） */
            word = "故障";
            wcol = C_ALARM;
            fault_cnt++;
            snprintf(info, sizeof(info), "请检查探头接线");
        } else {
            bool need_cal = false;
            const int index = sensor_index(v, si, &need_cal);
            int peak = 0;
            const int trend = hist_trend_peak(v->index, &peak);
            char dstr[16];
            char age[16];
            fmt_dpct(v->dpct, dstr, sizeof(dstr));
            fmt_age(now - v->last_rx_us, age, sizeof(age));

            if (trend == 1)       { arrow = "↑"; acol = C_ALARM; }
            else if (trend == -1) { arrow = "↓"; acol = C_OK;    }
            else if (trend == 0)  { arrow = "→"; acol = C_DIM;   }

            if (v->alarm) {
                word  = level_word(si, index, 1, &wcol);
                shown = index;
                blink = true;
                mute  = true;
                if (peak > 1000) {      /* >100% 只可能是探头异常时的噪声，别让它把行宽顶爆 */
                    snprintf(info, sizeof(info), "峰值 >100%% · %s", age);
                } else {
                    snprintf(info, sizeof(info), "峰值 %d.%d%% · %s", peak / 10, peak % 10, age);
                }
                if (alarm_idx < 0) {
                    alarm_idx     = i;
                    alarm_peak_x10 = peak;
                    snprintf(alarm_txt, sizeof(alarm_txt), "%.8s %.8s 报警",
                             v->tag[0] ? v->tag : "节点", si->short_name);
                }
            } else if (stale) {
                word  = "离线";
                wcol  = C_DIM;
                shown = index;
                snprintf(info, sizeof(info), "最后 %s", age);
            } else if (v->warmup) {
                word  = "预热";
                wcol  = C_WARN;
                shown = index;
                if (v->warmup_remain > 0) {
                    snprintf(info, sizeof(info), "预热中 剩 %u 秒", (unsigned)v->warmup_remain);
                } else {
                    snprintf(info, sizeof(info), "Δ %s · 预热中", dstr);
                }
            } else if (need_cal) {
                /* 环境类还没做清洁空气标定：不给假的"优"，直接提示去标定 */
                word  = "未标定";
                wcol  = C_DIM;
                shown = -1;
                snprintf(info, sizeof(info), "请在清洁空气里点 标定");
            } else {
                word  = level_word(si, index, 0, &wcol);
                shown = index;
                if (v->muted) {
                    snprintf(info, sizeof(info), "已消音 · %s", age);
                } else if (!si->is_safety && v->cal_ok && v->r0_ohm > 0) {
                    /* 环境类给绝对比值：Rs/R0（0.85 = 比清洁空气低 15%） */
                    const int r100 = (int)((uint64_t)v->rs * 100 / v->r0_ohm);
                    snprintf(info, sizeof(info), "Rs/R0 %d.%02d · %s", r100 / 100, r100 % 100, age);
                } else {
                    snprintf(info, sizeof(info), "Δ %s · %s", dstr, age);
                }
            }
        }

        /* ---- 一次性写控件 ---- */
        lv_label_set_text(c->badge, word);
        lv_obj_set_style_text_color(c->badge, wcol, LV_PART_MAIN);

        if (shown < 0) {
            lv_label_set_text(c->big, "--");
            lv_obj_set_style_text_color(c->big, C_DIM, LV_PART_MAIN);
            lv_obj_set_width(c->bar_fill, 0);
        } else {
            int w = BAR_W * shown / 100;
            if (w < 2 && shown > 0) {
                w = 2;                       /* 非零至少露一点，别看起来像 0 */
            }
            lv_label_set_text_fmt(c->big, "%d", shown);
            lv_obj_set_style_text_color(c->big, wcol, LV_PART_MAIN);
            lv_obj_set_width(c->bar_fill, w);
            lv_obj_set_style_bg_color(c->bar_fill, wcol, LV_PART_MAIN);
        }

        lv_label_set_text(c->trend, arrow);
        lv_obj_set_style_text_color(c->trend, acol, LV_PART_MAIN);
        lv_label_set_text(c->info, info);

        lv_obj_set_style_bg_color(c->card,
                                  blink ? ((s_blink & 1) ? C_ALARM_BG2 : C_ALARM_BG) : C_CARD,
                                  LV_PART_MAIN);
        lv_obj_set_style_border_color(c->card, blink ? C_ALARM : C_BORDER, LV_PART_MAIN);

        if (mute) {
            lv_obj_clear_flag(c->mute, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* ---- [阶段2] 顶栏时钟：SNTP 对上时间才显示，否则 --:-- ---- */
    {
        const time_t t = time(NULL);
        if (t > 1700000000) {
            struct tm tm_now;
            localtime_r(&t, &tm_now);
            lv_label_set_text_fmt(s_hdr_time, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        } else {
            lv_label_set_text(s_hdr_time, "--:--");
        }
    }

    /* 顶部状态条：报警 > 探头异常 > 一切正常 */
    if (alarm_idx >= 0) {
        lv_obj_set_style_bg_color(lv_obj_get_parent(s_hdr_lbl),
                                  (s_blink & 1) ? C_ALARM_BG2 : C_ALARM, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_hdr_lbl, (s_blink & 1) ? C_ALARM : lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_color(s_hdr_time, (s_blink & 1) ? C_ALARM : lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(s_hdr_lbl, alarm_txt);
    } else if (fault_cnt > 0) {
        lv_obj_set_style_bg_color(lv_obj_get_parent(s_hdr_lbl), C_ALARM_BG, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_hdr_lbl, C_ALARM, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_hdr_time, C_ALARM, LV_PART_MAIN);
        lv_label_set_text_fmt(s_hdr_lbl, "%d 个探头异常", fault_cnt);
    } else {
        const char *net = "联网";
        if (!WifiMqtt_IsWifiReady()) {
            net = "断网";
        } else if (!WifiMqtt_IsConnected()) {
            net = "云端断";
        }
        lv_obj_set_style_bg_color(lv_obj_get_parent(s_hdr_lbl), C_HDR, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_hdr_lbl, C_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_hdr_time, C_TEXT, LV_PART_MAIN);
        lv_label_set_text_fmt(s_hdr_lbl, "一切正常 · %s · %d/%d",
                              net, gw_nodes_ready_count(), gw_nodes_max());
    }

    /* ---- [阶段2] 报警声：任一路"真的报警且没被消音"就响 ----
     * 探头故障时的"报警"是噪声，不算；本机刚点过消音就整体静音 60 秒（与节点 MUTE 时长一致）。 */
    {
        bool beep = false;
        for (int i = 0; i < n; i++) {
            const gw_node_view_t *v = &nodes[i];
            if (v->have_data && !v->probe_fault && v->alarm && !v->muted) {
                beep = true;
                break;
            }
        }
        if (esp_timer_get_time() < s_local_mute_until_us) {
            beep = false;
        }
        gw_buzzer_set(beep);
    }

    /* ---- [阶段4] 报警自动弹大屏：报警出现就弹出来，解除后自动收起 ---- */
    if (s_alarm_pop != NULL) {
        if (alarm_idx < 0) {
            s_alarm_pop_suppressed = false;          /* 报警解除，恢复"自动弹出" */
            lv_obj_add_flag(s_alarm_pop, LV_OBJ_FLAG_HIDDEN);
        } else if (!s_alarm_pop_suppressed) {
            const gw_node_view_t *v = &nodes[alarm_idx];
            const sensor_info_t  *si = sensor_lookup(v->model);
            s_alarm_pop_node = v->index;

            if (lv_obj_has_flag(s_alarm_pop, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_clear_flag(s_alarm_pop, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_alarm_pop);
                ESP_LOGW(TAG, "报警弹窗: 节点%d %.8s", v->index, v->tag[0] ? v->tag : "");
            }

            char dstr[16];
            fmt_dpct(v->dpct, dstr, sizeof(dstr));
            lv_label_set_text_fmt(s_alarm_room, "%.8s · %.8s",
                                  v->tag[0] ? v->tag : "节点", si->short_name);
            if (alarm_peak_x10 > 1000) {
                lv_label_set_text_fmt(s_alarm_val, "Δ%% %s   峰值 >100%%", dstr);
            } else {
                lv_label_set_text_fmt(s_alarm_val, "Δ%% %s   峰值 %d.%d%%",
                                      dstr, alarm_peak_x10 / 10, alarm_peak_x10 % 10);
            }
            /* 红闪：横幅和边框在两种红之间交替 */
            const bool on = (s_blink & 1) != 0;
            lv_obj_set_style_bg_color(s_alarm_band, on ? C_ALARM_BG2 : C_ALARM, LV_PART_MAIN);
            if (s_alarm_band_lbl != NULL) {
                lv_obj_set_style_text_color(s_alarm_band_lbl, on ? C_ALARM : lv_color_white(), LV_PART_MAIN);
            }
            lv_obj_set_style_border_color(s_alarm_box, on ? C_ALARM_BG2 : C_ALARM, LV_PART_MAIN);
        }
    }
}

static void refresh_records(void)
{
    lv_label_set_text_fmt(s_rec_stat, "今日报警 %d 次 · 累计 %d 次",
                          gw_log_alarm_today(), gw_log_alarm_total());

    const int total = gw_log_total();
    if (total == s_rec_rendered) {
        return;
    }
    s_rec_rendered = total;

    gw_event_t evs[REC_LINES];
    const int n = gw_log_snapshot(evs, REC_LINES);

    for (int i = 0; i < REC_LINES; i++) {
        if (i >= n) {
            lv_label_set_text(s_rec_lines[i], i == 0 ? "暂无事件" : "");
            lv_obj_set_style_text_color(s_rec_lines[i], C_DIM, LV_PART_MAIN);
            continue;
        }
        const gw_event_t *e = &evs[i];
        char t[16] = "--:--:--";
        if (e->ts > 1700000000) {
            struct tm tm_ev;
            localtime_r(&e->ts, &tm_ev);
            snprintf(t, sizeof(t), "%02d:%02d:%02d", tm_ev.tm_hour, tm_ev.tm_min, tm_ev.tm_sec);
        }
        lv_obj_set_style_text_color(s_rec_lines[i], ev_color(e->type), LV_PART_MAIN);

        if (e->type == GW_EV_ALARM || e->type == GW_EV_CLEAR) {
            char dpct_s[16];
            fmt_dpct(e->dpct, dpct_s, sizeof(dpct_s));
            lv_label_set_text_fmt(s_rec_lines[i], "%s 节点%d %s %s",
                                  t, e->node, ev_name(e->type), dpct_s);
        } else if (e->text[0]) {
            lv_label_set_text_fmt(s_rec_lines[i], "%s 节点%d %s %s",
                                  t, e->node, ev_name(e->type), e->text);
        } else {
            lv_label_set_text_fmt(s_rec_lines[i], "%s 节点%d %s",
                                  t, e->node, ev_name(e->type));
        }
    }
}

static void refresh_control(void)
{
    gw_node_view_t nodes[GW_MAX_NODES];
    const int n = gw_nodes_snapshot(nodes, GW_MAX_NODES);

    for (int i = 0; i < GW_MAX_NODES; i++) {
        lv_obj_t *b = s_ctl_btn[i];
        lv_obj_t *l = lv_obj_get_child(b, 0);

        gw_node_view_t *v = (i < n) ? &nodes[i] : NULL;
        const bool ready = v && v->used && v->ready;
        const bool sel   = (i + 1) == s_ctl_sel;

        if (v && v->used) {
            lv_label_set_text_fmt(l, "节点%d %.6s", i + 1, v->tag[0] ? v->tag : "");
        } else {
            lv_label_set_text_fmt(l, "节点%d", i + 1);
        }

        lv_obj_set_style_bg_color(b, sel ? C_ACCENT : C_BTN, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, sel ? lv_color_white() : (ready ? C_TEXT : C_DIM), LV_PART_MAIN);
    }
}

static void ui_refresh(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    s_blink++;

    /* [CYD port] 每 10 秒报一次历史点数，方便确认曲线数据在累积 */
    static int hist_dbg = 0;
    if (++hist_dbg >= 20) {
        hist_dbg = 0;
        ESP_LOGI(TAG, "历史点数: 节点1=%d 节点2=%d 节点3=%d 节点4=%d ｜ 空闲内存 %u KB",
                 gw_hist_count(1), gw_hist_count(2), gw_hist_count(3), gw_hist_count(4),
                 (unsigned)(esp_get_free_heap_size() / 1024));

        /* 开机那一刻节点还没上报，卡片上都是 "--"，自检查不到"有数据"时的宽度。
         * 所以真数据上来后再查几轮（只查前 6 轮，避免万一越界就刷屏）。 */
        static int ovf_rounds = 0;
        if (ovf_rounds < 6) {
            ovf_rounds++;
            ui_check_overflow(s_tab_monitor, "监控页");
            ui_check_overflow(s_tab_records, "记录页");
            ui_check_overflow(s_tab_control, "控制页");
            ui_check_overflow(s_tab_settings, "设置页");
        }
    }

    refresh_monitor();
    refresh_records();
    refresh_control();
    refresh_detail();

    /* 板载 RGB 指示灯：蓝闪=网络断，红闪=报警，绿常亮=正常 */
    bool alarm = false;
    gw_node_view_t nodes[GW_MAX_NODES];
    const int n = gw_nodes_snapshot(nodes, GW_MAX_NODES);
    for (int i = 0; i < n; i++) {
        if (nodes[i].alarm) {
            alarm = true;
            break;
        }
    }
    const bool net_ok = WifiMqtt_IsWifiReady() && WifiMqtt_IsConnected();
    if (!net_ok) {
        board_rgb_set(0, 0, (s_blink & 1) ? 70 : 0);
    } else if (alarm) {
        board_rgb_set((s_blink & 1) ? 80 : 0, 0, 0);
    } else {
        board_rgb_set(0, 25, 0);
    }

    /* [阶段2/4] 背光 = 设置页里设定的亮度；开了"夜间自动"且当前没有报警时，
     * 22:00~07:00 降到该亮度的 25%。没对上时间（SNTP 未同步）就不判断。 */
    static int bl_pct = -1;
    int bl = s_bright;
    if (!alarm && s_night_auto) {
        const time_t t = time(NULL);
        if (t > 1700000000) {
            struct tm tm_now;
            localtime_r(&t, &tm_now);
            if (tm_now.tm_hour >= 22 || tm_now.tm_hour < 7) {
                bl = s_bright * 25 / 100;
            }
        }
    }
    if (bl < 5) {
        bl = 5;
    }
    if (bl != bl_pct) {
        bl_pct = bl;
        board_backlight_set(bl);
        ESP_LOGI(TAG, "背光 %d%%", bl);
    }

    refresh_settings();
    refresh_splash();
}

/* ------------------------------------------------------------------ */

void ui_init(void)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "拿不到 LVGL 锁，界面未创建");
        return;
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_BG, LV_PART_MAIN);
    lv_obj_set_style_text_font(scr, UI_FONT, LV_PART_MAIN);

    lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, TAB_H);
    lv_obj_set_style_bg_color(tv, C_BG, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(tv, 0, LV_PART_MAIN);

    /* 页签按钮：浅色主题 + 选中用蓝底白字 */
    lv_obj_t *btns = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_bg_color(btns, C_HDR, LV_PART_MAIN);
    lv_obj_set_style_text_font(btns, UI_FONT, LV_PART_MAIN);
    const uint32_t cnt = lv_obj_get_child_cnt(btns);
    for (uint32_t k = 0; k < cnt; k++) {
        lv_obj_t *b = lv_obj_get_child(btns, k);
        lv_obj_set_style_bg_color(b, C_HDR, LV_PART_MAIN);
        lv_obj_set_style_text_color(b, C_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_font(b, UI_FONT, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, C_ACCENT, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_text_color(b, lv_color_white(), LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_radius(b, 0, LV_PART_MAIN);
    }

    s_tab_monitor  = lv_tabview_add_tab(tv, "监控");
    s_tab_records  = lv_tabview_add_tab(tv, "记录");
    s_tab_control  = lv_tabview_add_tab(tv, "控制");
    s_tab_settings = lv_tabview_add_tab(tv, "设置");
    build_monitor_page(s_tab_monitor);
    build_records_page(s_tab_records);
    build_control_page(s_tab_control);
    build_settings_page(s_tab_settings);
    build_detail();                 /* 曲线浮层：默认隐藏，点卡片才出现 */
    build_splash();                 /* [阶段4] 开机自检页：3.5 秒后自动删除回收内存 */
    build_alarm_popup();            /* [阶段4] 报警弹窗：默认隐藏 */

    lv_timer_create(ui_refresh, 500, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "界面已创建（监控/记录/控制/设置 + 曲线详情 + 开机自检 + 报警弹窗）");
}
