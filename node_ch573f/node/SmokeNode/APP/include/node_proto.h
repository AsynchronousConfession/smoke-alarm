/********************************** (C) COPYRIGHT *******************************
 * File Name          : node_proto.h
 * Description        : 节点 <-> 网关 的 BLE 空中协议定义（两侧共用同一份约定）
 *
 * 物理通道：沁恒 SimpleProfile（服务 0xFFE0）
 *   0xFFE1  Read/Write  备用
 *   0xFFE3  Write       网关 -> 节点（命令/文本，最长 20 字节 ASCII）
 *   0xFFE4  Notify      节点 -> 网关（遥测帧/信息帧，最长 20 字节）
 *
 * 一帧长度不超过 ATT_MTU-3 = 20 字节（默认 MTU 23），所以遥测帧做成
 * 紧凑的二进制结构，而不是 JSON —— 20 字节里塞不下 JSON。
 *
 * 遥测帧（16 字节，小端）：
 *   偏移  长度  含义
 *    0     1    魔数 0x53 ('S')
 *    1     1    帧类型 0x01 = 遥测
 *    2     1    序号（0~255 循环，用来粗查丢包）
 *    3     1    标志位 bit0=报警 bit1=预热中 bit2=DO 为高 bit3=已消音
 *    4     2    ADC 原始值 0~4095
 *    6     2    模块 AO 电压 mV
 *    8     4    传感器等效电阻 Rs（Ω）
 *   12     2    Δ%（相对基线），单位 0.1%，带符号
 *   14     1    DO 电平 0/1
 *   15     1    校验：前 15 字节求和 & 0xFF
 *
 * 信息帧（20 字节）：
 *    0     1    魔数 0x53 ('S')
 *    1     1    帧类型 0x02 = 信息
 *    2~19  18   ASCII 文本，形如 "N01|MQ-2|1.0"（节点编号|型号|版本），
 *               不足 18 字节用 0x00 补齐
 *******************************************************************************/

#ifndef NODE_PROTO_H
#define NODE_PROTO_H

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_FRAME_MAGIC        0x53    /* 'S' */
#define NODE_FRAME_TELEMETRY    0x01
#define NODE_FRAME_INFO         0x02

#define NODE_TELEMETRY_LEN      16
#define NODE_INFO_LEN           20
#define NODE_INFO_TEXT_LEN      18

/* 遥测帧各字段偏移 */
#define TELE_OFF_MAGIC          0
#define TELE_OFF_TYPE           1
#define TELE_OFF_SEQ            2
#define TELE_OFF_FLAGS          3
#define TELE_OFF_ADC            4
#define TELE_OFF_AO_MV          6
#define TELE_OFF_RS             8
#define TELE_OFF_DPCT           12
#define TELE_OFF_DO             14
#define TELE_OFF_SUM            15

/* 标志位 */
#define NODE_FLAG_ALARM         0x01
#define NODE_FLAG_WARMUP        0x02
#define NODE_FLAG_DO            0x04
#define NODE_FLAG_MUTED         0x08

/* 网关 -> 节点的命令（写 0xFFE3，纯 ASCII，最长 20 字节）
 *   "M"  或 "MUTE"      消音 NODE_MUTE_MS
 *   "M30"               消音 30 秒
 *   "T"  或 "TEST"      自检（蜂鸣器长鸣一次）
 *   "R"  或 "RESET"     清除报警锁存
 *   其它字符串           只在节点串口日志里打印（本机没有显示屏） */
#define NODE_CMD_MUTE           'M'
#define NODE_CMD_TEST           'T'
#define NODE_CMD_RESET          'R'

#ifdef __cplusplus
}
#endif

#endif /* NODE_PROTO_H */
