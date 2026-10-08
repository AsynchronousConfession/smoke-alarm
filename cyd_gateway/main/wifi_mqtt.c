/*******************************************************************************
 * WiFi + MQTT 上报/下发模块（烟雾报警网关）
 *
 * 设计要点（沿用原网关工程的成熟做法）:
 *   - MQTT 服务器在同一局域网，走明文 1883，不暴露到公网
 *   - 遗嘱消息(LWT): 网关异常掉线时云端立刻把它标记成离线
 *   - 断网缓存: MQTT 未连接时数据先进内存环形队列，连上后补发
 *   - 时间戳: SNTP 同步；没同步上就发 0，由后端补当前时间
 *******************************************************************************/

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "mqtt_client.h"

#include "app_config.h"
#include "wifi_mqtt.h"

static const char *TAG = "NET";

#define OFF_TOPIC_LEN   72
#define OFF_PAYLOAD_LEN 256
#define CMD_PREFIX      APP_TOPIC_PREFIX "/cmd/"

typedef struct {
    char topic[OFF_TOPIC_LEN];
    char payload[OFF_PAYLOAD_LEN];
    int  retain;
} off_item_t;

static off_item_t        s_off[APP_OFFLINE_BUF_SLOTS];
static int               s_off_head  = 0;    /* 最老的一条 */
static int               s_off_count = 0;
static SemaphoreHandle_t s_off_lock;

static esp_mqtt_client_handle_t s_mqtt;
static esp_netif_t             *s_netif;
static volatile bool            s_wifi_ready   = false;
static volatile bool            s_mqtt_ready   = false;
static volatile bool            s_mqtt_started = false;
static int                      s_nodes_online = 0;
static int                      s_nodes_max    = 0;
static wifi_mqtt_cmd_cb_t       s_cmd_cb = NULL;

/* WiFi 就绪后才启动 MQTT 客户端; 两个条件谁先到都行 */
static void mqtt_start_if_ready(void)
{
    if (s_mqtt && s_wifi_ready && !s_mqtt_started) {
        s_mqtt_started = true;
        esp_mqtt_client_start(s_mqtt);
        ESP_LOGI(TAG, "正在连接 MQTT: %s", APP_MQTT_URI);
    }
}

/* 云端下发指令的组包缓存(支持分片) */
static char s_cmd_mac[16];
static char s_cmd_buf[64];
static int  s_cmd_len = 0;

/* ------------------------------------------------------------------------- */
/* 工具                                                                       */
/* ------------------------------------------------------------------------- */
static time_t now_ts(void)
{
    time_t t = 0;
    time(&t);
    /* 没同步上 SNTP 时是 1970 年, 直接给 0 让后端补时间 */
    return (t > 1600000000) ? t : 0;
}

/* 把 Δ%×10 格式化成 "-52.3" 这样的字符串（避免用浮点 printf） */
static void fmt_dpct(char *out, size_t n, int d10)
{
    int a = (d10 < 0) ? -d10 : d10;
    snprintf(out, n, "%s%d.%d", (d10 < 0) ? "-" : "", a / 10, a % 10);
}

/* ------------------------------------------------------------------------- */
/* 断网缓存                                                                   */
/* ------------------------------------------------------------------------- */
static void off_push(const char *topic, const char *payload, int retain)
{
    if (xSemaphoreTake(s_off_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }
    if (s_off_count >= APP_OFFLINE_BUF_SLOTS) {
        /* 满了就丢最老的, 保证最新数据能进来 */
        s_off_head = (s_off_head + 1) % APP_OFFLINE_BUF_SLOTS;
        s_off_count--;
        ESP_LOGW(TAG, "断网缓存已满, 丢弃最老的一条");
    }
    int idx = (s_off_head + s_off_count) % APP_OFFLINE_BUF_SLOTS;
    strlcpy(s_off[idx].topic, topic, sizeof(s_off[idx].topic));
    strlcpy(s_off[idx].payload, payload, sizeof(s_off[idx].payload));
    s_off[idx].retain = retain;
    s_off_count++;
    xSemaphoreGive(s_off_lock);
}

static void off_flush(void)
{
    while (1) {
        char topic[OFF_TOPIC_LEN];
        char payload[OFF_PAYLOAD_LEN];
        int  retain;

        if (xSemaphoreTake(s_off_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
            return;
        }
        if (s_off_count == 0) {
            xSemaphoreGive(s_off_lock);
            return;
        }
        strlcpy(topic, s_off[s_off_head].topic, sizeof(topic));
        strlcpy(payload, s_off[s_off_head].payload, sizeof(payload));
        retain = s_off[s_off_head].retain;
        s_off_head = (s_off_head + 1) % APP_OFFLINE_BUF_SLOTS;
        s_off_count--;
        xSemaphoreGive(s_off_lock);

        int id = esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, retain);
        if (id < 0) {
            ESP_LOGW(TAG, "补发失败, 剩下的下次再传");
            return;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 发布                                                                       */
/* ------------------------------------------------------------------------- */
static void mqtt_pub(const char *topic, const char *payload, const char *what, int retain)
{
    if (s_mqtt_ready && s_mqtt) {
        /* [CYD port] 必须用 enqueue 而不是 publish：
         * esp_mqtt_client_publish() 会在"调用者的任务上下文"里直接写 socket，
         * 而网关的遥测/事件都是 BLE 的 GATTC 回调里发出来的 —— 一旦 WiFi 侧写入变慢
         * （报警时上报速率翻 4 倍、又和 BLE 扫描抢射频），BLE 任务就被卡住，
         * 节点那条 1 秒的监督超时到期 → 断链重连，表现为"一报警就反复重连"。
         * enqueue 只把消息放进发送队列，真正的发送交给 MQTT 任务，不会阻塞 BLE。 */
        int id = esp_mqtt_client_enqueue(s_mqtt, topic, payload, 0, 1, retain, false);
        if (id >= 0) {
            ESP_LOGI(TAG, "↑ %s %s", what, payload);
            return;
        }
        ESP_LOGW(TAG, "入队失败(队列满?), 转入缓存");
    }
    off_push(topic, payload, retain);
}

void WifiMqtt_PublishTelemetry(const char *mac, int seq, int adc, int ao_mv,
                               uint32_t rs_ohm, int dpct_x10, int alarm,
                               int warmup, int do_level, int muted, int rssi)
{
    char topic[OFF_TOPIC_LEN];
    char payload[OFF_PAYLOAD_LEN];
    char d[12];

    fmt_dpct(d, sizeof(d), dpct_x10);
    snprintf(topic, sizeof(topic), "%s/tele/%s", APP_TOPIC_PREFIX, mac);
    snprintf(payload, sizeof(payload),
             "{\"ts\":%ld,\"mac\":\"%s\",\"seq\":%d,\"adc\":%d,\"ao\":%d,"
             "\"rs\":%lu,\"dpct\":%s,\"alarm\":%d,\"warmup\":%d,\"do\":%d,"
             "\"muted\":%d,\"rssi\":%d}",
             (long)now_ts(), mac, seq, adc, ao_mv,
             (unsigned long)rs_ohm, d, alarm, warmup, do_level, muted, rssi);
    mqtt_pub(topic, payload, "tele", 0);
}

void WifiMqtt_PublishAlarmEvent(const char *mac, const char *kind,
                                int dpct_x10, uint32_t rs_ohm, int adc, int rssi)
{
    char topic[OFF_TOPIC_LEN];
    char payload[OFF_PAYLOAD_LEN];
    char d[12];

    fmt_dpct(d, sizeof(d), dpct_x10);
    snprintf(topic, sizeof(topic), "%s/event/%s", APP_TOPIC_PREFIX, mac);
    snprintf(payload, sizeof(payload),
             "{\"ts\":%ld,\"mac\":\"%s\",\"kind\":\"%s\",\"dpct\":%s,"
             "\"rs\":%lu,\"adc\":%d,\"rssi\":%d}",
             (long)now_ts(), mac, kind, d, (unsigned long)rs_ohm, adc, rssi);
    mqtt_pub(topic, payload, "event", 0);
}

void WifiMqtt_PublishNodeState(const char *mac, bool online, int rssi,
                               const char *tag, const char *model, const char *fw)
{
    char topic[OFF_TOPIC_LEN];
    char payload[OFF_PAYLOAD_LEN];

    snprintf(topic, sizeof(topic), "%s/state/%s", APP_TOPIC_PREFIX, mac);
    snprintf(payload, sizeof(payload),
             "{\"ts\":%ld,\"mac\":\"%s\",\"online\":%s,\"rssi\":%d,"
             "\"tag\":\"%s\",\"model\":\"%s\",\"fw\":\"%s\"}",
             (long)now_ts(), mac, online ? "true" : "false", rssi,
             tag ? tag : "", model ? model : "", fw ? fw : "");
    mqtt_pub(topic, payload, "state", 1);      /* retain: 网页一打开就能看到 */
}

void WifiMqtt_PublishGatewayState(int nodes_online, int nodes_max)
{
    char topic[OFF_TOPIC_LEN];
    char payload[OFF_PAYLOAD_LEN];
    char ip[20] = "0.0.0.0";
    int  rssi = 0;

    s_nodes_online = nodes_online;
    s_nodes_max    = nodes_max;

    if (s_netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    snprintf(topic, sizeof(topic), "%s/state/gateway", APP_TOPIC_PREFIX);
    snprintf(payload, sizeof(payload),
             "{\"ts\":%ld,\"online\":true,\"ip\":\"%s\",\"wifi_rssi\":%d,"
             "\"nodes\":%d,\"nodes_max\":%d,\"fw\":\"%s\"}",
             (long)now_ts(), ip, rssi, nodes_online, nodes_max, APP_GW_FW_VER);
    mqtt_pub(topic, payload, "gateway", 1);
}

void WifiMqtt_PublishAck(const char *mac, const char *text, bool ok)
{
    char topic[OFF_TOPIC_LEN];
    char payload[OFF_PAYLOAD_LEN];

    snprintf(topic, sizeof(topic), "%s/ack/%s", APP_TOPIC_PREFIX, mac);
    snprintf(payload, sizeof(payload),
             "{\"ts\":%ld,\"mac\":\"%s\",\"text\":\"%s\",\"ok\":%s}",
             (long)now_ts(), mac, text, ok ? "true" : "false");
    mqtt_pub(topic, payload, "ack", 0);
}

/* ------------------------------------------------------------------------- */
/* MQTT 事件                                                                  */
/* ------------------------------------------------------------------------- */
static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_ready = true;
        ESP_LOGI(TAG, "MQTT 已连接: %s", APP_MQTT_URI);
        esp_mqtt_client_subscribe(s_mqtt, APP_TOPIC_PREFIX "/cmd/+", 1);
        off_flush();
        WifiMqtt_PublishGatewayState(s_nodes_online, s_nodes_max);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_ready = false;
        ESP_LOGW(TAG, "MQTT 断开, 数据将进入断网缓存");
        break;

    case MQTT_EVENT_DATA: {
        const size_t prefix_len = strlen(CMD_PREFIX);
        if (e->current_data_offset == 0) {
            s_cmd_len = 0;
            s_cmd_buf[0] = 0;
            if (e->topic_len > (int)prefix_len &&
                strncmp(e->topic, CMD_PREFIX, prefix_len) == 0) {
                int n = e->topic_len - (int)prefix_len;
                if (n > (int)sizeof(s_cmd_mac) - 1) {
                    n = sizeof(s_cmd_mac) - 1;
                }
                memcpy(s_cmd_mac, e->topic + prefix_len, n);
                s_cmd_mac[n] = 0;
            } else {
                s_cmd_mac[0] = 0;           /* 不是指令主题, 忽略 */
            }
        }

        if (s_cmd_mac[0] == 0) {
            break;
        }
        int copy = e->data_len;
        if (s_cmd_len + copy > (int)sizeof(s_cmd_buf) - 1) {
            copy = sizeof(s_cmd_buf) - 1 - s_cmd_len;
        }
        if (copy > 0) {
            memcpy(s_cmd_buf + s_cmd_len, e->data, copy);
            s_cmd_len += copy;
            s_cmd_buf[s_cmd_len] = 0;
        }
        if (e->current_data_offset + e->data_len >= e->total_data_len) {
            ESP_LOGI(TAG, "↓ 云端指令 mac=%s text=%s", s_cmd_mac, s_cmd_buf);
            if (s_cmd_cb) {
                s_cmd_cb(s_cmd_mac, (const uint8_t *)s_cmd_buf, (size_t)s_cmd_len);
            }
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT 错误 (会按指数退避自动重连)");
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* WiFi                                                                       */
/* ------------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_ready = false;
        ESP_LOGW(TAG, "WiFi 断开, 1s 后重连");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        s_wifi_ready = true;
        ESP_LOGI(TAG, "WiFi 已连接, IP: " IPSTR, IP2STR(&evt->ip_info.ip));

        if (!esp_sntp_enabled()) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_setservername(1, "cn.pool.ntp.org");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP 已启动, 用于给上报数据打时间戳");
        }
        mqtt_start_if_ready();
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, APP_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, APP_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "正在连接 WiFi: %s", APP_WIFI_SSID);
}

/* ------------------------------------------------------------------------- */
/* 对外接口                                                                   */
/* ------------------------------------------------------------------------- */
void WifiMqtt_Init(void)
{
    /* 时区: 让 time() 出来的是北京时间 */
    setenv("TZ", "CST-8", 1);
    tzset();

    if (!s_off_lock) {
        s_off_lock = xSemaphoreCreateMutex();
    }

    wifi_init();

    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri          = APP_MQTT_URI,
        .credentials.username        = APP_MQTT_USERNAME,
        /* 【重要】IDF 5.1 的密码字段是 credentials.authentication.password,
         * 漏掉这一行会用空密码连接, 被 Mosquitto 拒绝(网页一直显示网关离线) */
        .credentials.authentication.password = APP_MQTT_PASSWORD,
        .credentials.client_id       = "esp32gw-" APP_GATEWAY_ID,
        .session.keepalive           = 30,
        .session.last_will = {
            .topic   = APP_TOPIC_PREFIX "/state/gateway",
            .msg     = "{\"online\":false}",
            .msg_len = 16,          /* {"online":false} 正好 16 字节, 不能多 */
            .qos     = 1,
            .retain  = 1,
        },
        .network.reconnect_timeout_ms = 3000,
    };
    s_mqtt = esp_mqtt_client_init(&mcfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    /* 如果 WiFi 比 MQTT 客户端先就绪, 这里补一次启动 */
    mqtt_start_if_ready();
}

bool WifiMqtt_IsConnected(void)
{
    return s_mqtt_ready;
}

bool WifiMqtt_IsWifiReady(void)
{
    return s_wifi_ready;
}

void WifiMqtt_GetIp(char *out, size_t n)
{
    if (out == NULL || n == 0) {
        return;
    }
    snprintf(out, n, "0.0.0.0");
    if (s_netif != NULL) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
            snprintf(out, n, IPSTR, IP2STR(&info.ip));
        }
    }
}

int WifiMqtt_GetWifiRssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

void WifiMqtt_RegisterCmdHandler(wifi_mqtt_cmd_cb_t cb)
{
    s_cmd_cb = cb;
}
