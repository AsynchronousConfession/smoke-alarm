/********************************** (C) COPYRIGHT *******************************
 * File Name          : debug_uart.c
 * Description        : 调试串口 UART0（PB4 = RXD0, PB7 = TXD0）+ printf 重定向
 *
 *   为什么要单独写这个文件？
 *     1) 原来的从机例程用 UART1（TXD1 = PA9）打日志。本工程把调试串口统一成
 *        UART0 的 PB4/PB7：与 CH573F_MQ2_ADC_UART0 例程一致，接线最好找，
 *        也把 PA9 这个 ADC 脚留空给以后扩容（比如再挂一路 MQ-135）。
 *     2) newlib 的 printf 最终要调用 _write()，沁恒 SDK 已经实现好了
 *        （在 SRC/StdPeriphDriver/CH57x_sys.c），但它按 DEBUG 宏的取值
 *        选择串口：DEBUG=0 -> UART0，DEBUG=1 -> UART1，……
 *        所以本工程编译时用 -DDEBUG=0，日志就会从 UART0(PB7) 出来。
 *
 *   接线：USB-TTL 的 TX -> PB4，RX -> PB7，GND 共地；115200 8N1。
 *******************************************************************************/

/*********************************************************************
 * 头文件包含
 */
#include "CONFIG.h"

/*********************************************************************
 * @fn      DebugUart_Init
 *
 * @brief   初始化调试串口（UART0，PB4/PB7，115200 8N1）
 *
 * @return  无
 */
void DebugUart_Init(void)
{
#ifdef DEBUG
    /* 先把 TX 引脚置高再切成推挽输出，避免初始化瞬间出现低电平毛刺 */
    GPIOB_SetBits(bTXD0);                       /* PB7 = TXD0 */
    GPIOB_ModeCfg(bTXD0, GPIO_ModeOut_PP_5mA);
    GPIOB_ModeCfg(bRXD0, GPIO_ModeIN_PU);       /* PB4 = RXD0 */

    UART0_DefInit();
    UART0_BaudRateCfg(115200);
#endif
}

/*********************************************************************
 * @fn      _write
 *
 * @brief   newlib 的底层写接口：printf / PRINT 的每个字节都从这里发到 UART0
 *
 * @param   file - 文件描述符（未使用）
 *          ptr  - 数据
 *          len  - 长度
 *
 * @return  实际写出的字节数
 */
/******************************* 文件结束 **********************************/
