#ifndef H_MAINTENANCE_H
#define H_MAINTENANCE_H

#include <stdbool.h>
#include <stdint.h>
#include "Maintenance_Config.h"

typedef struct
{
    uint8_t tx[MAINTENANCE_TX_SIZE]; // 异步发送使用的持久缓冲区。
    volatile uint32_t milliseconds; // SysTick 中断累计的毫秒计数。
    volatile uint16_t rx_head, rx_tail; // 中断写队头，主循环写队尾。
    volatile uint32_t uart_error_count; // 串口接收或发送异常累计次数。
    volatile bool rx_fault, tx_busy; // 中断与主循环共享的接收故障和发送忙标志。
    uint32_t tx_started_ms; // 本轮发送起始时间，用于超时判断。
    volatile uint8_t rx[MAINTENANCE_RX_SIZE]; // 中断写入、主循环读取的环形接收缓冲区。
} Maintenance_Port; // 硬件状态；与中断共享的字段保留 volatile 修饰。

/*
 * 函数名：H_Maintenance_PortInit
 * 说明：绑定中断上下文，配置软件 I²C、打开 SCI9、设置 1 毫秒 SysTick 并开启屏幕电源
 * 输入：g_port：非空硬件状态指针，硬件运行期间地址保持有效，首次调用前由功能层清零
 * 输出：返回 true 表示所有初始化步骤成功，false 表示外设配置失败；无数据输出参数，保存 g_port 供中断使用并更新硬件配置
 * 使用：供 F_Maintenance.c 跨文件调用；主循环首次任务初始化时执行，独占一组 SCI9/SysTick，不支持同时绑定多个实例
 */
bool H_Maintenance_PortInit(Maintenance_Port *g_port);

/*
 * 函数名：H_Maintenance_PortNow
 * 说明：读取由 SysTick 累计的毫秒计数
 * 输入：g_port：非空硬件状态指针，硬件运行期间地址保持有效
 * 输出：返回 32 位毫秒计数，计数自然回绕；无输出参数
 * 使用：供 F_Maintenance.c 跨文件调用，也用于本文件的串口超时判断
 */
uint32_t H_Maintenance_PortNow(const Maintenance_Port *g_port);

/*
 * 函数名：H_Maintenance_PortReadByte
 * 说明：从指定硬件上下文的接收环形队列取出一个字节
 * 输入：g_port：非空硬件状态指针，硬件运行期间地址保持有效
 * 输出：byte：成功时写入接收字节，指针必须非空；返回 true 表示取到数据，false 表示队列为空
 *       g_port：成功时推进 rx_tail，队列为空时不修改 byte
 * 使用：供 F_Maintenance.c 跨文件调用；主循环读取，中断负责写入队列
 */
bool H_Maintenance_PortReadByte(Maintenance_Port *g_port, uint8_t *byte);

/*
 * 函数名：H_Maintenance_PortRxFault
 * 说明：原子读取并清除接收故障标志，发生故障时丢弃未处理的接收数据
 * 输入：g_port：非空硬件状态指针，硬件运行期间地址保持有效
 * 输出：返回本次读取到的故障标志；g_port：有故障时清除 rx_fault 并将 rx_tail 对齐 rx_head
 * 使用：供 F_Maintenance.c 跨文件调用；主循环检查通信状态，短暂屏蔽中断后恢复进入前的屏蔽状态
 */
bool H_Maintenance_PortRxFault(Maintenance_Port *g_port);

/*
 * 函数名：H_Maintenance_PortSend
 * 说明：复制数据到持久发送缓冲区并启动异步串口发送，处理发送忙及超时
 * 输入：g_port：非空硬件状态指针，硬件运行期间地址保持有效
 *       data：待发送的非空缓冲区
 *       length：字节数，范围为 1～MAINTENANCE_TX_SIZE
 * 输出：返回 true 仅表示驱动已受理发送，不代表物理发送完成；false 表示忙、参数非法或驱动失败
 *       g_port：更新发送缓冲区、开始时间、忙标志及相关错误状态
 * 使用：供 F_Maintenance.c 跨文件调用；主循环非阻塞发送，调用方在返回后可复用原 data 缓冲区
 */
bool H_Maintenance_PortSend(Maintenance_Port *g_port, const uint8_t *data, uint16_t length);

/*
 * 函数名：H_Maintenance_EepromRead
 * 说明：通过软件 I²C 从 24C256 指定地址连续读取数据
 * 输入：address：起始字节地址，地址加长度不得超过 32768
 *       data：非空可写缓冲区，容量至少为 length 字节
 *       length：读取字节数，必须大于零
 * 输出：返回 true 表示整个读取和停止条件成功，false 表示参数或总线错误；data：写入读取结果，失败时可能只更新部分数据
 * 使用：供 F_Maintenance.c 跨文件调用；主循环上下文，硬件初始化后用于日志扫描和回读校验
 */
bool H_Maintenance_EepromRead(uint16_t address, uint8_t *data, uint16_t length);

/*
 * 函数名：H_Maintenance_EepromWrite
 * 说明：按 EEPROM 页边界拆分写入，并在每页写入后有限轮询应答
 * 输入：address：起始字节地址，地址加长度不得超过 32768
 *       data：非空待写缓冲区，至少包含 length 字节
 *       length：写入字节数，必须大于零
 * 输出：返回 true 表示全部写入完成，false 表示参数或总线错误；无输出参数，失败时 EEPROM 可能已写入部分数据
 * 使用：供 F_Maintenance.c 跨文件调用；主循环同步执行，写入前关闭写保护，完成或通信失败后恢复写保护
 */
bool H_Maintenance_EepromWrite(uint16_t address, const uint8_t *data, uint16_t length);

/*
 * 函数名：SysTick_Handler
 * 说明：处理 1 毫秒时基中断，对已绑定的硬件上下文累加毫秒数
 * 输入：无，使用初始化时保存的硬件状态绑定
 * 输出：无返回值；已绑定上下文的 milliseconds 增加一，未绑定时不处理
 * 使用：由 MCU 中断向量跨文件引用，覆盖 BSP 弱定义；固定名称不可更改，模块独占 SysTick
 */
void SysTick_Handler(void);

#endif
