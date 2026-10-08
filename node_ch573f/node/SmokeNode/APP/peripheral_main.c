/********************************** (C) COPYRIGHT *******************************
 * File Name          : peripheral_main.c
 * Description        : CH573F 烟雾报警节点 —— 主函数与任务系统初始化
 *
 *   一台节点的完整启动顺序：
 *     时钟 60MHz → 调试串口 UART0(PB4/PB7) → BLE 协议栈 → HAL
 *       → GAP 从机角色 → Peripheral_Init(广播/服务)
 *       → SmokeNode_Init(传感器 + 蜂鸣器 + 上报任务) → 主循环
 *
 *   节点的硬件只有三样：CH573F 最小系统板 + MQ 传感器 + 蜂鸣器。
 *   没有 OLED、没有按键，所以固件里也不包含这两部分代码；
 *   本地状态只能通过蜂鸣器（响/不响）和串口日志观察。
 *
 *   本工程基于沁恒官方外设从机例程改造，
 *   传感器部分的算法来自 CH573F_MQ2_ADC_UART0 例程。
 *******************************************************************************/

/******************************************************************************/
/* 头文件包含 */
#include "CONFIG.h"
#include "HAL.h"
#include "gattprofile.h"
#include "peripheral.h"
#include "smoke_node.h"

/*********************************************************************
 * 全局变量
 */
/* BLE 协议栈使用的内存池（大小由 config.h 的 BLE_MEMHEAP_SIZE 决定） */
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

/* 如果打开了 BLE_MAC（config.h），就用这里的固定 MAC；默认用芯片出厂 MAC */
#if(defined(BLE_MAC)) && (BLE_MAC == TRUE)
const uint8_t MacAddr[6] = {0x84, 0xC2, 0xE4, 0x03, 0x02, 0x02};
#endif

/*********************************************************************
 * @fn      DebugUart_Init
 *
 * @brief   UART0 调试串口初始化（实现在 debug_uart.c）
 *
 * @return  无
 */
extern void DebugUart_Init(void);

/*********************************************************************
 * @fn      Main_Circulation
 *
 * @brief   主循环：TMOS 事件处理
 *
 * @return  无（不返回）
 */
__attribute__((section(".highcode")))
__attribute__((noinline))
void Main_Circulation(void)
{
    while(1)
    {
        TMOS_SystemProcess();       /* 蓝牙协议栈与各任务的事件处理 */
    }
}

/*********************************************************************
 * @fn      main
 *
 * @brief   主函数
 *
 * @return  无（不返回）
 */
int main(void)
{
#if(defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif

    /* 1. 系统主频 60MHz（ADC / 软件 I2C 延时都依赖这个时钟） */
    SetSysClock(CLK_SOURCE_PLL_60MHz);

#if(defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    GPIOA_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
#endif

    /* 2. 调试串口：UART0 = PB4(RXD0) / PB7(TXD0)，115200 8N1 */
    DebugUart_Init();

    PRINT("%s\n", VER_LIB);
    PRINT("=========== CH573F 烟雾报警节点 ===========\n");

    /* 3. 蓝牙协议栈 + HAL */
    CH57X_BLEInit();
    HAL_Init();

    /* 4. GAP 从机角色（开始广播） + 自定义服务 */
    GAPRole_PeripheralInit();
    Peripheral_Init();

    /* 5. 传感器采样、报警判定、周期上报、蜂鸣器 */
    SmokeNode_Init();

    /* 6. 进入主循环 */
    Main_Circulation();
}

/******************************** endfile @ peripheral_main *******************/
