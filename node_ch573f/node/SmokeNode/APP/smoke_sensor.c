/********************************** (C) COPYRIGHT *******************************
 * File Name          : smoke_sensor.c
 * Description        : MQ 系列气敏传感器采样 + 自适应基线 + 报警判定
 *
 *   本文件只做"传感器"这一件事，不碰 BLE、也不碰蜂鸣器，
 *   方便单独调试：串口打印、示波器看波形都从这里下手。
 *******************************************************************************/

/*********************************************************************
 * 头文件包含
 */
#include "CONFIG.h"
#include "smoke_sensor.h"

/*********************************************************************
 * 常量定义
 */
/* 预热需要多少个采样点 */
#define SENSOR_WARMUP_SAMPLES   (NODE_WARMUP_MS / NODE_SAMPLE_PERIOD_MS)

/*********************************************************************
 * 局部变量
 */
static sensor_sample_t s_sample;            /* 最近一次采样结果 */
static signed short    s_adc_calib = 0;     /* ADC 内部偏置粗校准值 */
static uint32_t        s_sample_count = 0;  /* 上电以来的采样次数 */
static uint8_t         s_alarm_cnt = 0;     /* 连续超阈值次数 */
static uint8_t         s_clear_cnt = 0;     /* 连续回落次数 */

/*********************************************************************
 * 局部函数声明
 */
static uint16_t Sensor_ReadAdcAvg(void);
static uint32_t Sensor_CalcRs(uint16_t raw, uint16_t *pAoMv);
static void     Sensor_UpdateBaseline(uint32_t rs);

/*********************************************************************
 * @fn      Sensor_Init
 *
 * @brief   初始化 ADC 与 DO 引脚，清空内部状态
 *
 * @return  无
 */
void Sensor_Init(void)
{
    /* AO：模拟输入脚用浮空 */
    GPIOA_ModeCfg(NODE_AO_PIN, GPIO_ModeIN_Floating);

    /* 单端采样，3.2MHz，0dB 增益 */
    ADC_ExtSingleChSampInit(SampleFreq_3_2, ADC_PGA_0);
    ADC_ChannelCfg(NODE_AO_CH);

    /* ADC 内部偏置粗校准（必须在通道配置之前做一次） */
    s_adc_calib = ADC_DataCalib_Rough();
    ADC_ChannelCfg(NODE_AO_CH);

#if (NODE_HAS_DO)
    /* DO 带上拉输入 */
    GPIOA_ModeCfg(NODE_DO_PIN, GPIO_ModeIN_PU);
#endif

    tmos_memset(&s_sample, 0, sizeof(s_sample));
    s_sample.warming = 1;
    s_sample_count   = 0;
    s_alarm_cnt      = 0;
    s_clear_cnt      = 0;
}

/*********************************************************************
 * @fn      Sensor_ReadAdcAvg
 *
 * @brief   连续采样 NODE_SAMPLE_NUM 次取平均并做上下限保护
 *
 * @return  平均后的 ADC 原始值（0 ~ 4095）
 */
static uint16_t Sensor_ReadAdcAvg(void)
{
    uint32_t sum = 0;
    int32_t  avg;
    uint8_t  i;

    for(i = 0; i < NODE_SAMPLE_NUM; i++)
    {
        sum += ADC_ExcutSingleConver();
    }

    avg = (int32_t)(sum / NODE_SAMPLE_NUM) + s_adc_calib;
    if(avg < 0)
    {
        avg = 0;
    }
    if(avg > 4095)
    {
        avg = 4095;
    }

    return (uint16_t)avg;
}

/*********************************************************************
 * @fn      Sensor_CalcRs
 *
 * @brief   由 ADC 值算出模块 AO 电压和传感器等效电阻
 *          Rs = RL × (VCC − Vao) / Vao
 *
 * @param   raw   - ADC 原始值
 * @param   pAoMv - 输出：模块 AO 电压(mV)（已按分压比还原）
 *
 * @return  Rs(Ω)，限幅到 999999
 */
static uint32_t Sensor_CalcRs(uint16_t raw, uint16_t *pAoMv)
{
    uint32_t pinMv = (uint32_t)raw * NODE_ADC_VREF_MV / 4096;   /* 引脚上的电压 */
    uint32_t aoMv  = pinMv * NODE_AO_DIV_X100 / 100;             /* 还原成模块 AO */
    uint32_t rs;

    if(aoMv > 65535)
    {
        aoMv = 65535;
    }
    *pAoMv = (uint16_t)aoMv;

    if(aoMv == 0)
    {
        return 999999;                                          /* 0V 按"无穷大"处理 */
    }
    if(aoMv >= NODE_MODULE_VCC_MV)
    {
        return 0;
    }

    rs = (uint32_t)NODE_RL_OHM * (NODE_MODULE_VCC_MV - aoMv) / aoMv;
    if(rs > 999999)
    {
        rs = 999999;
    }
    return rs;
}

/*********************************************************************
 * @fn      Sensor_UpdateBaseline
 *
 * @brief   自适应基线：每个样本最多跟随 NODE_BASE_STEP_PM‰
 *
 * @param   rs - 当前 Rs
 *
 * @return  无
 */
static void Sensor_UpdateBaseline(uint32_t rs)
{
    int32_t diff;
    int32_t lim;

    if(s_sample.base_rs == 0)          /* 第一个样本直接当基线 */
    {
        s_sample.base_rs = rs;
        return;
    }

    lim = (int32_t)(s_sample.base_rs / 1000 * NODE_BASE_STEP_PM);
    if(lim < 1)
    {
        lim = 1;
    }

    diff = (int32_t)rs - (int32_t)s_sample.base_rs;
    if(diff > lim)
    {
        diff = lim;
    }
    if(diff < -lim)
    {
        diff = -lim;
    }

    s_sample.base_rs = (uint32_t)((int32_t)s_sample.base_rs + diff);
}

/*********************************************************************
 * @fn      Sensor_Poll
 *
 * @brief   采样一次：算 Rs → 更新基线 → 算 Δ% → 判报警
 *
 * @return  无
 */
void Sensor_Poll(void)
{
    uint16_t raw;
    int32_t  d_pct;
    int32_t  abs_pct;

    /* 1. 采样 + 换算 */
    raw             = Sensor_ReadAdcAvg();
    s_sample.adc    = raw;
    s_sample.rs_ohm = Sensor_CalcRs(raw, &s_sample.ao_mv);

#if (NODE_HAS_DO)
    s_sample.do_level = (GPIOA_ReadPortPin(NODE_DO_PIN) ? 1 : 0);
#else
    s_sample.do_level = 0;
#endif

    /* 2. 基线跟随（Rs 异常为 0 时不参与，避免除零和污染基线） */
    if(s_sample.rs_ohm > 0)
    {
        Sensor_UpdateBaseline(s_sample.rs_ohm);
    }

    /* 3. Δ% = (Rs/Rs0 - 1) × 100，存的是 ×10 的整数 */
    if(s_sample.base_rs > 0)
    {
        d_pct = (int32_t)(((int32_t)s_sample.rs_ohm - (int32_t)s_sample.base_rs) * 1000
                          / (int32_t)s_sample.base_rs);
    }
    else
    {
        d_pct = 0;
    }
    if(d_pct > 32767)
    {
        d_pct = 32767;
    }
    if(d_pct < -32768)
    {
        d_pct = -32768;
    }
    s_sample.dpct_x10 = (int16_t)d_pct;

    /* 4. 预热判定：预热期间只跟基线，不报警 */
    s_sample_count++;
    if(s_sample_count <= SENSOR_WARMUP_SAMPLES)
    {
        s_sample.warming = 1;
        s_sample.alarm   = 0;
        s_alarm_cnt      = 0;
        s_clear_cnt      = 0;
        return;
    }
    s_sample.warming = 0;

    /* 5. 报警判定：启动要连续 NODE_ALARM_HOLD 次超阈值，
     *    解除要连续回落到 NODE_CLEAR_PCT 以下 NODE_CLEAR_HOLD 次（迟滞，防止抖动） */
    abs_pct = (d_pct >= 0) ? d_pct : -d_pct;

    if(abs_pct >= NODE_ALARM_PCT * 10)
    {
        if(s_alarm_cnt < 250)
        {
            s_alarm_cnt++;
        }
        s_clear_cnt = 0;
    }
    else if(s_sample.alarm)
    {
        if(abs_pct <= NODE_CLEAR_PCT * 10)
        {
            if(s_clear_cnt < 250)
            {
                s_clear_cnt++;
            }
        }
        else
        {
            s_clear_cnt = 0;
        }

        if(s_clear_cnt >= NODE_CLEAR_HOLD)
        {
            s_sample.alarm = 0;
            s_alarm_cnt    = 0;
            s_clear_cnt    = 0;
        }
    }
    else
    {
        s_alarm_cnt = 0;
        s_clear_cnt = 0;
    }

    if(!s_sample.alarm && s_alarm_cnt >= NODE_ALARM_HOLD)
    {
        s_sample.alarm = 1;
        s_clear_cnt    = 0;
    }
}

/*********************************************************************
 * @fn      Sensor_Get
 *
 * @brief   取最近一次采样结果
 *
 * @return  指向内部结构体的指针（只读使用）
 */
const sensor_sample_t *Sensor_Get(void)
{
    return &s_sample;
}

/*********************************************************************
 * @fn      Sensor_WarmupRemainSec
 *
 * @brief   预热剩余秒数
 *
 * @return  剩余秒数；0 = 预热已结束
 */
uint16_t Sensor_WarmupRemainSec(void)
{
    if(s_sample_count >= SENSOR_WARMUP_SAMPLES)
    {
        return 0;
    }
    return (uint16_t)((SENSOR_WARMUP_SAMPLES - s_sample_count) * NODE_SAMPLE_PERIOD_MS / 1000);
}

/*********************************************************************
 * @fn      Sensor_Reset
 *
 * @brief   清除报警状态（网页下发 "RESET" / 按 K4 时调用）
 *
 *   只清"报警锁存"，不动基线：基线本来就是慢跟随的，动它反而会让
 *   真正有烟的时候被重新当成背景，对报警器来说是不安全的。
 *
 * @return  无
 */
void Sensor_Reset(void)
{
    s_sample.alarm = 0;
    s_alarm_cnt    = 0;
    s_clear_cnt    = 0;
}

/******************************* 文件结束 **********************************/
