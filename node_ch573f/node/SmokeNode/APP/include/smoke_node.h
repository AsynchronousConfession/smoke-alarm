/********************************** (C) COPYRIGHT *******************************
 * File Name          : smoke_node.h
 * Description        : 烟雾报警节点应用层（替代原例程的 slave_app.c）
 *
 *   节点硬件：CH573F 最小系统板 + MQ 传感器 + 蜂鸣器（无 OLED、无按键）
 *
 *   职责:
 *     1) 每个 NODE_SAMPLE_PERIOD_MS 采一次 MQ 传感器（smoke_sensor.c）
 *     2) 按周期把遥测帧通过 CHAR4(0xFFE4) 通知给 ESP32 网关
 *        —— 正常 2s 一次，报警时 0.5s 一次
 *     3) 收到网关写 CHAR3(0xFFE3) 时解析命令：M 消音 / T 自检 / R 复位
 *     4) 本地唯一的输出是蜂鸣器，其余状态看串口日志
 *
 *   BLE 链路状态由 peripheral.c 通过 SmokeNode_OnLinkUp / OnLinkDown 通知进来，
 *   协议帧格式见 node_proto.h。
 *******************************************************************************/

#ifndef SMOKE_NODE_H
#define SMOKE_NODE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "CH57x_common.h"

/* 应用初始化：初始化传感器与蜂鸣器，并启动节点周期任务。
 * 在 Peripheral_Init() 之后调用一次（此时 TMOS 已经跑起来了）。 */
void SmokeNode_Init(void);

/* BLE 链路建立 / 断开（由 peripheral.c 调用） */
void SmokeNode_OnLinkUp(uint16_t connHandle, uint16_t connInterval);
void SmokeNode_OnLinkDown(void);

/* 当前是否已连接网关 */
uint8_t SmokeNode_IsConnected(void);

/* 网关写入 CHAR3(0xFFE3) 的数据（由 peripheral.c 调用） */
void SmokeNode_OnHostWrite(uint8_t *pValue, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* SMOKE_NODE_H */
