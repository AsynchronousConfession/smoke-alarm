/********************************** (C) COPYRIGHT *******************************
 * File Name          : node_cfg.h
 * Description        : 烟雾报警节点 —— 全部可配置项集中在这一个文件
 *
 *   一台 CH573F 最小系统板 + 一个 MQ 系列气敏传感器 = 一个"房间节点"。
 *   所有节点烧同一份固件，只有 NODE_TAG（节点编号）建议每块板子改成不同值，
 *   这样网页上就能直接看出"是哪个房间"在报警。
 *******************************************************************************/

#ifndef NODE_CFG_H
#define NODE_CFG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "CH57x_common.h"

/* ==================== 1. 节点身份 ==================== */

/* 节点编号：每块板子必须不同，例如 N01 / N02 / N03 / 厨房 ……（ASCII）。
 * 可以用编译参数覆盖，不改源码就能一次产出三块板子的固件：
 *     tools\build_node.ps1 -Tag N02 -Model "MQ-137" -Out .\deploy\fw
 * 下面的默认值只在"直接在 MounRiver 里 Build"时生效。 */
#ifndef NODE_TAG
#define NODE_TAG                "N01"
#endif

/* 传感器型号：决定网关和屏幕怎么解释这一路数据（MQ-2 / MQ-135 / MQ-137 / MQ-7 …）。
 * ★ 屏幕上的"型号 -> 数据含义"对照表在 cyd_gateway\main\ui.c 的 s_sensors[]，
 *   这里换了型号，那边要对应加一行，否则屏幕会退回"气体浓度"的兜底文案。 */
#ifndef NODE_MODEL
#define NODE_MODEL              "MQ-2"
#endif

/* 固件版本，给网页显示用 */
#define NODE_FW_VER             "1.0"

/* 广播名：ESP32 网关按这个名字过滤并连接，所有节点必须一致 */
#define NODE_ADV_NAME           "SMOKE_NODE"

/* ==================== 2. 传感器接线 ==================== */

/* AO 接在哪个 ADC 引脚上（DO -> PA5）。CH573F(QFN28) 能用的 ADC 引脚只有这些，
 * 通道号和引脚必须成对，映射来自 CH573SFR.h 的 bAINx：
 *      AIN0  -> PA4     AIN1  -> PA5     AIN2  -> PA12    AIN3  -> PA13
 *      AIN4  -> PA14    AIN5  -> PA15    AIN12 -> PA8     AIN13 -> PA9
 *      （AIN8 -> PB0、AIN9 -> PB6，本工程不用）
 * ★ 不要占 PB4/PB7（调试串口）、PB15（蜂鸣器）。
 * 不用改源码：编译时用 -AoPin 指定，脚本会自动带出对应的 ADC 通道号，例如
 *      tools\build_node.ps1 -Tag N01 -Model "MQ-135" -AoPin PA15 -Out ...\N01.hex */
#ifndef NODE_AO_CH
#define NODE_AO_CH              CH_EXTIN_0
#endif
#ifndef NODE_AO_PIN
#define NODE_AO_PIN             GPIO_Pin_4
#endif
#ifndef NODE_AO_PIN_NAME
#define NODE_AO_PIN_NAME        "PA4"
#endif

/* DO（模块上比较器的数字输出）-> PA5，可选。不用就把 NODE_HAS_DO 改成 0。 */
#define NODE_HAS_DO             1
#define NODE_DO_PIN             GPIO_Pin_5
#define NODE_DO_PIN_NAME        "PA5"

/* 模块供电电压(mV)：接 3.3V 写 3300；接 5V（推荐，加热更充分）写 5000。
 * 接 5V 时 AO 必须分压后再进 MCU，见 NODE_AO_DIV_X100。
 * 这三个电气参数也能用编译参数覆盖（见 tools\build_node.ps1 的 -Define），例如：
 *     -Define NODE_MODULE_VCC_MV=5000,NODE_AO_DIV_X100=200   ← 5V + 10k/10k 分压 */
#ifndef NODE_MODULE_VCC_MV
#define NODE_MODULE_VCC_MV      3300
#endif

/* AO 到 MCU 的分压比 ×100：直连 = 100；10k+10k 对半分压 = 200；10k+20k = 300 */
#ifndef NODE_AO_DIV_X100
#define NODE_AO_DIV_X100        100
#endif

/* 模块底板上的负载电阻 RL（常见 1kΩ），只影响 Rs 的绝对值 */
#ifndef NODE_RL_OHM
#define NODE_RL_OHM             1000
#endif

/* CH573 的 ADC 参考电压 = 芯片供电 3.3V */
#define NODE_ADC_VREF_MV        3300

/* 每次上报前连续采样多少次取平均（越大越稳，耗时也越长） */
#define NODE_SAMPLE_NUM         16

/* ==================== 3. 报警判定 ==================== */

/* 上电预热时间(ms)：这段时间只跟随基线，不报警 */
#define NODE_WARMUP_MS          30000

/* 自适应基线每样本最多跟随 1%（‰=10）：慢漂移跟得上，几秒钟的气体事件跟不上 */
#define NODE_BASE_STEP_PM       10

/* 采样周期(ms)：基线跟随即由它决定 */
#define NODE_SAMPLE_PERIOD_MS   500

/* |Δ%| >= NODE_ALARM_PCT 连续 NODE_ALARM_HOLD 次 -> 报警 */
#define NODE_ALARM_PCT          25
#define NODE_ALARM_HOLD         2

/* |Δ%| 回落到 NODE_CLEAR_PCT 以下连续 NODE_CLEAR_HOLD 次 -> 解除报警 */
#define NODE_CLEAR_PCT          12
#define NODE_CLEAR_HOLD         5

/* ==================== 4. 上报节奏 ==================== */

#define NODE_REPORT_NORMAL_MS   2000    /* 正常时每 2 秒上报一次 */
#define NODE_REPORT_ALARM_MS    500     /* 报警时每 0.5 秒上报一次 */
#define NODE_TICK_MS            100     /* 节点任务的心跳周期(ms) */

/* ==================== 5. 本地外设 ====================
 *
 * 节点硬件只有：CH573F 最小系统板 + MQ 传感器 + 蜂鸣器。
 * （没有 OLED、没有按键，固件里也没有这两部分代码。）
 * 蜂鸣器是节点唯一的本地报警输出：报警断续响、自检长鸣、消音后不响。
 */

/* 蜂鸣器接 PB15（经三极管/驱动模块，高电平响）。
 * 蜂鸣器属于报警器的"本机报警"，建议接上；确实没有就把这里改成 0，
 * 此时报警信息只会出现在串口日志和网页上。 */
#define NODE_HAS_BUZZER         1       /* 有源蜂鸣器（或 LED 指示灯） */
#define NODE_BUZZER_PIN         GPIO_Pin_15     /* PB15，官方 EVT 板上的 LED1 也在 PB15 */
#define NODE_BUZZER_ACTIVE_HIGH 1               /* 1: 高电平响；0: 低电平响 */

/* 蜂鸣节奏(ms)：报警时 "响 NODE_BEEP_ON_MS / 停 NODE_BEEP_OFF_MS" 循环 */
#define NODE_BEEP_ON_MS         300
#define NODE_BEEP_OFF_MS        200

/* 消音时长(ms)：按下消音后这段时间内不响，但数据照常上报 */
#define NODE_MUTE_MS            60000

/* 自检时长(ms)：蜂鸣器长鸣一次，用于确认节点还活着 */
#define NODE_SELFTEST_MS        3000

/* 预热结束时短鸣一声(ms)，表示"基线已建立，正式开始检测"。
 * 现场靠这一声判断什么时候可以开始测；不想听就改成 0。 */
#define NODE_READY_BEEP_MS      150

#ifdef __cplusplus
}
#endif

#endif /* NODE_CFG_H */
