/********************************** (C) COPYRIGHT *******************************
 * File Name          : peripheral.h
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/11
 * Description        :
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *******************************************************************************/

#ifndef PERIPHERAL_H
#define PERIPHERAL_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************************************************************
 * 头文件包含
 */

/*********************************************************************
 * 常量定义
 */

// 从机应用任务的事件位
#define SBP_START_DEVICE_EVT    0x0001
#define SBP_PERIODIC_EVT        0x0002
#define SBP_READ_RSSI_EVT       0x0004
#define SBP_PARAM_UPDATE_EVT    0x0008

/*********************************************************************
 * 宏定义
 */
typedef struct
{
    uint16_t connHandle; // 当前连接的句柄
    uint16_t connInterval;
    uint16_t connSlaveLatency;
    uint16_t connTimeout;
} peripheralConnItem_t;

/*********************************************************************
 * 函数声明
 */

/*
 * BLE 应用任务初始化
 */
extern void Peripheral_Init(void);

/*
 * BLE 应用任务的事件处理函数
 */
extern uint16_t Peripheral_ProcessEvent(uint8_t task_id, uint16_t events);

/*********************************************************************
*********************************************************************/

#ifdef __cplusplus
}
#endif

#endif
