/*******************************************************************************
 * WiFi + MQTT 上报/下发模块（烟雾报警网关）
 *
 *   CH573F 节点 --BLE--> ESP32-S3 网关 --家里WiFi--> Mosquitto(笔记本)
 *                                                      <--> 后端/网页
 *
 * 上报:
 *   home/<gw>/tele/<MAC>    遥测（ADC/Rs/Δ%/报警标志/信号强度）
 *   home/<gw>/event/<MAC>   报警出现/解除事件
 *   home/<gw>/state/<MAC>   节点在线状态 + 型号（retained）
 *   home/<gw>/state/gateway 网关心跳（retained + 遗嘱消息）
 *   home/<gw>/ack/<MAC>     指令回执
 * 下发:
 *   home/<gw>/cmd/<MAC>     纯文本命令 -> 回调给 gateway_main.c -> BLE 写给节点
 *******************************************************************************/
#ifndef WIFI_MQTT_H
#define WIFI_MQTT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 收到云端下发指令时的回调: mac 为 12 位十六进制字符串(无冒号) */
typedef void (*wifi_mqtt_cmd_cb_t)(const char *mac, const uint8_t *data, size_t len);

/* 初始化 WiFi(STA) + SNTP + MQTT 客户端, 内部处理断线重连 */
void WifiMqtt_Init(void);

/* MQTT 是否已连接（未连接时上报会自动进入断网缓存） */
bool WifiMqtt_IsConnected(void);

/* WiFi 是否已拿到 IP */
bool WifiMqtt_IsWifiReady(void);

/* [阶段4] 设置页显示用：当前 IP 字符串、WiFi 信号强度(dBm，未连接返回 0) */
void WifiMqtt_GetIp(char *out, size_t n);
int  WifiMqtt_GetWifiRssi(void);

/* 注册云端指令回调 */
void WifiMqtt_RegisterCmdHandler(wifi_mqtt_cmd_cb_t cb);

/* 上报一条遥测（dpct_x10 = Δ% ×10，例如 -523 表示 −52.3%） */
void WifiMqtt_PublishTelemetry(const char *mac, int seq, int adc, int ao_mv,
                               uint32_t rs_ohm, int dpct_x10, int alarm,
                               int warmup, int do_level, int muted, int rssi);

/* 上报报警/解除事件: kind = "alarm" / "clear" */
void WifiMqtt_PublishAlarmEvent(const char *mac, const char *kind,
                                int dpct_x10, uint32_t rs_ohm, int adc, int rssi);

/* 上报节点在线状态（retained，网页刚打开也能立刻看到） */
void WifiMqtt_PublishNodeState(const char *mac, bool online, int rssi,
                               const char *tag, const char *model, const char *fw);

/* 上报网关自身状态（IP / WiFi 信号 / 在线节点数） */
void WifiMqtt_PublishGatewayState(int nodes_online, int nodes_max);

/* 回执: 告知云端某条指令已下发到节点 */
void WifiMqtt_PublishAck(const char *mac, const char *text, bool ok);

#endif /* WIFI_MQTT_H */
