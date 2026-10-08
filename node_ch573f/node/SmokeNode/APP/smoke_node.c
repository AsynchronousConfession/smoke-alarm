/********************************** (C) COPYRIGHT *******************************
 * File Name          : smoke_node.c
 * Description        : 烟雾报警节点应用层（采样 + 上报 + 命令 + 蜂鸣器）
 *
 *   节点硬件：CH573F 最小系统板 + MQ 传感器 + 蜂鸣器（无 OLED、无按键）
 *
 *   数据通路:
 *     Sensor_Poll()   -> smoke_sensor.c 算 Rs / Δ% / 报警
 *     SmokeNode_Tick()-> SmokeNode_Report() -> CHAR4(0xFFE4) 通知 -> ESP32 网关
 *     网关写 CHAR3(0xFFE3) -> peripheral.c -> SmokeNode_OnHostWrite() -> 执行命令
 *
 *   本地能观察到的东西只有两样：
 *     ① 蜂鸣器：预热结束"嘀"一声 / 报警断续响 / 自检长鸣；
 *     ② 串口日志（PB7=TX，115200）：每秒一行数据，报警/解除时另外打一行。
 *******************************************************************************/

/*********************************************************************
 * 头文件包含
 */
#include "CONFIG.h"
#include "smoke_node.h"
#include "smoke_sensor.h"
#include "node_proto.h"
#include "node_cfg.h"
#include "gattprofile.h"

/*********************************************************************
 * 宏定义
 */
/* 本任务在 TMOS 中的事件位（与 peripheral.c 的事件位互不冲突） */
#define SMOKE_NODE_EVT          0x0001

/* 信息帧重发周期：网关重启后会重新订阅，这里定期补发，保证网页能看到型号 */
#define NODE_INFO_RESEND_MS     30000

/*********************************************************************
 * 局部变量
 */
static uint8_t  nodeTaskID      = INVALID_TASK_ID;
static uint16_t connHandle      = GAP_CONNHANDLE_INIT;
static uint32_t uptimeMs        = 0;        /* 节点自己的软时钟 */
static uint32_t sampleAcc       = 0;        /* 采样分频累加 */
static uint32_t lastReportMs    = 0;
static uint32_t lastInfoMs      = 0;
static uint32_t lastPrintMs     = 0;
static uint32_t txCount         = 0;        /* 成功发出的通知帧数 */
static uint8_t  seq             = 0;        /* 遥测帧序号 */
static uint8_t  muted           = 0;
static uint32_t muteUntilMs     = 0;
static uint32_t selftestUntilMs = 0;
static uint8_t  prevWarming     = 1;        /* 上一拍的预热状态，用来抓"预热结束" */
static uint8_t  prevAlarm       = 0;        /* 上一拍的报警状态，用来打印报警/解除事件行 */

/*********************************************************************
 * 局部函数声明
 */
static uint16_t SmokeNode_ProcessEvent(uint8_t task_id, uint16_t events);
static void     SmokeNode_Tick(void);
static void     SmokeNode_Report(void);
static void     SmokeNode_Buzzer(void);
static uint8_t  SmokeNode_SendFrame(const uint8_t *buf, uint16_t len);
static void     SmokeNode_SendInfo(void);
static void     SmokeNode_PutU16(uint8_t *p, uint16_t v);
static void     SmokeNode_PutU32(uint8_t *p, uint32_t v);
static void     SmokeNode_Mute(uint32_t ms);

/*********************************************************************
 * @fn      SmokeNode_PutU16 / SmokeNode_PutU32
 *
 * @brief   小端写入整数（帧格式见 node_proto.h）
 *
 * @return  无
 */
static void SmokeNode_PutU16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void SmokeNode_PutU32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/*********************************************************************
 * @fn      SmokeNode_SendFrame
 *
 * @brief   通过 CHAR4(0xFFE4) 发一帧通知给网关
 *          （只有网关已经打开 CCCD 时才会真正发出去）
 *
 * @param   buf - 待发送数据
 *          len - 数据长度（最大 ATT_MTU-3 = 20）
 *
 * @return  1 = 已发出, 0 = 没发出去（未连接 / CCCD 未开 / 内存不足）
 */
static uint8_t SmokeNode_SendFrame(const uint8_t *buf, uint16_t len)
{
    attHandleValueNoti_t noti;
    bStatus_t            status;

    if(connHandle == GAP_CONNHANDLE_INIT || len == 0)
    {
        return 0;
    }
    if(len > (ATT_MTU_SIZE - 3))
    {
        PRINT("[NODE] frame too long (%d)\n", (int)len);
        return 0;
    }

    noti.len    = len;
    noti.pValue = GATT_bm_alloc(connHandle, ATT_HANDLE_VALUE_NOTI, noti.len, NULL, 0);
    if(noti.pValue == NULL)
    {
        return 0;
    }

    tmos_memcpy(noti.pValue, buf, len);

    status = simpleProfile_Notify(connHandle, &noti);
    if(status != SUCCESS)
    {
        GATT_bm_free((gattMsg_t *)&noti, ATT_HANDLE_VALUE_NOTI);
        return 0;
    }
    return 1;
}

/*********************************************************************
 * @fn      SmokeNode_SendInfo
 *
 * @brief   发送信息帧：节点编号 | 传感器型号 | 固件版本
 *          （网页上显示"这是哪个房间的什么传感器"）
 *
 * @return  无
 */
static void SmokeNode_SendInfo(void)
{
    uint8_t frame[NODE_INFO_LEN];
    char    text[NODE_INFO_TEXT_LEN + 1];
    uint8_t i;

    tmos_memset(frame, 0, sizeof(frame));
    frame[0] = NODE_FRAME_MAGIC;
    frame[1] = NODE_FRAME_INFO;

    sprintf(text, "%s|%s|%s", NODE_TAG, NODE_MODEL, NODE_FW_VER);
    for(i = 0; i < NODE_INFO_TEXT_LEN && text[i] != '\0'; i++)
    {
        frame[2 + i] = (uint8_t)text[i];
    }

    if(SmokeNode_SendFrame(frame, sizeof(frame)))
    {
        PRINT("[NODE] info sent: %s\n", text);
        lastInfoMs = uptimeMs;
    }
}

/*********************************************************************
 * @fn      SmokeNode_Report
 *
 * @brief   把最近一次采样打包成 16 字节遥测帧并发出
 *
 * @return  无
 */
static void SmokeNode_Report(void)
{
    const sensor_sample_t *s = Sensor_Get();
    uint8_t  frame[NODE_TELEMETRY_LEN];
    uint8_t  flags = 0;
    uint16_t sum   = 0;
    uint8_t  i;

    if(s->alarm)    flags |= NODE_FLAG_ALARM;
    if(s->warming)  flags |= NODE_FLAG_WARMUP;
    if(s->do_level) flags |= NODE_FLAG_DO;
    if(muted)       flags |= NODE_FLAG_MUTED;

    tmos_memset(frame, 0, sizeof(frame));
    frame[TELE_OFF_MAGIC] = NODE_FRAME_MAGIC;
    frame[TELE_OFF_TYPE]  = NODE_FRAME_TELEMETRY;
    frame[TELE_OFF_SEQ]   = seq;
    frame[TELE_OFF_FLAGS] = flags;
    SmokeNode_PutU16(&frame[TELE_OFF_ADC], s->adc);
    SmokeNode_PutU16(&frame[TELE_OFF_AO_MV], s->ao_mv);
    SmokeNode_PutU32(&frame[TELE_OFF_RS], s->rs_ohm);
    SmokeNode_PutU16(&frame[TELE_OFF_DPCT], (uint16_t)s->dpct_x10);
    frame[TELE_OFF_DO] = s->do_level;

    for(i = 0; i < TELE_OFF_SUM; i++)
    {
        sum += frame[i];
    }
    frame[TELE_OFF_SUM] = (uint8_t)(sum & 0xFF);

    if(SmokeNode_IsConnected())
    {
        if(SmokeNode_SendFrame(frame, sizeof(frame)))
        {
            seq++;
            txCount++;
            lastReportMs = uptimeMs;
        }
    }
}

/*********************************************************************
 * @fn      SmokeNode_Buzzer
 *
 * @brief   蜂鸣器控制：自检 -> 长鸣；报警且未消音 -> 断续响；其余 -> 停
 *          （这是节点唯一的本地报警输出）
 *
 * @return  无
 */
static void SmokeNode_Buzzer(void)
{
#if (NODE_HAS_BUZZER)
    const sensor_sample_t *s = Sensor_Get();
    uint8_t on = 0;

    if(uptimeMs < selftestUntilMs)
    {
        on = 1;                                     /* 自检：连续长鸣 */
    }
    else if(s->alarm && !muted)
    {
        uint32_t phase = uptimeMs % (NODE_BEEP_ON_MS + NODE_BEEP_OFF_MS);
        on = (phase < NODE_BEEP_ON_MS) ? 1 : 0;     /* 报警：断续响 */
    }

#if (NODE_BUZZER_ACTIVE_HIGH)
    if(on)
    {
        GPIOB_SetBits(NODE_BUZZER_PIN);
    }
    else
    {
        GPIOB_ResetBits(NODE_BUZZER_PIN);
    }
#else
    if(on)
    {
        GPIOB_ResetBits(NODE_BUZZER_PIN);
    }
    else
    {
        GPIOB_SetBits(NODE_BUZZER_PIN);
    }
#endif
#endif /* NODE_HAS_BUZZER */
}

/*********************************************************************
 * @fn      SmokeNode_Mute
 *
 * @brief   消音一段时间（不影响报警判定和上报，只是本机蜂鸣器不响）
 *
 * @param   ms - 消音时长(ms)
 *
 * @return  无
 */
static void SmokeNode_Mute(uint32_t ms)
{
    muted       = 1;
    muteUntilMs = uptimeMs + ms;
    PRINT("[NODE] mute for %d ms\n", (int)ms);
}

/*********************************************************************
 * @fn      SmokeNode_Tick
 *
 * @brief   节点周期任务（默认每 100ms 一次）
 *
 * @return  无
 */
static void SmokeNode_Tick(void)
{
    const sensor_sample_t *s;
    uint32_t reportPeriod;

    uptimeMs += NODE_TICK_MS;

    /* 1. 采样：每 NODE_SAMPLE_PERIOD_MS 一次 */
    sampleAcc += NODE_TICK_MS;
    if(sampleAcc >= NODE_SAMPLE_PERIOD_MS)
    {
        sampleAcc = 0;
        Sensor_Poll();
    }

    s = Sensor_Get();

    /* 2. 消音到期 */
    if(muted && (int32_t)(uptimeMs - muteUntilMs) >= 0)
    {
        muted = 0;
        PRINT("[NODE] mute expired\n");
    }

    /* 3. 预热结束的那一刻短鸣一声：告诉现场的人"现在开始才是有效检测" */
    if(prevWarming && !s->warming)
    {
        PRINT("[NODE] warm-up done, baseline ready\n");
#if (NODE_READY_BEEP_MS) > 0
        selftestUntilMs = uptimeMs + NODE_READY_BEEP_MS;
#endif
    }
    prevWarming = s->warming;

    /* 4. 蜂鸣器 */
    SmokeNode_Buzzer();

    /* 5. 周期上报：报警时更快 */
    reportPeriod = s->alarm ? NODE_REPORT_ALARM_MS : NODE_REPORT_NORMAL_MS;
    if((uint32_t)(uptimeMs - lastReportMs) >= reportPeriod)
    {
        SmokeNode_Report();
    }

    /* 6. 报警状态跳变时单独打一行，事后翻串口日志一眼就能找到 */
    if(s->alarm && !prevAlarm)
    {
        PRINT(">>> [NODE %s] 报警! D%%=%s%d.%d%%  ADC=%d  Rs=%d\n",
              NODE_TAG,
              (s->dpct_x10 < 0) ? "-" : "+",
              (int)((s->dpct_x10 < 0 ? -s->dpct_x10 : s->dpct_x10) / 10),
              (int)((s->dpct_x10 < 0 ? -s->dpct_x10 : s->dpct_x10) % 10),
              (int)s->adc, (int)s->rs_ohm);
    }
    else if(!s->alarm && prevAlarm)
    {
        PRINT("<<< [NODE %s] 报警解除, 当前 D%%=%s%d.%d%%\n", NODE_TAG,
              (s->dpct_x10 < 0) ? "-" : "+",
              (int)((s->dpct_x10 < 0 ? -s->dpct_x10 : s->dpct_x10) / 10),
              (int)((s->dpct_x10 < 0 ? -s->dpct_x10 : s->dpct_x10) % 10));
    }
    prevAlarm = s->alarm;

    /* 7. 信息帧：连上后先发一次，之后每 NODE_INFO_RESEND_MS 补发一次 */
    if(SmokeNode_IsConnected())
    {
        if(lastInfoMs == 0 || (uint32_t)(uptimeMs - lastInfoMs) >= NODE_INFO_RESEND_MS)
        {
            SmokeNode_SendInfo();
        }
    }

    /* 8. 串口日志：每秒一行，方便不接网关时单独验证传感器 */
    if((uint32_t)(uptimeMs - lastPrintMs) >= 1000)
    {
        int32_t d10 = s->dpct_x10;

        lastPrintMs = uptimeMs;
        PRINT("[NODE %s] t=%us ADC=%d AO=%dmV Rs=%d D=%s%d.%d%% DO=%d ALM=%d %s%s TX=%d\n",
              NODE_TAG, (unsigned)(uptimeMs / 1000),
              (int)s->adc, (int)s->ao_mv, (int)s->rs_ohm,
              (d10 < 0) ? "-" : "+",
              (int)((d10 < 0 ? -d10 : d10) / 10),
              (int)((d10 < 0 ? -d10 : d10) % 10),
              (int)s->do_level, (int)s->alarm,
              s->warming ? "WARM" : "    ",
              muted ? " MUTE" : "     ",
              (int)txCount);
    }
}

/*********************************************************************
 * @fn      SmokeNode_ProcessEvent
 *
 * @brief   TMOS 事件处理（与 peripheral.c 的写法一致）
 *
 * @param   task_id - 任务 ID
 *          events  - 事件位
 *
 * @return  未处理的事件位
 */
static uint16_t SmokeNode_ProcessEvent(uint8_t task_id, uint16_t events)
{
    if(events & SMOKE_NODE_EVT)
    {
        SmokeNode_Tick();
        tmos_start_task(task_id, SMOKE_NODE_EVT, MS1_TO_SYSTEM_TIME(NODE_TICK_MS));
        return (events ^ SMOKE_NODE_EVT);
    }
    return 0;
}

/*********************************************************************
 * @fn      SmokeNode_Init
 *
 * @brief   初始化传感器与蜂鸣器，注册并启动周期任务
 *
 * @return  无
 */
void SmokeNode_Init(void)
{
    PRINT("[NODE] ==========================================\n");
    PRINT("[NODE] 节点编号 : %s\n", NODE_TAG);
    PRINT("[NODE] 广播名   : %s\n", NODE_ADV_NAME);
    PRINT("[NODE] 传感器   : %s (AO=%s, DO=%s)\n", NODE_MODEL,
          NODE_AO_PIN_NAME, NODE_HAS_DO ? NODE_DO_PIN_NAME : "未使用");
    PRINT("[NODE] 模块供电 : %dmV, 分压比 /%d.%02d, RL=%d ohm\n",
          NODE_MODULE_VCC_MV, NODE_AO_DIV_X100 / 100, NODE_AO_DIV_X100 % 100,
          NODE_RL_OHM);
    PRINT("[NODE] 报警阈值 : |D%%| >= %d%%, 连续 %d 次; 解除 <= %d%%\n",
          NODE_ALARM_PCT, NODE_ALARM_HOLD, NODE_CLEAR_PCT);
    PRINT("[NODE] 预热时间 : %d 秒（结束时蜂鸣器短鸣一声）\n", NODE_WARMUP_MS / 1000);
    PRINT("[NODE] 上报周期 : 正常 %dms / 报警 %dms\n",
          NODE_REPORT_NORMAL_MS, NODE_REPORT_ALARM_MS);
    PRINT("[NODE] 本机外设 : 蜂鸣器 %s（无 OLED / 无按键）\n",
          NODE_HAS_BUZZER ? "已启用" : "未使用");
    PRINT("[NODE] ==========================================\n");

    /* 传感器 */
    Sensor_Init();

    /* 蜂鸣器（先把电平设成"不响"再配成推挽输出，避免上电瞬间响一下） */
#if (NODE_HAS_BUZZER)
#if (NODE_BUZZER_ACTIVE_HIGH)
    GPIOB_ResetBits(NODE_BUZZER_PIN);
#else
    GPIOB_SetBits(NODE_BUZZER_PIN);
#endif
    GPIOB_ModeCfg(NODE_BUZZER_PIN, GPIO_ModeOut_PP_5mA);
#endif

    /* 周期任务 */
    nodeTaskID = TMOS_ProcessEventRegister(SmokeNode_ProcessEvent);
    tmos_start_task(nodeTaskID, SMOKE_NODE_EVT, MS1_TO_SYSTEM_TIME(NODE_TICK_MS));
}

/*********************************************************************
 * @fn      SmokeNode_OnLinkUp
 *
 * @brief   BLE 链路建立：记录连接句柄，允许上报
 *
 * @param   handle   - 连接句柄
 *          interval - 连接间隔（单位 1.25ms）
 *
 * @return  无
 */
void SmokeNode_OnLinkUp(uint16_t handle, uint16_t interval)
{
    connHandle   = handle;
    lastInfoMs   = 0;           /* 新连接：信息帧重新发一次 */
    lastReportMs = uptimeMs;    /* 避免刚连上就补一条旧数据 */
    PRINT("[NODE] link up: handle=0x%04X interval=0x%04X\n", handle, interval);
}

/*********************************************************************
 * @fn      SmokeNode_OnLinkDown
 *
 * @brief   BLE 链路断开：清连接句柄，停止上报（本地报警和蜂鸣器不受影响）
 *
 * @return  无
 */
void SmokeNode_OnLinkDown(void)
{
    connHandle = GAP_CONNHANDLE_INIT;
    PRINT("[NODE] link down: 本地报警继续, 等待网关重连\n");
}

/*********************************************************************
 * @fn      SmokeNode_IsConnected
 *
 * @brief   当前是否已连接网关
 *
 * @return  1 = 已连接
 */
uint8_t SmokeNode_IsConnected(void)
{
    return (connHandle != GAP_CONNHANDLE_INIT) ? 1 : 0;
}

/*********************************************************************
 * @fn      SmokeNode_OnHostWrite
 *
 * @brief   网关写入 CHAR3(0xFFE3) 的数据
 *          "M"/"Mnn" 消音(默认 60s / 指定秒数) / "T" 自检 / "R" 复位 /
 *          其它内容只在串口日志里打印（本机没有屏，不做显示）
 *
 * @param   pValue - 数据
 *          len    - 长度（最大 20）
 *
 * @return  无
 */
void SmokeNode_OnHostWrite(uint8_t *pValue, uint16_t len)
{
    char     text[24];
    uint16_t n;
    uint8_t  cmd;

    if(len == 0)
    {
        return;
    }
    n = (len < sizeof(text) - 1) ? len : (uint16_t)sizeof(text) - 1;
    tmos_memcpy(text, pValue, n);
    text[n] = '\0';

    PRINT("[NODE] host write: \"%s\" (len=%d)\n", text, (int)len);

    cmd = (uint8_t)text[0];
    if(cmd >= 'a' && cmd <= 'z')
    {
        cmd = (uint8_t)(cmd - 'a' + 'A');       /* 统一成大写再判断 */
    }

    switch(cmd)
    {
        case NODE_CMD_MUTE:
        {
            /* "M" 用默认时长；"M30" 表示消音 30 秒 */
            uint32_t sec = 0;
            uint8_t  i;

            for(i = 1; i < n && text[i] >= '0' && text[i] <= '9'; i++)
            {
                sec = sec * 10 + (uint32_t)(text[i] - '0');
                if(sec > 3600)
                {
                    sec = 3600;
                    break;
                }
            }
            SmokeNode_Mute(sec ? (sec * 1000) : NODE_MUTE_MS);
            break;
        }

        case NODE_CMD_TEST:
            selftestUntilMs = uptimeMs + NODE_SELFTEST_MS;
            PRINT("[NODE] self test: buzzer on for %d ms\n", NODE_SELFTEST_MS);
            break;

        case NODE_CMD_RESET:
            Sensor_Reset();
            muted = 0;
            PRINT("[NODE] alarm latch cleared\n");
            break;

        default:
            /* 本机没有显示屏，除了上面三个命令，其它内容只记日志 */
            break;
    }
}

/******************************* 文件结束 **********************************/
