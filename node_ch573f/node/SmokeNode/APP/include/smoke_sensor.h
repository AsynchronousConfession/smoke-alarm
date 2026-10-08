/********************************** (C) COPYRIGHT *******************************
 * File Name          : smoke_sensor.h
 * Description        : MQ 系列气敏传感器采样 + 自适应基线 + 报警判定
 *
 *   算法与 CH573F_MQ2_ADC_UART0 例程一致（那份例程已验证可用）：
 *     1) AO 电压 -> 传感器等效电阻 Rs = RL × (VCC − Vao) / Vao
 *     2) 自适应基线 Rs0：每个样本最多跟随 NODE_BASE_STEP_PM‰，
 *        预热漂移/温湿慢漂移跟得上，几秒钟的气体事件跟不上
 *     3) Δ% = (Rs / Rs0 − 1) × 100，|Δ%| 超过阈值并保持几个样本 -> 报警
 *
 *   注意：正常 MQ 系列传感器闻到可燃气/烟雾时 Rs 变小、Δ% 变负；
 *   若你那块模块的分压方向相反，Δ% 会变正 —— 以"变化幅度"为准，判据取绝对值。
 *******************************************************************************/

#ifndef SMOKE_SENSOR_H
#define SMOKE_SENSOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include "CH57x_common.h"
#include "node_cfg.h"

/* 一次采样得到的所有量 */
typedef struct
{
    uint16_t adc;       /* ADC 原始值 0~4095 */
    uint16_t ao_mv;     /* 还原到模块 AO 引脚的电压 mV（已按分压比反算） */
    uint32_t rs_ohm;    /* 传感器等效电阻 Rs(Ω)，上限 999999 */
    int16_t  dpct_x10;  /* 相对基线的变化 Δ% ×10（例如 -523 表示 −52.3%） */
    uint8_t  do_level;  /* 模块 DO 引脚电平 0/1（没有 DO 时恒为 0） */
    uint8_t  warming;   /* 1 = 还在预热 */
    uint8_t  alarm;     /* 1 = 报警中 */
    uint32_t base_rs;   /* 当前基线 Rs0 */
} sensor_sample_t;

/* 初始化 ADC / DO 引脚 / 内部状态（在 main() 里，外设初始化之后调用一次） */
void Sensor_Init(void);

/* 采样一次并更新全部内部状态（由节点任务按 NODE_SAMPLE_PERIOD_MS 周期调用） */
void Sensor_Poll(void);

/* 取最近一次采样的结果（指针常驻，不要释放） */
const sensor_sample_t *Sensor_Get(void);

/* 预热剩余秒数（0 表示预热已结束） */
uint16_t Sensor_WarmupRemainSec(void);

/* 复位：清报警状态并把基线重置为下一次采样的值（网页下发 "RESET" 时用） */
void Sensor_Reset(void);

#ifdef __cplusplus
}
#endif

#endif /* SMOKE_SENSOR_H */
