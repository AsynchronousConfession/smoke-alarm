/*******************************************************************************
 * ESP32-S3 烟雾报警网关 —— 唯一需要修改的配置文件
 *
 * 改完直接:  powershell -ExecutionPolicy Bypass -File .\tools\build_gateway.ps1 -Port COM8 -Flash
 ******************************************************************************/
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---------------- 家庭 WiFi / 笔记本热点 ---------------- */
/* 注意: 笔记本移动热点的名称和密码, 就是这里要填的内容
 *      （移动热点必须选 2.4GHz，ESP32-S3 不支持 5GHz） */
#define APP_WIFI_SSID           "YOUR_WIFI_SSID"
#define APP_WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"

/* ---------------- MQTT 服务器 ---------------- */
/* 两种情况二选一：
 *   ① 服务器跑在笔记本上（默认）：
 *      笔记本开移动热点时地址通常是 192.168.137.1；
 *      实际地址在 PC 上运行 server\start.ps1 时会打印出来，照着填即可。
 *   ② 服务器部署到云服务器上（本项目已部署，云服务器公网 IP = 192.0.2.10）：
 *      把下面那行换成：
 *          #define APP_MQTT_URI  "mqtt://192.0.2.10:1883"
 *      （部署步骤见 deploy\README_部署到阿里云.md；账号密码同样用下面这两个） */
/* 【当前生效】云服务器（已部署）： */
#define APP_MQTT_URI            "mqtt://192.0.2.10:1883"
/* 【本地调试备用】笔记本上跑 server\start.ps1 时用这一行（注释掉上面那行）：
 *   #define APP_MQTT_URI         "mqtt://192.168.137.1:1883"   // 笔记本移动热点
 *   #define APP_MQTT_URI         "mqtt://192.168.1.100:1883"     // 家里路由器 */
#define APP_MQTT_USERNAME       "esp32gw"
#define APP_MQTT_PASSWORD       "CHANGE_ME_DEVICE_PW"

/* ---------------- 网关编号(与 PC 端 config.json 的 gateway_id 一致) -------- */
#define APP_GATEWAY_ID          "gw01"
#define APP_TOPIC_PREFIX        "home/" APP_GATEWAY_ID
#define APP_GW_FW_VER           "1.0"

/* ---------------- BLE 从机(节点)匹配 ---------------- */
/* 节点广播名，必须与节点固件 node_cfg.h 里的 NODE_ADV_NAME 完全一致 */
#define APP_NODE_ADV_NAME       "SMOKE_NODE"

/* ---------------- 组网规模与节奏 ---------------- */
#define APP_MAX_NODES           4       /* 最多同时连接的节点数(家里几个房间就填几) */
#define APP_HEARTBEAT_SEC       30      /* 网关心跳/节点状态重发周期(秒) */
#define APP_STATUS_PERIOD_MS    10000   /* 串口状态表打印周期(毫秒) */
#define APP_OFFLINE_BUF_SLOTS   32      /* 断网期间缓存的最大消息条数 */

/* 修改 APP_MAX_NODES 后必须同步改 sdkconfig.defaults 里的
 * CONFIG_BT_ACL_CONNECTIONS（建议 = APP_MAX_NODES + 1），
 * 然后执行一次 tools\build_gateway.ps1 -FullClean 让它重新生效。 */

#endif /* APP_CONFIG_H */
