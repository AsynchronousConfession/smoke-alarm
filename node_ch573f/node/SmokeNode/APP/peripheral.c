/********************************** (C) COPYRIGHT *******************************
 * File Name          : peripheral.C
 * Author             : WCH
 * Version            : V1.0
 * Date               : 2018/12/10
 * Description        : 外设从机多连接应用程序，初始化广播连接参数，然后广播，连接主机后，
 *                      请求更新连接参数，通过自定义服务传输数据
 * Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0
 *******************************************************************************/

/*********************************************************************
 * 头文件包含
 */
#include "CONFIG.h"
#include "devinfoservice.h"
#include "gattprofile.h"
#include "peripheral.h"
#include "node_cfg.h"
#include "smoke_node.h"

/*********************************************************************
 * 宏定义
 */

/*********************************************************************
 * 常量定义
 */

// 周期事件（CHAR4 通知）的执行周期，单位 625us：1600 × 625us = 1s
// 【本工程改动】通知改由节点周期任务触发（见 smoke_node.c），这里置 0 关闭原有的每秒通知
#define SBP_PERIODIC_EVT_PERIOD              0

// 读取 RSSI 事件的执行周期，单位 625us：3200 × 625us = 2s
#define SBP_READ_RSSI_EVT_PERIOD             3200

// 连接参数更新延时，单位 625us：6400 × 625us = 4s
// （连上后先留时间给主机完成服务发现，再发起更新请求）
#define SBP_PARAM_UPDATE_DELAY               6400

// 可发现状态下的广播间隔，单位 625us：80 × 625us = 50ms
#define DEFAULT_ADVERTISING_INTERVAL         80

// 受限可发现模式（LIMITED）：广播 30.72s 后自动停止
// 通用可发现模式（GENERAL）：一直广播，不会自动停止
#define DEFAULT_DISCOVERABLE_MODE            GAP_ADTYPE_FLAGS_GENERAL

// 可接受的最小连接间隔，单位 1.25ms：6 × 1.25ms = 7.5ms
#define DEFAULT_DESIRED_MIN_CONN_INTERVAL    6

// 可接受的最大连接间隔，单位 1.25ms：100 × 1.25ms = 125ms
#define DEFAULT_DESIRED_MAX_CONN_INTERVAL    100

// 参数更新时使用的从机延迟（每个连接周期可跳过、不响应的次数）
#define DEFAULT_DESIRED_SLAVE_LATENCY        0

// 监督超时时间，单位 10ms：100 × 10ms = 1s（超时收不到主机数据即判定断线）
#define DEFAULT_DESIRED_CONN_TIMEOUT         100

// 厂商标识：沁恒（WCH）
#define WCH_COMPANY_ID                       0x07D7

/*********************************************************************
 * 类型定义
 */

/*********************************************************************
 * 全局变量
 */

/*********************************************************************
 * 外部变量
 */

/*********************************************************************
 * 外部函数
 */

/*********************************************************************
 * 局部变量
 */
static uint8_t Peripheral_TaskID = INVALID_TASK_ID; // 本任务在 TMOS 中的任务 ID，用于收发消息、启停定时器

// GAP 扫描响应包（SCAN_RSP）数据，最大 31 字节：手机/网关发扫描请求时回给它的内容。
// 【本工程改动】内容在 Peripheral_Init() 里按 NODE_ADV_NAME 现场拼装，
// 这样改广播名只要改 node_cfg.h 一处，不会出现"长度字节和名字字符数对不上"的坑。
static uint8_t scanRspData[31];
static uint8_t scanRspDataLen = 0;

/*********************************************************************
 * @fn      Peripheral_BuildScanRsp
 *
 * @brief   拼装扫描响应数据：完整设备名 + 期望连接间隔 + 发射功率
 *
 * @return  无
 */
static void Peripheral_BuildScanRsp(void)
{
    uint8_t     n = 0;
    uint8_t     i;
    const char *name    = NODE_ADV_NAME;
    uint8_t     nameLen = (uint8_t)strlen(name);

    // 完整设备名（ESP32 网关按这个名字过滤并建立连接）
    scanRspData[n++] = (uint8_t)(nameLen + 1);
    scanRspData[n++] = GAP_ADTYPE_LOCAL_NAME_COMPLETE;
    for(i = 0; i < nameLen; i++)
    {
        scanRspData[n++] = (uint8_t)name[i];
    }

    // 连接间隔范围（告诉主机从机期望的连接间隔区间）
    scanRspData[n++] = 0x05;
    scanRspData[n++] = GAP_ADTYPE_SLAVE_CONN_INTERVAL_RANGE;
    scanRspData[n++] = LO_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL);
    scanRspData[n++] = HI_UINT16(DEFAULT_DESIRED_MIN_CONN_INTERVAL);
    scanRspData[n++] = LO_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL);
    scanRspData[n++] = HI_UINT16(DEFAULT_DESIRED_MAX_CONN_INTERVAL);

    // 发射功率等级
    scanRspData[n++] = 0x02;
    scanRspData[n++] = GAP_ADTYPE_POWER_LEVEL;
    scanRspData[n++] = 0;

    scanRspDataLen = n;
}

// GAP 广播包（ADV_IND）数据，最大 31 字节；
// 广播期间包越短越省电，放不下的内容放到上面的 scanRspData 里
static uint8_t advertData[] = {
    // Flags 字段：由 DEFAULT_DISCOVERABLE_MODE 决定采用通用可发现模式
    // （一直广播）还是受限可发现模式（30.72s 后停止），
    // 再与"不支持 BR/EDR"标志按位或
    0x02, // 本段长度 = 1（类型）+ 1（标志值）
    GAP_ADTYPE_FLAGS,
    DEFAULT_DISCOVERABLE_MODE | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,

    // 服务 UUID：提前告知主机本机提供哪些服务
    // （这里只是"预告"，真实服务以 gattprofile.c 的属性表为准）
    0x03,                  // 本段长度 = 1（类型）+ 2（UUID）
    GAP_ADTYPE_16BIT_MORE, // 0x02：只列出了部分 16 位 UUID
    LO_UINT16(SIMPLEPROFILE_SERV_UUID),
    HI_UINT16(SIMPLEPROFILE_SERV_UUID)
};

// GATT 层的设备名（与广播名、扫描响应名是三份互相独立的数据）
// 【本工程改动】与广播名保持一致，统一由 node_cfg.h 的 NODE_ADV_NAME 定义
static uint8_t attDeviceName[GAP_DEVICE_NAME_LEN] = NODE_ADV_NAME;

// 连接信息表（本例只维护 1 路连接，所以是单个变量而不是数组）
static peripheralConnItem_t peripheralConnList;

static uint8_t peripheralMTU = ATT_MTU_SIZE;
/*********************************************************************
 * 局部函数声明
 */
static void Peripheral_ProcessTMOSMsg(tmos_event_hdr_t *pMsg);
static void peripheralStateNotificationCB(gapRole_States_t newState, gapRoleEvent_t *pEvent);
static void performPeriodicTask(void);
static void simpleProfileChangeCB(uint8_t paramID, uint8_t *pValue, uint16_t len);
static void peripheralParamUpdateCB(uint16_t connHandle, uint16_t connInterval,
                                    uint16_t connSlaveLatency, uint16_t connTimeout);
static void peripheralInitConnItem(peripheralConnItem_t *peripheralConnList);
static void peripheralRssiCB(uint16_t connHandle, int8_t rssi);
static void peripheralChar4Notify(uint8_t *pValue, uint16_t len);

/*********************************************************************
 * Profile 回调
 */

// GAP 角色回调：状态变化 / RSSI / 连接参数更新
static gapRolesCBs_t Peripheral_PeripheralCBs = {
    peripheralStateNotificationCB, // 角色状态变化回调
    peripheralRssiCB,              // 读到有效 RSSI 时回调（结果异步返回）
    peripheralParamUpdateCB
};

// 广播回调（从机角色下不使用，保留结构）
static gapRolesBroadcasterCBs_t Broadcaster_BroadcasterCBs = {
    NULL, // 从机角色下未使用
    NULL  // 收到扫描请求（SCAN_REQ）的回调
};

// 配对绑定管理回调
static gapBondCBs_t Peripheral_BondMgrCBs = {
    NULL, // 配对码回调（本应用未使用）
    NULL  // 配对 / 绑定状态回调（本应用未使用）
};

// 自定义服务（SimpleProfile）回调
static simpleProfileCBs_t Peripheral_SimpleProfileCBs = {
    simpleProfileChangeCB // 特征值被主机改写时的回调
};
/*********************************************************************
 * 公共函数
 */

/*********************************************************************
 * @fn      Peripheral_Init
 *
 * @brief   外设从机应用任务的初始化函数。
 *          在系统初始化阶段被调用，用于完成本应用相关的初始化
 *          （硬件初始化、属性表初始化、上电通知等）。
 *
 * @param   task_id - TMOS 分配的任务 ID，用于发送消息和启动定时器
 *
 * @return  无
 */
void Peripheral_Init()
{
    Peripheral_TaskID = TMOS_ProcessEventRegister(Peripheral_ProcessEvent);

    // 配置 GAP 从机角色参数
    {
        uint8_t  initial_advertising_enable = TRUE;//false
        uint16_t desired_min_interval = DEFAULT_DESIRED_MIN_CONN_INTERVAL;
        uint16_t desired_max_interval = DEFAULT_DESIRED_MAX_CONN_INTERVAL;

        // 设置角色参数：广播使能、扫描响应数据、广播数据、连接间隔范围
        GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &initial_advertising_enable);//启用广播
        Peripheral_BuildScanRsp();
        GAPRole_SetParameter(GAPROLE_SCAN_RSP_DATA, scanRspDataLen, scanRspData);//扫描响应数据
        GAPRole_SetParameter(GAPROLE_ADVERT_DATA, sizeof(advertData), advertData);//广播数据
        GAPRole_SetParameter(GAPROLE_MIN_CONN_INTERVAL, sizeof(uint16_t), &desired_min_interval);//允许最小连接间隔
        GAPRole_SetParameter(GAPROLE_MAX_CONN_INTERVAL, sizeof(uint16_t), &desired_max_interval);//允许最大连接间隔
    }

    // 设置 GATT 设备名（主机连上后在 GAP 服务里看到的名字）
    GGS_SetParameter(GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN, attDeviceName);//attDeviceName位于136行

    // 设置广播间隔（最小与最大取同一值，即固定间隔）
    {
        uint16_t advInt = DEFAULT_ADVERTISING_INTERVAL;

        GAP_SetParamValue(TGAP_DISC_ADV_INT_MIN, advInt);
        GAP_SetParamValue(TGAP_DISC_ADV_INT_MAX, advInt);
    }

    // 配置配对绑定管理器
    {
        uint32_t passkey = 0; // 配对码 "000000"
        uint8_t  pairMode = GAPBOND_PAIRING_MODE_WAIT_FOR_REQ;
        uint8_t  mitm = TRUE;
        uint8_t  bonding = TRUE;
        uint8_t  ioCap = GAPBOND_IO_CAP_DISPLAY_ONLY;
        GAPBondMgr_SetParameter(GAPBOND_PERI_DEFAULT_PASSCODE, sizeof(uint32_t), &passkey);
        GAPBondMgr_SetParameter(GAPBOND_PERI_PAIRING_MODE, sizeof(uint8_t), &pairMode);
        GAPBondMgr_SetParameter(GAPBOND_PERI_MITM_PROTECTION, sizeof(uint8_t), &mitm);
        GAPBondMgr_SetParameter(GAPBOND_PERI_IO_CAPABILITIES, sizeof(uint8_t), &ioCap);
        GAPBondMgr_SetParameter(GAPBOND_PERI_BONDING_ENABLED, sizeof(uint8_t), &bonding);
    }

    // 注册服务（顺序不能颠倒：先标准服务，最后自定义服务）
    GGS_AddService(GATT_ALL_SERVICES);           // GAP 服务
    GATTServApp_AddService(GATT_ALL_SERVICES);   // GATT 属性服务
    DevInfo_AddService();                        // DIS设备信息服务
    SimpleProfile_AddService(GATT_ALL_SERVICES); // 自定义服务（0xFFE0）

    // 设置自定义服务各特征的初值
    {
        uint8_t charValue1[SIMPLEPROFILE_CHAR1_LEN] = {1};
        uint8_t charValue2[SIMPLEPROFILE_CHAR2_LEN] = {2};
        uint8_t charValue3[SIMPLEPROFILE_CHAR3_LEN] = {3};
        uint8_t charValue4[SIMPLEPROFILE_CHAR4_LEN] = {4};
        uint8_t charValue5[SIMPLEPROFILE_CHAR5_LEN] = {1, 2, 3, 4, 5};

        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR1, SIMPLEPROFILE_CHAR1_LEN, charValue1);
        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR2, SIMPLEPROFILE_CHAR2_LEN, charValue2);
        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR3, SIMPLEPROFILE_CHAR3_LEN, charValue3);
        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR4, SIMPLEPROFILE_CHAR4_LEN, charValue4);
        SimpleProfile_SetParameter(SIMPLEPROFILE_CHAR5, SIMPLEPROFILE_CHAR5_LEN, charValue5);
    }

    // 初始化连接信息表
    peripheralInitConnItem(&peripheralConnList);

    // 向自定义服务注册应用层回调
    SimpleProfile_RegisterAppCBs(&Peripheral_SimpleProfileCBs);

    // 注册扫描请求回调（从机角色下未使用）
    GAPRole_BroadcasterSetCB(&Broadcaster_BroadcasterCBs);

    // 抛出启动事件（真正的启动在事件处理函数里完成）
    tmos_set_event(Peripheral_TaskID, SBP_START_DEVICE_EVT);
}

/*********************************************************************
 * @fn      peripheralInitConnItem
 *
 * @brief   初始化连接信息表
 *
 * @param   peripheralConnList - 待初始化的连接信息表
 *
 * @return  无
 */
static void peripheralInitConnItem(peripheralConnItem_t *peripheralConnList)
{
    peripheralConnList->connHandle = GAP_CONNHANDLE_INIT;
    peripheralConnList->connInterval = 0;
    peripheralConnList->connSlaveLatency = 0;
    peripheralConnList->connTimeout = 0;
}

/*********************************************************************
 * @fn      Peripheral_ProcessEvent
 *
 * @brief   外设从机应用任务的事件处理函数，负责处理本任务的全部事件，
 *          包括定时器事件、TMOS 消息以及用户自定义事件。
 *
 * @param   task_id - TMOS 分配的任务 ID
 * @param   events  - 待处理的事件位图，可能同时包含多个事件
 *
 * @return  未处理的事件位（返回 0 表示全部处理完）
 */
uint16_t Peripheral_ProcessEvent(uint8_t task_id, uint16_t events)
{
    //  VOID task_id; // TMOS 规定的形参，本函数未使用

    if(events & SYS_EVENT_MSG)
    {
        uint8_t *pMsg;

        if((pMsg = tmos_msg_receive(Peripheral_TaskID)) != NULL)
        {
            Peripheral_ProcessTMOSMsg((tmos_event_hdr_t *)pMsg);
            // 释放 TMOS 消息
            tmos_msg_deallocate(pMsg);
        }
        // 返回未处理的事件（异或掉已处理的位）
        return (events ^ SYS_EVENT_MSG);
    }

    if(events & SBP_START_DEVICE_EVT)
    {
        // 启动设备：开始广播
        GAPRole_PeripheralStartDevice(Peripheral_TaskID, &Peripheral_BondMgrCBs, &Peripheral_PeripheralCBs);
        return (events ^ SBP_START_DEVICE_EVT);
    }

    if(events & SBP_PERIODIC_EVT)
    {
        // 重启周期定时器
        if(SBP_PERIODIC_EVT_PERIOD)
        {
            tmos_start_task(Peripheral_TaskID, SBP_PERIODIC_EVT, SBP_PERIODIC_EVT_PERIOD);
        }
        // 执行周期性任务（通过 CHAR4 发通知）
        performPeriodicTask();
        return (events ^ SBP_PERIODIC_EVT);
    }

    if(events & SBP_PARAM_UPDATE_EVT)
    {
        // 发起连接参数更新请求
        GAPRole_PeripheralConnParamUpdateReq(peripheralConnList.connHandle,
                                             DEFAULT_DESIRED_MIN_CONN_INTERVAL,
                                             DEFAULT_DESIRED_MAX_CONN_INTERVAL,
                                             DEFAULT_DESIRED_SLAVE_LATENCY,
                                             DEFAULT_DESIRED_CONN_TIMEOUT,
                                             Peripheral_TaskID);

        return (events ^ SBP_PARAM_UPDATE_EVT);
    }

    if(events & SBP_READ_RSSI_EVT)
    {
        GAPRole_ReadRssiCmd(peripheralConnList.connHandle);
        tmos_start_task(Peripheral_TaskID, SBP_READ_RSSI_EVT, SBP_READ_RSSI_EVT_PERIOD);
        return (events ^ SBP_READ_RSSI_EVT);
    }

    // 丢弃未知事件
    return 0;
}

/*********************************************************************
 * @fn      Peripheral_ProcessTMOSMsg
 *
 * @brief   处理收到的任务消息。
 *
 * @param   pMsg - 待处理的消息
 *
 * @return  无
 */
static void Peripheral_ProcessTMOSMsg(tmos_event_hdr_t *pMsg)
{
    switch(pMsg->event)
    {
        case GAP_MSG_EVENT:
        {
            break;
        }

        case GATT_MSG_EVENT:
        {
            gattMsgEvent_t *pMsgEvent;

            pMsgEvent = (gattMsgEvent_t *)pMsg;
            if(pMsgEvent->method == ATT_MTU_UPDATED_EVENT)
            {
                peripheralMTU = pMsgEvent->msg.exchangeMTUReq.clientRxMTU;
                PRINT("mtu exchange: %d\n", pMsgEvent->msg.exchangeMTUReq.clientRxMTU);
            }
            break;
        }

        default:
            break;
    }
}

/*********************************************************************
 * @fn      Peripheral_LinkEstablished
 *
 * @brief   处理链路建立（连接成功）事件。
 *
 * @param   pEvent - 待处理的事件
 *
 * @return  无
 */
static void Peripheral_LinkEstablished(gapRoleEvent_t *pEvent)
{
    gapEstLinkReqEvent_t *event = (gapEstLinkReqEvent_t *)pEvent;

    // 判断是否已有连接（本例只支持 1 路，第 2 路会被直接断开）
    if(peripheralConnList.connHandle != GAP_CONNHANDLE_INIT)
    {
        GAPRole_TerminateLink(pEvent->linkCmpl.connectionHandle);
        PRINT("Connection max...\n");
    }
    else
    {
        peripheralConnList.connHandle = event->connectionHandle;
        peripheralConnList.connInterval = event->connInterval;
        peripheralConnList.connSlaveLatency = event->connLatency;
        peripheralConnList.connTimeout = event->connTimeout;

        // 【本工程改动】不再启动周期通知定时器（SBP_PERIODIC_EVT_PERIOD = 0），
        // 通知改由节点周期任务触发，见 smoke_node.c

        // 启动参数更新定时器（4s 后发起更新请求）
        tmos_start_task(Peripheral_TaskID, SBP_PARAM_UPDATE_EVT, SBP_PARAM_UPDATE_DELAY);

        // 启动 RSSI 读取定时器（2s 一次）
        tmos_start_task(Peripheral_TaskID, SBP_READ_RSSI_EVT, SBP_READ_RSSI_EVT_PERIOD);

        PRINT("Conn %x - Int %x \n", event->connectionHandle, event->connInterval);

        /* 通知应用层：链路已建立，此后可以上报数据 */
        SmokeNode_OnLinkUp(event->connectionHandle, event->connInterval);
    }
}

/*********************************************************************
 * @fn      Peripheral_LinkTerminated
 *
 * @brief   处理链路断开事件。
 *
 * @param   pEvent - 待处理的事件
 *
 * @return  无
 */
static void Peripheral_LinkTerminated(gapRoleEvent_t *pEvent)
{
    gapTerminateLinkEvent_t *event = (gapTerminateLinkEvent_t *)pEvent;

    if(event->connectionHandle == peripheralConnList.connHandle)
    {
        peripheralConnList.connHandle = GAP_CONNHANDLE_INIT;
        peripheralConnList.connInterval = 0;
        peripheralConnList.connSlaveLatency = 0;
        peripheralConnList.connTimeout = 0;
        tmos_stop_task(Peripheral_TaskID, SBP_PERIODIC_EVT);
        tmos_stop_task(Peripheral_TaskID, SBP_READ_RSSI_EVT);

        /* 通知应用层：链路已断开，暂停上报 */
        SmokeNode_OnLinkDown();

        // 重新打开广播（否则设备会停在 GAPROLE_WAITING，手机再也搜不到）
        {
            uint8_t advertising_enable = TRUE;
            GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED, sizeof(uint8_t), &advertising_enable);
        }
    }
    else
    {
        PRINT("ERR..\n");
    }
}

/*********************************************************************
 * @fn      peripheralRssiCB
 *
 * @brief   RSSI 回调。
 *
 * @param   connHandle - 连接句柄
 * @param   rssi - 信号强度（dBm，负值，越接近 0 越强）
 *
 * @return  无
 */
static void peripheralRssiCB(uint16_t connHandle, int8_t rssi)
{
    PRINT("RSSI -%d dB Conn  %x \n", -rssi, connHandle);
}

/*********************************************************************
 * @fn      peripheralParamUpdateCB
 *
 * @brief   连接参数更新完成回调
 *
 * @param   connHandle - 连接句柄
 *          connInterval - 更新后的连接间隔（单位 1.25ms）
 *          connSlaveLatency - 更新后的从机延迟
 *          connTimeout - 更新后的监督超时
 *
 * @return  无
 */
static void peripheralParamUpdateCB(uint16_t connHandle, uint16_t connInterval,
                                    uint16_t connSlaveLatency, uint16_t connTimeout)
{
    if(connHandle == peripheralConnList.connHandle)
    {
        peripheralConnList.connInterval = connInterval;
        peripheralConnList.connSlaveLatency = connSlaveLatency;
        peripheralConnList.connTimeout = connTimeout;

        PRINT("Update %x - Int %x \n", connHandle, connInterval);
    }
    else
    {
        PRINT("ERR..\n");
    }
}

/*********************************************************************
 * @fn      peripheralStateNotificationCB
 *
 * @brief   角色状态变化通知回调，从机协议栈状态机的唯一出口。
 *
 * @param   newState - 新状态
 *
 * @return  无
 */
static void peripheralStateNotificationCB(gapRole_States_t newState, gapRoleEvent_t *pEvent)
{
    switch(newState)
    {
        case GAPROLE_STARTED:
            PRINT("Initialized..\n");
            break;

        case GAPROLE_ADVERTISING:
            if(pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)
            {
                Peripheral_LinkTerminated(pEvent);
                PRINT("Disconnected.. Reason:%x\n", pEvent->linkTerminate.reason);
                PRINT("Advertising..\n");
            }
            else if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Advertising..\n");
            }
            break;

        case GAPROLE_CONNECTED:
            if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                Peripheral_LinkEstablished(pEvent);
                PRINT("Connected..\n");
            }
            break;

        case GAPROLE_CONNECTED_ADV:
            if(pEvent->gap.opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Connected Advertising..\n");
            }
            break;

        case GAPROLE_WAITING:
            if(pEvent->gap.opcode == GAP_END_DISCOVERABLE_DONE_EVENT)
            {
                PRINT("Waiting for advertising..\n");
            }
            else if(pEvent->gap.opcode == GAP_LINK_TERMINATED_EVENT)
            {
                Peripheral_LinkTerminated(pEvent);
                PRINT("Disconnected.. Reason:%x\n", pEvent->linkTerminate.reason);
            }
            else if(pEvent->gap.opcode == GAP_LINK_ESTABLISHED_EVENT)
            {
                if(pEvent->gap.hdr.status != SUCCESS)
                {
                    PRINT("Waiting for advertising..\n");
                }
                else
                {
                    PRINT("Error..\n");
                }
            }
            else
            {
                PRINT("Error..%x\n", pEvent->gap.opcode);
            }
            break;

        case GAPROLE_ERROR:
            PRINT("Error..\n");
            break;

        default:
            break;
    }
}

/*********************************************************************
 * @fn      performPeriodicTask
 *
 * @brief   周期性应用任务，由 SBP_PERIODIC_EVT 事件触发（每 1 秒一次）。
 *          【注】原注释描述的"把第 3 个特征的值复制到第 4 个特征"是 TI 原版
 *          例程的做法；WCH 这个例程里只做一件事：通过 CHAR4 发送 0x88 通知。
 *
 * @param   无
 *
 * @return  无
 */
static void performPeriodicTask(void)
{
    /* 【本工程改动】原来这里每秒通过 CHAR4 发送一次 0x88 示例通知，
     * 会被 ESP32 网关误当成有效帧，因此删除。
     * 现在 CHAR4 只在节点周期任务里发送（见 smoke_node.c 的 SmokeNode_Report）。 */
}

/*********************************************************************
 * @fn      peripheralChar4Notify
 *
 * @brief   打包并发送 CHAR4 通知
 *
 * @param   pValue - 要上报的数据
 *          len - 数据长度
 *
 * @return  无
 */
static void peripheralChar4Notify(uint8_t *pValue, uint16_t len)
{
    attHandleValueNoti_t noti;
    if(len > (peripheralMTU - 3))
    {
        PRINT("Too large noti\n");
        return;
    }
    noti.len = len;
    noti.pValue = GATT_bm_alloc(peripheralConnList.connHandle, ATT_HANDLE_VALUE_NOTI, noti.len, NULL, 0);
    if(noti.pValue)
    {
        tmos_memcpy(noti.pValue, pValue, noti.len);
        if(simpleProfile_Notify(peripheralConnList.connHandle, &noti) != SUCCESS)
        {
            GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);
        }
    }
}

/*********************************************************************
 * @fn      simpleProfileChangeCB
 *
 * @brief   自定义服务的应用层回调，主机改写特征值时触发
 *
 * @param   paramID - 发生变化的特征编号
 *          pValue - 指向新数据的指针
 *          len - 数据长度
 *
 * @return  无
 */
static void simpleProfileChangeCB(uint8_t paramID, uint8_t *pValue, uint16_t len)
{
    switch(paramID)
    {
        case SIMPLEPROFILE_CHAR1:
        {
            uint8_t newValue[SIMPLEPROFILE_CHAR1_LEN];
            tmos_memcpy(newValue, pValue, len);
            PRINT("profile ChangeCB CHAR1.. \n");
            break;
        }

        case SIMPLEPROFILE_CHAR3:
        {
            /* 特征长度固定 1 字节, 主机写多了会溢出, 这里按特征长度截断 */
            uint8_t  newValue[SIMPLEPROFILE_CHAR3_LEN] = {0};
            uint16_t copyLen = (len > SIMPLEPROFILE_CHAR3_LEN) ? SIMPLEPROFILE_CHAR3_LEN : len;
            tmos_memcpy(newValue, pValue, copyLen);
            PRINT("profile ChangeCB CHAR3..\n");
            /* 主机 -> 从机 的数据：交给应用层解析命令（消音/自检/复位） */
            SmokeNode_OnHostWrite(newValue, copyLen);
            break;
        }

        default:
            // 正常不会走到这里（只有 CHAR1 / CHAR3 允许写）
            break;
    }
}

/*********************************************************************
*********************************************************************/
