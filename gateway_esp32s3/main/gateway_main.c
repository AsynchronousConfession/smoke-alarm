/*******************************************************************************
 * ESP32-S3 烟雾报警网关（BLE Central + WiFi/MQTT 上云）
 *
 * 做什么：
 *   1. 上电后持续扫描广播名为 "SMOKE_NODE" 的 CH573F 节点，最多连 APP_MAX_NODES 台
 *   2. 每台连上后自动：交换 MTU → 发现服务 0xFFE0 → 订阅 CHAR4(0xFFE4) 通知
 *   3. 节点每 2 秒（报警时 0.5 秒）推送一帧 16 字节遥测，网关解析后
 *      通过 MQTT 发给 PC 端服务器；报警出现/解除时另发一条事件
 *   4. 网页下发的指令（消音/自检/复位/文本）经 MQTT 到达后，网关
 *      写节点的 CHAR3(0xFFE3) 转发过去
 *   5. 任意节点掉线后自动重新扫描并重连
 *
 * 与节点的 GATT 约定（节点侧见 node_ch573f/.../node_proto.h）:
 *   服务 0xFFE0
 *     0xFFE1  Read/Write 备用
 *     0xFFE3  Write      网关 → 节点（命令/文本，最长 20 字节）
 *     0xFFE4  Notify     节点 → 网关（遥测帧/信息帧，最长 20 字节）
 *******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"

#include "app_config.h"
#include "wifi_mqtt.h"
#include "node_proto.h"

static const char *TAG = "GW";

/* ===================== 协议约定（需与节点固件一致） ===================== */
#define SLAVE_NAME              APP_NODE_ADV_NAME
#define SLAVE_SERVICE_UUID      0xFFE0
#define CHAR_STATUS_UUID        0xFFE1          /* Read  */
#define CHAR_CMD_UUID           0xFFE3          /* Write */
#define CHAR_TELE_NOTIFY_UUID   0xFFE4          /* Notify: 节点 -> 网关的遥测/信息帧 */

#define MASTER_MAX_SLAVES       APP_MAX_NODES
#define SCAN_DURATION_FOREVER   0
#define SLAVE_CMD_MAX_LEN       20

/* 是否允许"只按服务 UUID 0xFFE0"匹配从机。
 * 0xFFE0 是沁恒 SimpleProfile 的默认服务, 其它沁恒实验板也在广播它,
 * 因此默认关闭, 只认广播名 —— 防止连到隔壁的实验板。 */
#define MASTER_MATCH_BY_UUID_FALLBACK   0

/* ===================== 数据结构 ===================== */
typedef enum {
    SLOT_FREE = 0,      /* 空闲, 可用 */
    SLOT_CONNECTING,    /* 已指定从机, 正在建立连接 */
    SLOT_DISCOVERING,   /* 连接已建立, 正在发现服务 */
    SLOT_READY,         /* 服务发现完成, 已订阅通知 */
} slot_state_t;

typedef struct {
    slot_state_t   state;
    uint8_t        index;           /* 展示编号 1~N */
    uint16_t       app_id;          /* GATTC 应用 ID */
    esp_gatt_if_t  gattc_if;        /* 该应用对应的 GATTC 接口 */
    esp_bd_addr_t  bda;             /* 从机 MAC */
    uint16_t       conn_id;
    uint16_t       svc_start;       /* 服务 0xFFE0 句柄范围 */
    uint16_t       svc_end;
    bool           svc_found;
    uint16_t       notify_handle;   /* 0xFFE4 */
    uint16_t       cccd_handle;     /* 0xFFE4 的 CCCD (0x2902) */
    uint16_t       cmd_handle;      /* 0xFFE3 */
    int8_t         rssi;

    /* 节点信息（信息帧上报） */
    char           tag[16];
    char           model[16];
    char           fw[16];
    bool           info_ready;

    /* 最近一帧遥测，用于串口状态表 */
    bool           have_data;
    uint16_t       last_seq;
    uint16_t       last_adc;
    uint16_t       last_ao;
    uint32_t       last_rs;
    int16_t        last_dpct;
    uint8_t        last_alarm;
    uint8_t        last_warmup;
    uint8_t        last_do;
    uint8_t        last_muted;
    uint32_t       tele_count;      /* 收到多少帧 */
    uint8_t        have_data_alarm; /* 上一次的报警状态，用来抓 0->1 / 1->0 跳变 */
} slave_slot_t;

static slave_slot_t s_slot[MASTER_MAX_SLAVES];
static bool         s_scanning        = false;
static int          s_apps_registered = 0;

/* 服务发现失败（不是我们的节点）的设备会被拉黑, 避免"连上-断开"死循环刷屏 */
#define MASTER_MAX_BLACKLIST    4
static esp_bd_addr_t s_blacklist[MASTER_MAX_BLACKLIST];
static int           s_blacklist_count = 0;

/* ===================== 小工具函数 ===================== */
static const char *slot_state_name(slot_state_t st)
{
    switch (st) {
    case SLOT_FREE:        return "空闲";
    case SLOT_CONNECTING:  return "连接中";
    case SLOT_DISCOVERING: return "发现服务";
    case SLOT_READY:       return "已就绪";
    default:               return "?";
    }
}

static void bda_to_str(const uint8_t *bda, char *out /* >= 18 字节 */)
{
    sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

/* MAC -> 12 位十六进制(无冒号), 用作 MQTT 主题里的设备标识 */
static void bda_to_macstr(const uint8_t *bda, char *out /* >= 13 字节 */)
{
    sprintf(out, "%02X%02X%02X%02X%02X%02X",
            bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool bda_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bool bda_blacklisted(const uint8_t *bda)
{
    for (int i = 0; i < s_blacklist_count; i++) {
        if (bda_equal(s_blacklist[i], bda)) {
            return true;
        }
    }
    return false;
}

static void bda_blacklist_add(const uint8_t *bda)
{
    if (s_blacklist_count >= MASTER_MAX_BLACKLIST || bda_blacklisted(bda)) {
        return;
    }
    memcpy(s_blacklist[s_blacklist_count], bda, 6);
    s_blacklist_count++;
}

static int slot_of_bda(const uint8_t *bda)
{
    for (int i = 0; i < MASTER_MAX_SLAVES; i++) {
        if (s_slot[i].state != SLOT_FREE && bda_equal(s_slot[i].bda, bda)) {
            return i;
        }
    }
    return -1;
}

static int slot_of_conn_id(uint16_t conn_id)
{
    for (int i = 0; i < MASTER_MAX_SLAVES; i++) {
        /* 只有已完成物理连接的槽位才持有有效 conn_id */
        if ((s_slot[i].state == SLOT_DISCOVERING || s_slot[i].state == SLOT_READY) &&
            s_slot[i].conn_id == conn_id) {
            return i;
        }
    }
    return -1;
}

static int slot_of_gattc_if(esp_gatt_if_t gattc_if)
{
    if (gattc_if == ESP_GATT_IF_NONE) {
        return -1;
    }
    for (int i = 0; i < MASTER_MAX_SLAVES; i++) {
        if (s_slot[i].gattc_if == gattc_if) {
            return i;
        }
    }
    return -1;
}

static int slot_alloc(const uint8_t *bda)
{
    for (int i = 0; i < MASTER_MAX_SLAVES; i++) {
        if (s_slot[i].state == SLOT_FREE) {
            /* gattc_if / app_id / index 是注册时拿到的, 释放槽位时要保留 */
            esp_gatt_if_t keep_if  = s_slot[i].gattc_if;
            uint16_t      keep_app = s_slot[i].app_id;
            uint8_t       keep_ix  = s_slot[i].index;
            memset(&s_slot[i], 0, sizeof(s_slot[i]));
            s_slot[i].index    = keep_ix ? keep_ix : (uint8_t)(i + 1);
            s_slot[i].app_id   = keep_app;
            s_slot[i].gattc_if = keep_if;
            s_slot[i].conn_id  = 0xFFFF;
            s_slot[i].state    = SLOT_CONNECTING;
            if (bda) {
                memcpy(s_slot[i].bda, bda, 6);
            }
            return i;
        }
    }
    return -1;
}

/* 释放一个槽位（掉线 / 连接失败时调用），并补一次扫描 */
static void slot_release(int i, const char *why)
{
    if (i < 0 || i >= MASTER_MAX_SLAVES) {
        return;
    }
    if (s_slot[i].state != SLOT_FREE) {
        char mac[18] = "??";
        bda_to_str(s_slot[i].bda, mac);
        ESP_LOGW(TAG, "[节点%d] %s (%s) —— 槽位已释放, 等待重连",
                 s_slot[i].index, why, mac);
        {
            char mac12[16];
            bda_to_macstr(s_slot[i].bda, mac12);
            WifiMqtt_PublishNodeState(mac12, false, 0,
                                      s_slot[i].tag, s_slot[i].model, s_slot[i].fw);
        }
    }
    esp_gatt_if_t keep_if = s_slot[i].gattc_if;
    uint8_t       keep_ix = s_slot[i].index;
    memset(&s_slot[i], 0, sizeof(s_slot[i]));
    s_slot[i].gattc_if = keep_if;
    s_slot[i].index    = keep_ix;
    s_slot[i].app_id   = i;
    s_slot[i].conn_id  = 0xFFFF;
    s_slot[i].state    = SLOT_FREE;
}

static int slot_free_count(void)
{
    int n = 0;
    for (int i = 0; i < MASTER_MAX_SLAVES; i++) {
        if (s_slot[i].state == SLOT_FREE) {
            n++;
        }
    }
    return n;
}

static int slot_ready_count(void)
{
    int n = 0;
    for (int i = 0; i < MASTER_MAX_SLAVES; i++) {
        if (s_slot[i].state == SLOT_READY) {
            n++;
        }
    }
    return n;
}

/* ===================== 扫描控制 ===================== */
static void master_start_scan(void)
{
    if (!s_apps_registered || s_scanning) {
        return;
    }
    if (slot_free_count() == 0) {
        ESP_LOGI(TAG, "%d 个节点已占满, 停止扫描", MASTER_MAX_SLAVES);
        return;
    }
    esp_ble_gap_start_scanning(SCAN_DURATION_FOREVER);
}

static void master_stop_scan(void)
{
    if (s_scanning) {
        esp_ble_gap_stop_scanning();
    }
}

/* ===================== 连接后的 GATT 流程 ===================== */
static void slave_start_discovery(slave_slot_t *s)
{
    esp_bt_uuid_t svc_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid.uuid16 = SLAVE_SERVICE_UUID,
    };
    s->svc_found = false;
    esp_ble_gattc_search_service(s->gattc_if, s->conn_id, &svc_uuid);
}

static void slave_request_mtu(slave_slot_t *s)
{
    esp_err_t err = esp_ble_gattc_send_mtu_req(s->gattc_if, s->conn_id);
    if (err != ESP_OK) {
        /* MTU 请求没发出去就不会有 CFG_MTU_EVT, 这里直接进入服务发现 */
        ESP_LOGW(TAG, "[节点%d] MTU 请求失败(%s), 按默认 MTU 继续",
                 s->index, esp_err_to_name(err));
        slave_start_discovery(s);
    }
}

/* 找到 0xFFE4 特征及其 CCCD, 订阅通知 */
static void slave_subscribe_notify(slave_slot_t *s)
{
    esp_gattc_char_elem_t chars[8] = {0};
    uint16_t              count    = 8;
    esp_bt_uuid_t         char_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid.uuid16 = CHAR_TELE_NOTIFY_UUID,
    };

    esp_gatt_status_t st = esp_ble_gattc_get_char_by_uuid(s->gattc_if, s->conn_id,
                                                          s->svc_start, s->svc_end,
                                                          char_uuid, chars, &count);
    if (st != ESP_GATT_OK || count == 0) {
        ESP_LOGE(TAG, "[节点%d] 未找到通知特征 0x%04X (status=%d, count=%d)",
                 s->index, CHAR_TELE_NOTIFY_UUID, st, count);
        return;
    }
    s->notify_handle = chars[0].char_handle;

    esp_gattc_descr_elem_t descrs[4] = {0};
    uint16_t               dcount    = 4;
    esp_bt_uuid_t          cccd_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,
    };

    st = esp_ble_gattc_get_descr_by_char_handle(s->gattc_if, s->conn_id,
                                                s->notify_handle, cccd_uuid,
                                                descrs, &dcount);
    if (st == ESP_GATT_OK && dcount > 0) {
        s->cccd_handle = descrs[0].handle;
    }

    esp_err_t err = esp_ble_gattc_register_for_notify(s->gattc_if, s->bda, s->notify_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[节点%d] 注册通知失败: %s", s->index, esp_err_to_name(err));
        return;
    }

    /* 打开 CCCD（0x2902 写 0x0001） */
    if (s->cccd_handle) {
        uint8_t cccd_val[2] = { 0x01, 0x00 };
        esp_ble_gattc_write_char_descr(s->gattc_if, s->conn_id, s->cccd_handle,
                                       sizeof(cccd_val), cccd_val,
                                       ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    }

    ESP_LOGI(TAG, "[节点%d] 通知特征句柄 0x%04X, CCCD 句柄 0x%04X, 已发起订阅",
             s->index, s->notify_handle, s->cccd_handle);
}

/* 找到网关 → 节点的写特征, 用于后续下发命令 */
static void slave_find_cmd_char(slave_slot_t *s)
{
    esp_gattc_char_elem_t chars[8] = {0};
    uint16_t              count    = 8;
    esp_bt_uuid_t         char_uuid = {
        .len = ESP_UUID_LEN_16,
        .uuid.uuid16 = CHAR_CMD_UUID,
    };

    if (esp_ble_gattc_get_char_by_uuid(s->gattc_if, s->conn_id, s->svc_start, s->svc_end,
                                       char_uuid, chars, &count) == ESP_GATT_OK && count > 0) {
        s->cmd_handle = chars[0].char_handle;
    }
}

/* 给节点写一段数据（写入 0xFFE3）, 节点按命令字执行（消音/自检/复位） */
static void slave_send_data(slave_slot_t *s, const uint8_t *data, size_t len)
{
    if (s->cmd_handle == 0 || len == 0) {
        return;
    }
    if (len > SLAVE_CMD_MAX_LEN) {
        len = SLAVE_CMD_MAX_LEN;
    }
    esp_ble_gattc_write_char(s->gattc_if, s->conn_id, s->cmd_handle,
                             len, (uint8_t *)data,
                             ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

/* 云端下发指令: 按 MAC 找到对应的节点, 把字符串通过 BLE 写过去 */
static void on_cloud_command(const char *mac, const uint8_t *data, size_t len)
{
    char text[SLAVE_CMD_MAX_LEN + 1];
    size_t n = (len < sizeof(text) - 1) ? len : sizeof(text) - 1;
    memcpy(text, data, n);
    text[n] = '\0';

    int idx = -1;
    for (int i = 0; i < MASTER_MAX_SLAVES; i++) {
        if (s_slot[i].state == SLOT_FREE) {
            continue;
        }
        char mac12[16];
        bda_to_macstr(s_slot[i].bda, mac12);
        if (strcmp(mac12, mac) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0 || s_slot[idx].state != SLOT_READY) {
        ESP_LOGW(TAG, "云端指令目标不可用: mac=%s text=%s", mac, text);
        WifiMqtt_PublishAck(mac, text, false);
        return;
    }

    slave_send_data(&s_slot[idx], data, n);
    ESP_LOGI(TAG, "★ 云端指令已转发给 节点%d: \"%s\"", s_slot[idx].index, text);
    WifiMqtt_PublishAck(mac, text, true);
}

/* ===================== 节点上报帧解析 ===================== */

/* 解析节点发来的 20 字节信息帧: "N01|MQ-2|1.0" */
static void handle_info_frame(int idx, const uint8_t *data, int len)
{
    slave_slot_t *s = &s_slot[idx];
    char  text[32];
    int   n = len - 2;
    char *p;

    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(text) - 1) {
        n = sizeof(text) - 1;
    }
    memset(text, 0, sizeof(text));
    memcpy(text, data + 2, n);
    /* 去掉尾部填充的 0x00，顺便保证字符串干净 */
    text[sizeof(text) - 1] = '\0';

    /* 格式: 节点编号|传感器型号|固件版本 */
    p = strchr(text, '|');
    if (p) {
        *p = '\0';
        strlcpy(s->tag, text, sizeof(s->tag));
        strlcpy(s->model, p + 1, sizeof(s->model));
        p = strchr(s->model, '|');
        if (p) {
            *p = '\0';
            strlcpy(s->fw, p + 1, sizeof(s->fw));
        }
    } else {
        strlcpy(s->tag, text, sizeof(s->tag));
    }
    s->info_ready = true;

    {
        char mac12[16];
        bda_to_macstr(s->bda, mac12);
        WifiMqtt_PublishNodeState(mac12, true, s->rssi, s->tag, s->model, s->fw);
    }
    ESP_LOGI(TAG, "[节点%d] 信息帧: 编号=%s 型号=%s 版本=%s",
             s->index, s->tag, s->model, s->fw);
}

/* 解析 16 字节遥测帧 -> 上报 MQTT；报警出现/解除时另发事件 */
static void handle_telemetry_frame(int idx, const uint8_t *data, int len)
{
    slave_slot_t *s = &s_slot[idx];
    uint16_t sum = 0;
    uint16_t adc, ao, dpct_u16;
    uint32_t rs;
    int16_t  dpct;
    uint8_t  flags;
    int      alarm, warmup, do_level, muted;
    char     mac12[16];

    if (len < NODE_TELEMETRY_LEN) {
        ESP_LOGW(TAG, "[节点%d] 遥测帧长度不对: %d", s->index, len);
        return;
    }
    for (int i = 0; i < TELE_OFF_SUM; i++) {
        sum += data[i];
    }
    if ((uint8_t)(sum & 0xFF) != data[TELE_OFF_SUM]) {
        ESP_LOGW(TAG, "[节点%d] 遥测帧校验失败, 丢弃", s->index);
        return;
    }

    flags    = data[TELE_OFF_FLAGS];
    adc      = (uint16_t)(data[TELE_OFF_ADC] | (data[TELE_OFF_ADC + 1] << 8));
    ao       = (uint16_t)(data[TELE_OFF_AO_MV] | (data[TELE_OFF_AO_MV + 1] << 8));
    rs       = (uint32_t)data[TELE_OFF_RS] |
               ((uint32_t)data[TELE_OFF_RS + 1] << 8) |
               ((uint32_t)data[TELE_OFF_RS + 2] << 16) |
               ((uint32_t)data[TELE_OFF_RS + 3] << 24);
    dpct_u16 = (uint16_t)(data[TELE_OFF_DPCT] | (data[TELE_OFF_DPCT + 1] << 8));
    dpct     = (int16_t)dpct_u16;

    alarm    = (flags & NODE_FLAG_ALARM)  ? 1 : 0;
    warmup   = (flags & NODE_FLAG_WARMUP) ? 1 : 0;
    do_level = (flags & NODE_FLAG_DO)     ? 1 : 0;
    muted    = (flags & NODE_FLAG_MUTED)  ? 1 : 0;

    /* 记到槽位里，供 10 秒一次的状态表打印 */
    s->last_seq    = data[TELE_OFF_SEQ];
    s->last_adc    = adc;
    s->last_ao     = ao;
    s->last_rs     = rs;
    s->last_dpct   = dpct;
    s->last_alarm  = (uint8_t)alarm;
    s->last_warmup = (uint8_t)warmup;
    s->last_do     = (uint8_t)do_level;
    s->last_muted  = (uint8_t)muted;
    s->tele_count++;

    bda_to_macstr(s->bda, mac12);

    /* 上报遥测 */
    WifiMqtt_PublishTelemetry(mac12, data[TELE_OFF_SEQ], adc, ao, rs,
                              dpct, alarm, warmup, do_level, muted, s->rssi);

    /* 报警状态跳变 -> 单独发一条事件，网页可以据此做提醒/统计 */
    if (alarm && !s->have_data_alarm) {
        ESP_LOGW(TAG, "★★★ [节点%d] 报警! Δ%%=%d.%d  ADC=%d  Rs=%lu",
                 s->index, dpct / 10, (dpct < 0 ? -dpct : dpct) % 10,
                 adc, (unsigned long)rs);
        WifiMqtt_PublishAlarmEvent(mac12, "alarm", dpct, rs, adc, s->rssi);
    } else if (!alarm && s->have_data_alarm) {
        ESP_LOGI(TAG, "✔ [节点%d] 报警解除, Δ%%=%d.%d", s->index,
                 dpct / 10, (dpct < 0 ? -dpct : dpct) % 10);
        WifiMqtt_PublishAlarmEvent(mac12, "clear", dpct, rs, adc, s->rssi);
    }
    s->have_data_alarm = (uint8_t)alarm;

    if (!s->have_data) {
        s->have_data = true;
        /* 第一次收到数据就把节点标成在线（网页立刻能看到） */
        WifiMqtt_PublishNodeState(mac12, true, s->rssi, s->tag, s->model, s->fw);
    }
}

static void handle_node_frame(int idx, const uint8_t *data, int len)
{
    if (idx < 0 || len < 2 || data[0] != NODE_FRAME_MAGIC) {
        ESP_LOGW(TAG, "收到无法识别的一帧 (len=%d, 首字节=0x%02X)",
                 len, len > 0 ? data[0] : 0);
        return;
    }

    switch (data[1]) {
    case NODE_FRAME_TELEMETRY:
        handle_telemetry_frame(idx, data, len);
        break;
    case NODE_FRAME_INFO:
        handle_info_frame(idx, data, len);
        break;
    default:
        ESP_LOGW(TAG, "[节点%d] 未知帧类型 0x%02X", s_slot[idx].index, data[1]);
        break;
    }
}

/* ===================== GAP 回调 ===================== */
static esp_ble_scan_params_t s_scan_params = {
    .scan_type          = BLE_SCAN_TYPE_ACTIVE,      /* 主动扫描, 才能拿到扫描应答里的设备名 */
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval      = 0x60,                      /* 60 ms */
    .scan_window        = 0x30,                      /* 30 ms */
};

#if MASTER_MATCH_BY_UUID_FALLBACK
static bool adv_has_service_uuid(uint8_t *adv, uint8_t type, uint16_t uuid16)
{
    uint8_t  len  = 0;
    uint8_t *data = esp_ble_resolve_adv_data(adv, type, &len);
    if (!data) {
        return false;
    }
    for (uint8_t i = 0; i + 1 < len; i += 2) {
        if ((uint16_t)(data[i] | (data[i + 1] << 8)) == uuid16) {
            return true;
        }
    }
    return false;
}
#endif

static bool adv_is_target_slave(uint8_t *adv)
{
    uint8_t  len      = 0;
    uint8_t *name     = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_NAME_CMPL, &len);
    size_t   name_len = strlen(SLAVE_NAME);

    if (name && len == name_len && memcmp(name, SLAVE_NAME, name_len) == 0) {
        return true;
    }
    /* 名称被截断时（短名称字段）也认 */
    name = esp_ble_resolve_adv_data(adv, ESP_BLE_AD_TYPE_NAME_SHORT, &len);
    if (name && len >= 4 && memcmp(name, SLAVE_NAME, len) == 0) {
        return true;
    }
#if MASTER_MATCH_BY_UUID_FALLBACK
    return adv_has_service_uuid(adv, ESP_BLE_AD_TYPE_16SRV_CMPL, SLAVE_SERVICE_UUID) ||
           adv_has_service_uuid(adv, ESP_BLE_AD_TYPE_16SRV_PART, SLAVE_SERVICE_UUID);
#else
    return false;
#endif
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        master_start_scan();
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_scanning = true;
            ESP_LOGI(TAG, "开始扫描, 等待节点广播…");
        } else {
            ESP_LOGE(TAG, "扫描启动失败: %d", param->scan_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        s_scanning = false;
        ESP_LOGI(TAG, "扫描已停止");
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
            break;
        }
        const uint8_t *bda = param->scan_rst.bda;

        if (slot_of_bda(bda) >= 0) {
            break;                                  /* 已在连接/已连接 */
        }
        if (bda_blacklisted(bda)) {
            break;                                  /* 拉黑的不再重试 */
        }
        if (slot_free_count() == 0) {
            master_stop_scan();
            break;
        }
        if (!adv_is_target_slave(param->scan_rst.ble_adv)) {
            break;
        }

        int idx = slot_alloc(bda);
        if (idx < 0) {
            break;
        }
        char mac[18];
        bda_to_str(bda, mac);
        ESP_LOGI(TAG, "发现节点 %s (类型 %d) → 分配为 节点%d, 正在连接…",
                 mac, param->scan_rst.ble_addr_type, s_slot[idx].index);

        esp_err_t err = esp_ble_gattc_open(s_slot[idx].gattc_if, (uint8_t *)bda,
                                           param->scan_rst.ble_addr_type, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "发起连接失败: %s", esp_err_to_name(err));
            slot_release(idx, "发起连接失败");
        }
        break;
    }

    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT: {
        int idx = slot_of_bda(param->read_rssi_cmpl.remote_addr);
        if (idx >= 0 && param->read_rssi_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_slot[idx].rssi = param->read_rssi_cmpl.rssi;
        }
        break;
    }

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "连接参数已更新: status=%d, interval=%d, latency=%d, timeout=%d",
                 param->update_conn_params.status,
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;

    default:
        break;
    }
}

/* ===================== GATTC 回调 ===================== */
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT: {
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "注册 GATTC 应用 %d 失败: %d", param->reg.app_id, param->reg.status);
            break;
        }
        int idx = (param->reg.app_id < MASTER_MAX_SLAVES) ? param->reg.app_id : -1;
        if (idx < 0) {
            break;
        }
        s_slot[idx].gattc_if = gattc_if;
        s_slot[idx].app_id   = param->reg.app_id;
        s_slot[idx].index    = idx + 1;
        s_slot[idx].state    = SLOT_FREE;
        s_apps_registered++;
        ESP_LOGI(TAG, "GATTC 应用 %d 注册成功 (gattc_if=%d) [%d/%d]",
                 param->reg.app_id, gattc_if, s_apps_registered, MASTER_MAX_SLAVES);

        if (s_apps_registered == MASTER_MAX_SLAVES) {
            ESP_LOGI(TAG, "蓝牙协议栈就绪, 开始扫描节点");
            esp_ble_gap_set_scan_params(&s_scan_params);
        }
        break;
    }

    case ESP_GATTC_CONNECT_EVT: {
        int idx = slot_of_bda(param->connect.remote_bda);
        if (idx < 0) {
            ESP_LOGW(TAG, "收到未知设备的连接事件 (gattc_if=%d)", gattc_if);
            break;
        }
        s_slot[idx].conn_id = param->connect.conn_id;
        s_slot[idx].state   = SLOT_DISCOVERING;

        char mac[18];
        bda_to_str(s_slot[idx].bda, mac);
        ESP_LOGI(TAG, "★ 节点%d 物理连接已建立  MAC=%s  conn_id=%d  间隔=%.2fms",
                 s_slot[idx].index, mac, param->connect.conn_id,
                 param->connect.conn_params.interval * 1.25f);

        if (slot_free_count() == 0) {
            master_stop_scan();
        }
        slave_request_mtu(&s_slot[idx]);
        break;
    }

    case ESP_GATTC_OPEN_EVT: {
        int idx = slot_of_bda(param->open.remote_bda);
        if (idx < 0) {
            break;
        }
        if (param->open.status != ESP_GATT_OK) {
            slot_release(idx, "连接建立失败");
            master_start_scan();
        } else {
            ESP_LOGI(TAG, "节点%d 连接确认成功", s_slot[idx].index);
        }
        break;
    }

    case ESP_GATTC_CFG_MTU_EVT: {
        int idx = slot_of_conn_id(param->cfg_mtu.conn_id);
        if (idx < 0) {
            break;
        }
        ESP_LOGI(TAG, "节点%d MTU 协商完成: %d 字节", s_slot[idx].index, param->cfg_mtu.mtu);
        slave_start_discovery(&s_slot[idx]);
        break;
    }

    case ESP_GATTC_SEARCH_RES_EVT: {
        int idx = slot_of_conn_id(param->search_res.conn_id);
        if (idx < 0) {
            break;
        }
        if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
            param->search_res.srvc_id.uuid.uuid.uuid16 == SLAVE_SERVICE_UUID) {
            s_slot[idx].svc_start = param->search_res.start_handle;
            s_slot[idx].svc_end   = param->search_res.end_handle;
            s_slot[idx].svc_found = true;
            ESP_LOGI(TAG, "节点%d 找到服务 0x%04X, 句柄 0x%04X~0x%04X",
                     s_slot[idx].index, SLAVE_SERVICE_UUID,
                     param->search_res.start_handle, param->search_res.end_handle);
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        int idx = slot_of_conn_id(param->search_cmpl.conn_id);
        if (idx < 0) {
            break;
        }
        if (!s_slot[idx].svc_found) {
            char mac[18];
            bda_to_str(s_slot[idx].bda, mac);
            ESP_LOGE(TAG, "节点%d (%s) 没有服务 0x%04X —— 可能不是本工程的节点, 拉黑不再重连",
                     s_slot[idx].index, mac, SLAVE_SERVICE_UUID);
            bda_blacklist_add(s_slot[idx].bda);
            esp_ble_gattc_close(s_slot[idx].gattc_if, s_slot[idx].conn_id);
            break;
        }
        slave_find_cmd_char(&s_slot[idx]);
        slave_subscribe_notify(&s_slot[idx]);
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "注册通知失败: status=%d", param->reg_for_notify.status);
            break;
        }
        /* 所有节点固件相同, 特征句柄很可能一样,
         * 所以必须用 gattc_if（每路连接唯一）来定位是从哪个节点来的 */
        int idx = slot_of_gattc_if(gattc_if);
        if (idx >= 0) {
            s_slot[idx].state = SLOT_READY;
            ESP_LOGI(TAG, "节点%d 已订阅遥测通知, 等待上报数据", s_slot[idx].index);
            {
                char mac12[16];
                bda_to_macstr(s_slot[idx].bda, mac12);
                WifiMqtt_PublishNodeState(mac12, true, s_slot[idx].rssi,
                                          s_slot[idx].tag, s_slot[idx].model, s_slot[idx].fw);
            }
        }
        break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
        int idx = slot_of_conn_id(param->notify.conn_id);
        if (idx < 0) {
            idx = slot_of_bda(param->notify.remote_bda);
        }
        if (idx >= 0 && param->notify.value_len > 0) {
            handle_node_frame(idx, param->notify.value, param->notify.value_len);
        }
        break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "CCCD 写入失败: status=%d", param->write.status);
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (param->write.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "特征写入失败: handle=0x%04X status=%d",
                     param->write.handle, param->write.status);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT: {
        int idx = slot_of_conn_id(param->disconnect.conn_id);
        if (idx < 0) {
            idx = slot_of_bda(param->disconnect.remote_bda);
        }
        if (idx >= 0) {
            char mac[18];
            bda_to_str(s_slot[idx].bda, mac);
            ESP_LOGW(TAG, "✘ 节点%d (%s) 连接断开, 原因 0x%02X",
                     s_slot[idx].index, mac, param->disconnect.reason);
            slot_release(idx, "连接断开");
        }
        master_start_scan();   /* 有空闲槽位就重新扫描 */
        break;
    }

    default:
        break;
    }
}

/* ===================== 周期状态上报 ===================== */
static void status_task(void *arg)
{
    (void)arg;
    int tick = 0;
    int hb_ticks = APP_HEARTBEAT_SEC * 1000 / APP_STATUS_PERIOD_MS;

    if (hb_ticks < 1) {
        hb_ticks = 1;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(APP_STATUS_PERIOD_MS));

        ESP_LOGI(TAG, "┌──────── 网关状态 (每 %d 秒) ────────", APP_STATUS_PERIOD_MS / 1000);
        for (int i = 0; i < MASTER_MAX_SLAVES; i++) {
            if (s_slot[i].state == SLOT_FREE) {
                ESP_LOGI(TAG, "│ 节点%d  %-8s  (等待节点上线)", s_slot[i].index,
                         slot_state_name(s_slot[i].state));
                continue;
            }
            char mac[18];
            bda_to_str(s_slot[i].bda, mac);
            esp_ble_gap_read_rssi(s_slot[i].bda);   /* 结果在下一次状态表打印 */
            if (s_slot[i].have_data) {
                int16_t d = s_slot[i].last_dpct;
                ESP_LOGI(TAG, "│ 节点%d  %-8s %s[%s/%s]  ADC=%d Rs=%lu D=%s%d.%d%% ALM=%d %s",
                         s_slot[i].index, slot_state_name(s_slot[i].state),
                         mac, s_slot[i].tag, s_slot[i].model,
                         s_slot[i].last_adc, (unsigned long)s_slot[i].last_rs,
                         (d < 0) ? "-" : "+", (int)((d < 0 ? -d : d) / 10),
                         (int)((d < 0 ? -d : d) % 10),
                         s_slot[i].last_alarm, s_slot[i].last_warmup ? "预热中" : "");
            } else {
                ESP_LOGI(TAG, "│ 节点%d  %-8s %s  RSSI=%d dBm (等数据)",
                         s_slot[i].index, slot_state_name(s_slot[i].state),
                         mac, s_slot[i].rssi);
            }
        }
        ESP_LOGI(TAG, "│ 已就绪 %d / %d        %s", slot_ready_count(), MASTER_MAX_SLAVES,
                 s_scanning ? "(正在扫描…)" : "(扫描已停止)");
        ESP_LOGI(TAG, "│ WiFi:%s  MQTT:%s",
                 WifiMqtt_IsWifiReady() ? "已连接" : "未连接",
                 WifiMqtt_IsConnected() ? "已连接" : "未连接");
        ESP_LOGI(TAG, "└─────────────────────────────────────");

        tick++;
        if (tick >= hb_ticks) {
            tick = 0;
            WifiMqtt_PublishGatewayState(slot_ready_count(), MASTER_MAX_SLAVES);
            for (int i = 0; i < MASTER_MAX_SLAVES; i++) {
                if (s_slot[i].state == SLOT_READY) {
                    char mac12[16];
                    bda_to_macstr(s_slot[i].bda, mac12);
                    WifiMqtt_PublishNodeState(mac12, true, s_slot[i].rssi,
                                              s_slot[i].tag, s_slot[i].model, s_slot[i].fw);
                }
            }
        }
    }
}

/* ===================== 入口 ===================== */
void app_main(void)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " ESP32-S3 烟雾报警网关 已启动");
    ESP_LOGI(TAG, " 目标: 自动连接 %d 个 CH573F 烟雾节点 (广播名 %s)",
             MASTER_MAX_SLAVES, SLAVE_NAME);
    ESP_LOGI(TAG, " 上报: WiFi + MQTT -> %s", APP_MQTT_URI);
    ESP_LOGI(TAG, "==================================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* ESP32-S3 没有经典蓝牙, 这一步失败不影响 BLE */
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "释放经典蓝牙内存: %s (S3 无经典蓝牙属正常)", esp_err_to_name(ret));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event_handler));

    {
        char mac[18];
        bda_to_str(esp_bt_dev_get_address(), mac);
        ESP_LOGI(TAG, "网关蓝牙地址: %s", mac);
    }

    /* 为每一路连接注册一个独立的 GATTC 应用（Bluedroid 中一路连接对应一个 gattc_if） */
    for (uint16_t i = 0; i < MASTER_MAX_SLAVES; i++) {
        s_slot[i].index    = i + 1;
        s_slot[i].app_id   = i;
        s_slot[i].gattc_if = ESP_GATT_IF_NONE;
        s_slot[i].conn_id  = 0xFFFF;
        s_slot[i].state    = SLOT_FREE;
        ESP_ERROR_CHECK(esp_ble_gattc_app_register(i));
    }

    /* 云端链路: 连 WiFi + MQTT 上报/下发 */
    WifiMqtt_RegisterCmdHandler(on_cloud_command);
    WifiMqtt_Init();

    xTaskCreate(status_task, "gw_status", 4096, NULL, 4, NULL);
}
