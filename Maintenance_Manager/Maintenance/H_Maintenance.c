#include "hal_data.h"
#include "H_Maintenance.h"
#include <string.h>

// 固定中断签名不能传入上下文，因此仅保留一个文件内绑定；普通硬件函数仍使用入口参数。
typedef struct
{
    Maintenance_Port *g_port;
} Maintenance_Interrupt_Binding;

static Maintenance_Interrupt_Binding g_binding; // 硬件初始化时绑定，生命周期覆盖串口和 SysTick 运行期。

/*
 * 函数名：H_Maintenance_DelayHalf
 * 说明：产生软件 I²C 的 5 微秒半周期延时
 * 输入：无
 * 输出：无返回值；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于标称不高于 100 kHz 的 I²C 时序，期间保持中断原有使能状态
 */
static void H_Maintenance_DelayHalf(void) { R_BSP_SoftwareDelay(5U, BSP_DELAY_UNITS_MICROSECONDS); }

/*
 * 函数名：H_Maintenance_Scl
 * 说明：设置软件 I²C 时钟线 SCL的开漏输出状态
 * 输入：high：true 释放总线并由外部电阻拉高，false 主动拉低
 * 输出：无返回值；无输出参数，直接更新对应引脚电平
 * 使用：仅在本 .c 文件内部调用，用于 I²C 起止条件和位时序
 */
static void H_Maintenance_Scl(bool high)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, SCL_24C, high ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW); // 开漏输出高电平表示释放总线，由 R55/R56 上拉电阻拉高。
}

/*
 * 函数名：H_Maintenance_Sda
 * 说明：设置软件 I²C 数据线 SDA的开漏输出状态
 * 输入：high：true 释放总线并由外部电阻拉高，false 主动拉低
 * 输出：无返回值；无输出参数，直接更新对应引脚电平
 * 使用：仅在本 .c 文件内部调用，用于 I²C 起止条件和位时序
 */
static void H_Maintenance_Sda(bool high)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, SDA_24C, high ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW); // 开漏输出高电平表示释放总线，由 R55/R56 上拉电阻拉高。
}

/*
 * 函数名：H_Maintenance_LineHigh
 * 说明：读取指定引脚并判断其电平是否为高
 * 输入：pin：FSP 引脚编号
 * 输出：返回 true 表示读取成功且为高电平，false 表示低电平或读取失败；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于总线检测和应答采样
 */
static bool H_Maintenance_LineHigh(bsp_io_port_pin_t pin)
{
    bsp_io_level_t level = BSP_IO_LEVEL_LOW;
    return (R_IOPORT_PinRead(&g_ioport_ctrl, pin, &level) == FSP_SUCCESS) && (level == BSP_IO_LEVEL_HIGH);
}

/*
 * 函数名：H_Maintenance_ClockHigh
 * 说明：释放 SCL 并有限等待实际电平变高，允许从设备拉长时钟
 * 输入：无
 * 输出：返回 true 表示时钟线已拉高，false 表示等待超时；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于需要 SCL 高电平的总线操作
 */
static bool H_Maintenance_ClockHigh(void)
{
    uint16_t limit = 200U;
    H_Maintenance_Scl(true);
    while (!H_Maintenance_LineHigh(SCL_24C))
    {
        if (limit-- == 0U) { return false; } // 限制时钟拉伸等待次数，避免总线故障导致死循环。
        H_Maintenance_DelayHalf();
    }
    H_Maintenance_DelayHalf();
    return true;
}

/*
 * 函数名：H_Maintenance_Stop
 * 说明：产生 I²C 停止条件并检查 SDA 是否已释放
 * 输入：无
 * 输出：返回 true 表示时钟拉高成功且数据线为高，false 表示总线异常；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于结束访问和总线恢复
 */
static bool H_Maintenance_Stop(void)
{
    bool ok;
    H_Maintenance_Scl(false); H_Maintenance_Sda(false); H_Maintenance_DelayHalf();
    ok = H_Maintenance_ClockHigh();
    H_Maintenance_Sda(true); H_Maintenance_DelayHalf();
    return ok && H_Maintenance_LineHigh(SDA_24C);
}

/*
 * 函数名：H_Maintenance_Start
 * 说明：检查总线状态并产生 I²C 起始或重复起始条件
 * 输入：无
 * 输出：返回 true 表示起始条件成功，false 表示时钟或数据线异常；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于 EEPROM 寻址和应答轮询
 */
static bool H_Maintenance_Start(void)
{
    H_Maintenance_Sda(true); H_Maintenance_DelayHalf();
    if (!H_Maintenance_ClockHigh() || !H_Maintenance_LineHigh(SDA_24C)) { return false; }
    H_Maintenance_Sda(false); H_Maintenance_DelayHalf(); H_Maintenance_Scl(false); H_Maintenance_DelayHalf();
    return true;
}

/*
 * 函数名：H_Maintenance_RecoverBus
 * 说明：检查 I²C 总线，数据线被占用时发送九个时钟并尝试停止条件
 * 输入：无
 * 输出：返回 true 表示总线可用或恢复成功，false 表示总线异常；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于 EEPROM 每次访问前的总线恢复
 */
static bool H_Maintenance_RecoverBus(void)
{
    uint8_t i;
    H_Maintenance_Sda(true);
    if (!H_Maintenance_ClockHigh()) { return false; }
    if (H_Maintenance_LineHigh(SDA_24C)) { return true; }
    for (i = 0; i < 9U; ++i)
    {
        H_Maintenance_Scl(false); H_Maintenance_DelayHalf();
        if (!H_Maintenance_ClockHigh()) { return false; }
    }
    return H_Maintenance_Stop();
}

/*
 * 函数名：H_Maintenance_WriteByte
 * 说明：按高位在前发送一个 I²C 字节并读取从设备应答
 * 输入：value：待发送字节
 * 输出：返回 true 表示收到应答，false 表示无应答或时钟超时；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于设备地址、存储地址和数据发送
 */
static bool H_Maintenance_WriteByte(uint8_t value)
{
    uint8_t i;
    bool ack;
    for (i = 0; i < 8U; ++i)
    {
        H_Maintenance_Sda((value & 0x80U) != 0U); H_Maintenance_DelayHalf();
        if (!H_Maintenance_ClockHigh()) { return false; }
        H_Maintenance_Scl(false); H_Maintenance_DelayHalf(); value = (uint8_t) (value << 1);
    }
    H_Maintenance_Sda(true); H_Maintenance_DelayHalf();
    if (!H_Maintenance_ClockHigh()) { return false; }
    ack = !H_Maintenance_LineHigh(SDA_24C); // 第九个时钟采样，低电平表示从设备应答。
    H_Maintenance_Scl(false); H_Maintenance_DelayHalf();
    return ack;
}

/*
 * 函数名：H_Maintenance_ReadByte
 * 说明：按高位在前读取一个 I²C 字节，并发送应答或非应答
 * 输入：ack：true 发送应答以继续读取，false 发送非应答以结束读取
 * 输出：value：成功时写入接收字节；返回 true 表示成功，false 表示时钟超时，失败不修改 value
 * 使用：仅在本 .c 文件内部调用，用于 EEPROM 连续读取，最后一字节发送非应答
 */
static bool H_Maintenance_ReadByte(uint8_t *value, bool ack)
{
    uint8_t i, data = 0;
    H_Maintenance_Sda(true);
    for (i = 0; i < 8U; ++i)
    {
        H_Maintenance_DelayHalf();
        if (!H_Maintenance_ClockHigh()) { return false; }
        data = (uint8_t) ((data << 1) | (H_Maintenance_LineHigh(SDA_24C) ? 1U : 0U));
        H_Maintenance_Scl(false); H_Maintenance_DelayHalf();
    }
    H_Maintenance_Sda(!ack); H_Maintenance_DelayHalf(); // 继续读取时发送低电平应答，末字节发送非应答。
    if (!H_Maintenance_ClockHigh()) { return false; }
    H_Maintenance_Scl(false); H_Maintenance_DelayHalf(); H_Maintenance_Sda(true);
    *value = data;
    return true;
}

/*
 * 函数名：H_Maintenance_AddressWrite
 * 说明：发送起始条件、EEPROM 写设备地址和两字节存储地址
 * 输入：address：待访问的 EEPROM 字节地址，由上层保证在有效范围内
 * 输出：返回 true 表示全部字节得到应答，false 表示任一步骤失败；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于读写访问的地址定位
 */
static bool H_Maintenance_AddressWrite(uint16_t address)
{
    return H_Maintenance_Start() && H_Maintenance_WriteByte((uint8_t) (MAINTENANCE_EEPROM_ADDRESS << 1)) &&
           H_Maintenance_WriteByte((uint8_t) (address >> 8)) && H_Maintenance_WriteByte((uint8_t) address);
}

/*
 * 函数名：H_Maintenance_EepromRead
 * 说明：通过软件 I²C 从 24C256 指定地址连续读取数据
 * 输入：address：起始字节地址，地址加长度不得超过 32768
 *       data：非空可写缓冲区，容量至少为 length 字节
 *       length：读取字节数，必须大于零
 * 输出：返回 true 表示整个读取和停止条件成功，false 表示参数或总线错误；data：写入读取结果，失败时可能只更新部分数据
 * 使用：供 F_Maintenance.c 跨文件调用；主循环上下文，硬件初始化后用于日志扫描和回读校验
 */
bool H_Maintenance_EepromRead(uint16_t address, uint8_t *data, uint16_t length)
{
    uint16_t i;
    bool ok;
    if ((data == NULL) || (length == 0U) || ((uint32_t) address + length > 32768U)) { return false; }
    ok = H_Maintenance_RecoverBus() && H_Maintenance_AddressWrite(address) && H_Maintenance_Start() &&
         H_Maintenance_WriteByte((uint8_t) ((MAINTENANCE_EEPROM_ADDRESS << 1) | 1U));
    for (i = 0; ok && (i < length); ++i) { ok = H_Maintenance_ReadByte(&data[i], (i + 1U) < length); }
    if (!H_Maintenance_Stop()) { ok = false; }
    return ok;
}

/*
 * 函数名：H_Maintenance_EepromWrite
 * 说明：按 EEPROM 页边界拆分写入，并在每页写入后有限轮询应答
 * 输入：address：起始字节地址，地址加长度不得超过 32768
 *       data：非空待写缓冲区，至少包含 length 字节
 *       length：写入字节数，必须大于零
 * 输出：返回 true 表示全部写入完成，false 表示参数或总线错误；无输出参数，失败时 EEPROM 可能已写入部分数据
 * 使用：供 F_Maintenance.c 跨文件调用；主循环同步执行，写入前关闭写保护，完成或通信失败后恢复写保护
 */
bool H_Maintenance_EepromWrite(uint16_t address, const uint8_t *data, uint16_t length)
{
    uint16_t offset = 0;
    bool ok = true;
    if ((data == NULL) || (length == 0U) || ((uint32_t) address + length > 32768U)) { return false; }
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, WP_24C, BSP_IO_LEVEL_LOW);
    while (ok && (offset < length))
    {
        uint16_t i;
        uint16_t page_left = (uint16_t) (MAINTENANCE_EEPROM_PAGE_SIZE - (address % MAINTENANCE_EEPROM_PAGE_SIZE));
        uint16_t chunk = ((length - offset) < page_left) ? (uint16_t) (length - offset) : page_left; // 每次写入不越过页边界，避免 EEPROM 页内地址回卷。
        bool ready = false;
        ok = H_Maintenance_RecoverBus() && H_Maintenance_AddressWrite(address);
        for (i = 0; ok && (i < chunk); ++i) { ok = H_Maintenance_WriteByte(data[offset + i]); }
        if (!H_Maintenance_Stop()) { ok = false; }

        for (i = 0; ok && !ready && (i < 20U); ++i) // 停止条件后有限轮询应答，等待 24C256 内部写周期完成。
        {
            R_BSP_SoftwareDelay(500U, BSP_DELAY_UNITS_MICROSECONDS);
            ready = H_Maintenance_Start() && H_Maintenance_WriteByte((uint8_t) (MAINTENANCE_EEPROM_ADDRESS << 1));
            if (!H_Maintenance_Stop()) { ok = false; }
        }
        ok = ok && ready;
        offset = (uint16_t) (offset + chunk);
        address = (uint16_t) (address + chunk);
    }
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, WP_24C, BSP_IO_LEVEL_HIGH); // 无论通信成功与否，都恢复 EEPROM 写保护。
    return ok;
}

/*
 * 函数名：H_Maintenance_PortNow
 * 说明：读取由 SysTick 累计的毫秒计数
 * 输入：g_port：非空硬件状态指针，硬件运行期间地址保持有效
 * 输出：返回 32 位毫秒计数，计数自然回绕；无输出参数
 * 使用：供 F_Maintenance.c 跨文件调用，也用于本文件的串口超时判断
 */
uint32_t H_Maintenance_PortNow(const Maintenance_Port *g_port) { return g_port->milliseconds; }

/*
 * 函数名：SysTick_Handler
 * 说明：处理 1 毫秒时基中断，对已绑定的硬件上下文累加毫秒数
 * 输入：无，使用初始化时保存的硬件状态绑定
 * 输出：无返回值；已绑定上下文的 milliseconds 增加一，未绑定时不处理
 * 使用：由 MCU 中断向量跨文件引用，覆盖 BSP 弱定义；固定名称不可更改，模块独占 SysTick
 */
void SysTick_Handler(void)
{
    Maintenance_Port *g_port = g_binding.g_port;
    if (g_port != NULL) { ++g_port->milliseconds; }
}

/*
 * 函数名：DIS_Callback
 * 说明：处理串口接收、发送完成及错误事件，维护接收环形队列和状态标志
 * 输入：g_args：FSP 提供的非空串口事件参数，含事件类型和接收数据
 * 输出：无返回值；已绑定上下文：更新接收队列、发送忙标志及错误计数，未绑定时不处理
 * 使用：由生成的 hal_data.c 配置跨文件引用，在 FSP 串口中断回调中执行，不执行业务计算或 EEPROM 保存
 */
void DIS_Callback(uart_callback_args_t *g_args)
{
    Maintenance_Port *g_port = g_binding.g_port;
    if (g_port == NULL) { return; }
    if (g_args->event == UART_EVENT_RX_CHAR)
    {
        uint16_t next = (uint16_t) ((g_port->rx_head + 1U) % MAINTENANCE_RX_SIZE);
        if (next == g_port->rx_tail)
        {
            g_port->rx_fault = true;
            ++g_port->uart_error_count;
        }
        else
        {
            g_port->rx[g_port->rx_head] = (uint8_t) g_args->data;
            __DMB(); // 先写入数据再发布队头，保证主循环观察到完整字节。
            g_port->rx_head = next;
        }
    }
    else if (g_args->event == UART_EVENT_TX_COMPLETE) { g_port->tx_busy = false; }
    else if ((g_args->event == UART_EVENT_ERR_PARITY) || (g_args->event == UART_EVENT_ERR_FRAMING) ||
             (g_args->event == UART_EVENT_ERR_OVERFLOW) || (g_args->event == UART_EVENT_BREAK_DETECT))
    {
        g_port->rx_fault = true;
        ++g_port->uart_error_count;
    }
}

/*
 * 函数名：H_Maintenance_PortReadByte
 * 说明：从指定硬件上下文的接收环形队列取出一个字节
 * 输入：g_port：非空硬件状态指针，硬件运行期间地址保持有效
 * 输出：byte：成功时写入接收字节，指针必须非空；返回 true 表示取到数据，false 表示队列为空
 *       g_port：成功时推进 rx_tail，队列为空时不修改 byte
 * 使用：供 F_Maintenance.c 跨文件调用；主循环读取，中断负责写入队列
 */
bool H_Maintenance_PortReadByte(Maintenance_Port *g_port, uint8_t *byte)
{
    if (g_port->rx_tail == g_port->rx_head) { return false; }
    __DMB();
    *byte = g_port->rx[g_port->rx_tail];
    __DMB();
    g_port->rx_tail = (uint16_t) ((g_port->rx_tail + 1U) % MAINTENANCE_RX_SIZE); // 读取完成后再发布队尾，避免中断提前复用该槽位。
    return true;
}

/*
 * 函数名：H_Maintenance_PortRxFault
 * 说明：原子读取并清除接收故障标志，发生故障时丢弃未处理的接收数据
 * 输入：g_port：非空硬件状态指针，硬件运行期间地址保持有效
 * 输出：返回本次读取到的故障标志；g_port：有故障时清除 rx_fault 并将 rx_tail 对齐 rx_head
 * 使用：供 F_Maintenance.c 跨文件调用；主循环检查通信状态，短暂屏蔽中断后恢复进入前的屏蔽状态
 */
bool H_Maintenance_PortRxFault(Maintenance_Port *g_port)
{
    bool fault;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    fault = g_port->rx_fault;
    if (fault)
    {
        g_port->rx_tail = g_port->rx_head;
        g_port->rx_fault = false;
    }
    __set_PRIMASK(mask); // 恢复进入函数前的中断屏蔽状态，不能无条件开启中断。
    return fault;
}

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
bool H_Maintenance_PortSend(Maintenance_Port *g_port, const uint8_t *data, uint16_t length)
{
    fsp_err_t error;
    uint32_t now = H_Maintenance_PortNow(g_port);
    if (g_port->tx_busy)
    {
        if ((uint32_t) (now - g_port->tx_started_ms) > 100U)
        {
            if (R_SCI_UART_Abort(&DIS_UART_ctrl, UART_DIR_TX) == FSP_SUCCESS) // 驱动确认取消后才能释放持久发送缓冲区，当前调用仍返回忙。
            {
                g_port->tx_busy = false;
                g_port->rx_fault = true;
                ++g_port->uart_error_count;
            }
        }
        return false;
    }
    if ((data == NULL) || (length == 0U) || (length > sizeof(g_port->tx))) { return false; }
    memcpy(g_port->tx, data, length); // 异步发送使用持久缓冲区，函数返回后不再依赖调用方的原始数据。
    g_port->tx_started_ms = now;
    g_port->tx_busy = true;
    error = R_SCI_UART_Write(&DIS_UART_ctrl, g_port->tx, length);
    if (error != FSP_SUCCESS)
    {
        g_port->tx_busy = false;
        ++g_port->uart_error_count;
        return false;
    }
    return true;
}

/*
 * 函数名：H_Maintenance_PortInit
 * 说明：绑定中断上下文，配置软件 I²C、打开 SCI9、设置 1 毫秒 SysTick 并开启屏幕电源
 * 输入：g_port：非空硬件状态指针，硬件运行期间地址保持有效，首次调用前由功能层清零
 * 输出：返回 true 表示所有初始化步骤成功，false 表示外设配置失败；无数据输出参数，保存 g_port 供中断使用并更新硬件配置
 * 使用：供 F_Maintenance.c 跨文件调用；主循环首次任务初始化时执行，独占一组 SCI9/SysTick，不支持同时绑定多个实例
 */
bool H_Maintenance_PortInit(Maintenance_Port *g_port)
{
    g_binding.g_port = g_port; // 先绑定上下文，再开启可能触发中断的 UART 和 SysTick。
    const uint32_t open_drain = IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                               IOPORT_CFG_PORT_OUTPUT_HIGH | IOPORT_CFG_NMOS_ENABLE;
    if ((R_IOPORT_PinCfg(&g_ioport_ctrl, SCL_24C, open_drain) != FSP_SUCCESS) ||
        (R_IOPORT_PinCfg(&g_ioport_ctrl, SDA_24C, open_drain) != FSP_SUCCESS) ||
        (R_IOPORT_PinWrite(&g_ioport_ctrl, WP_24C, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS)) { return false; }
    if (R_SCI_UART_Open(&DIS_UART_ctrl, &DIS_UART_cfg) != FSP_SUCCESS) { return false; }
    SystemCoreClockUpdate();
    if (SysTick_Config(SystemCoreClock / 1000U) != 0U)
    {
        (void) R_SCI_UART_Close(&DIS_UART_ctrl);
        return false;
    }

    if ((R_IOPORT_PinWrite(&g_ioport_ctrl, DIS_POWER, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS) ||
        (R_IOPORT_PinWrite(&g_ioport_ctrl, DIS_EN, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS)) // U18 电源开关和 U16 关断控制均由高电平使能。
    {
        SysTick->CTRL = 0U;
        (void) R_SCI_UART_Close(&DIS_UART_ctrl);
        return false;
    }
    return true;
}
