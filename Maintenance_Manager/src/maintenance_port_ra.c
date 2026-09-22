#include "hal_data.h"
#include "maintenance_internal.h"
#include <string.h>

/* Software I2C, nominal <=100 kHz; interrupts remain enabled throughout.
 * High on these open-drain pins releases the line to R55/R56 pull-ups. */
static void delay_half(void) { R_BSP_SoftwareDelay(5U, BSP_DELAY_UNITS_MICROSECONDS); }
static void scl(bool high)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, SCL_24C, high ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
}
static void sda(bool high)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, SDA_24C, high ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
}
static bool line_high(bsp_io_port_pin_t pin)
{
    bsp_io_level_t level = BSP_IO_LEVEL_LOW;
    return (R_IOPORT_PinRead(&g_ioport_ctrl, pin, &level) == FSP_SUCCESS) && (level == BSP_IO_LEVEL_HIGH);
}
static bool clock_high(void)
{
    uint16_t limit = 200U;
    scl(true);
    while (!line_high(SCL_24C))
    {
        if (limit-- == 0U) { return false; }
        delay_half();
    }
    delay_half();
    return true;
}
static bool stop(void)
{
    bool ok;
    scl(false); sda(false); delay_half();
    ok = clock_high();
    sda(true); delay_half();
    return ok && line_high(SDA_24C);
}
static bool start(void)
{
    sda(true); delay_half();
    if (!clock_high() || !line_high(SDA_24C)) { return false; }
    sda(false); delay_half(); scl(false); delay_half();
    return true;
}
static bool recover_bus(void)
{
    uint8_t i;
    sda(true);
    if (!clock_high()) { return false; }
    if (line_high(SDA_24C)) { return true; }
    for (i = 0; i < 9U; ++i)
    {
        scl(false); delay_half();
        if (!clock_high()) { return false; }
    }
    return stop();
}
static bool write_byte(uint8_t value)
{
    uint8_t i;
    bool ack;
    for (i = 0; i < 8U; ++i)
    {
        sda((value & 0x80U) != 0U); delay_half();
        if (!clock_high()) { return false; }
        scl(false); delay_half(); value = (uint8_t) (value << 1);
    }
    sda(true); delay_half();
    if (!clock_high()) { return false; }
    ack = !line_high(SDA_24C);
    scl(false); delay_half();
    return ack;
}
static bool read_byte(uint8_t *value, bool ack)
{
    uint8_t i, data = 0;
    sda(true);
    for (i = 0; i < 8U; ++i)
    {
        delay_half();
        if (!clock_high()) { return false; }
        data = (uint8_t) ((data << 1) | (line_high(SDA_24C) ? 1U : 0U));
        scl(false); delay_half();
    }
    sda(!ack); delay_half();
    if (!clock_high()) { return false; }
    scl(false); delay_half(); sda(true);
    *value = data;
    return true;
}
static bool address_write(uint16_t address)
{
    return start() && write_byte((uint8_t) (MAINTENANCE_EEPROM_ADDRESS << 1)) &&
           write_byte((uint8_t) (address >> 8)) && write_byte((uint8_t) address);
}

bool Maintenance_EepromRead(uint16_t address, uint8_t *data, uint16_t length)
{
    uint16_t i;
    bool ok;
    if ((data == NULL) || (length == 0U) || ((uint32_t) address + length > 32768U)) { return false; }
    ok = recover_bus() && address_write(address) && start() &&
         write_byte((uint8_t) ((MAINTENANCE_EEPROM_ADDRESS << 1) | 1U));
    for (i = 0; ok && (i < length); ++i) { ok = read_byte(&data[i], (i + 1U) < length); }
    if (!stop()) { ok = false; }
    return ok;
}

bool Maintenance_EepromWrite(uint16_t address, const uint8_t *data, uint16_t length)
{
    uint16_t offset = 0;
    bool ok = true;
    if ((data == NULL) || (length == 0U) || ((uint32_t) address + length > 32768U)) { return false; }
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, WP_24C, BSP_IO_LEVEL_LOW);
    while (ok && (offset < length))
    {
        uint16_t i;
        uint16_t page_left = (uint16_t) (MAINTENANCE_EEPROM_PAGE_SIZE - (address % MAINTENANCE_EEPROM_PAGE_SIZE));
        uint16_t chunk = ((length - offset) < page_left) ? (uint16_t) (length - offset) : page_left;
        bool ready = false;
        ok = recover_bus() && address_write(address);
        for (i = 0; ok && (i < chunk); ++i) { ok = write_byte(data[offset + i]); }
        if (!stop()) { ok = false; }
        /* Bounded ACK polling after STOP (24C256 write cycle). */
        for (i = 0; ok && !ready && (i < 20U); ++i)
        {
            R_BSP_SoftwareDelay(500U, BSP_DELAY_UNITS_MICROSECONDS);
            ready = start() && write_byte((uint8_t) (MAINTENANCE_EEPROM_ADDRESS << 1));
            if (!stop()) { ok = false; }
        }
        ok = ok && ready;
        offset = (uint16_t) (offset + chunk);
        address = (uint16_t) (address + chunk);
    }
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, WP_24C, BSP_IO_LEVEL_HIGH);
    return ok;
}

uint32_t Maintenance_PortNow(void) { return Maintenance_para.milliseconds; }

/* Strong handler replaces the weak BSP handler. SysTick is owned by this module. */
void SysTick_Handler(void);
void SysTick_Handler(void) { ++Maintenance_para.milliseconds; }

void DIS_Callback(uart_callback_args_t *args)
{
    if (args->event == UART_EVENT_RX_CHAR)
    {
        uint16_t next = (uint16_t) ((Maintenance_para.rx_head + 1U) % MAINTENANCE_RX_SIZE);
        if (next == Maintenance_para.rx_tail)
        {
            Maintenance_para.rx_fault = true;
            ++Maintenance_para.uart_error_count;
        }
        else
        {
            Maintenance_para.rx[Maintenance_para.rx_head] = (uint8_t) args->data;
            __DMB();
            Maintenance_para.rx_head = next;
        }
    }
    else if (args->event == UART_EVENT_TX_COMPLETE) { Maintenance_para.tx_busy = false; }
    else if ((args->event == UART_EVENT_ERR_PARITY) || (args->event == UART_EVENT_ERR_FRAMING) ||
             (args->event == UART_EVENT_ERR_OVERFLOW) || (args->event == UART_EVENT_BREAK_DETECT))
    {
        Maintenance_para.rx_fault = true;
        ++Maintenance_para.uart_error_count;
    }
}

bool Maintenance_PortReadByte(uint8_t *byte)
{
    if (Maintenance_para.rx_tail == Maintenance_para.rx_head) { return false; }
    __DMB();
    *byte = Maintenance_para.rx[Maintenance_para.rx_tail];
    __DMB();
    Maintenance_para.rx_tail = (uint16_t) ((Maintenance_para.rx_tail + 1U) % MAINTENANCE_RX_SIZE);
    return true;
}

bool Maintenance_PortRxFault(void)
{
    bool fault;
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    fault = Maintenance_para.rx_fault;
    if (fault)
    {
        Maintenance_para.rx_tail = Maintenance_para.rx_head;
        Maintenance_para.rx_fault = false;
    }
    __set_PRIMASK(mask);
    return fault;
}

bool Maintenance_PortSend(const uint8_t *data, uint16_t length)
{
    fsp_err_t error;
    uint32_t now = Maintenance_PortNow();
    if (Maintenance_para.tx_busy)
    {
        if ((uint32_t) (now - Maintenance_para.tx_started_ms) > 100U)
        {
            /* Cancel transfer before reusing the persistent TX buffer. */
            if (R_SCI_UART_Abort(&DIS_UART_ctrl, UART_DIR_TX) == FSP_SUCCESS)
            {
                Maintenance_para.tx_busy = false;
                Maintenance_para.rx_fault = true;
                ++Maintenance_para.uart_error_count;
            }
        }
        return false;
    }
    if ((data == NULL) || (length == 0U) || (length > sizeof(Maintenance_para.tx))) { return false; }
    memcpy(Maintenance_para.tx, data, length);
    Maintenance_para.tx_started_ms = now;
    Maintenance_para.tx_busy = true;
    error = R_SCI_UART_Write(&DIS_UART_ctrl, Maintenance_para.tx, length);
    if (error != FSP_SUCCESS)
    {
        Maintenance_para.tx_busy = false;
        ++Maintenance_para.uart_error_count;
        return false;
    }
    return true;
}

bool Maintenance_PortInit(void)
{
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
    /* U18 power switch and U16 /SHUTDOWN are both enabled by a high MCU output. */
    if ((R_IOPORT_PinWrite(&g_ioport_ctrl, DIS_POWER, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS) ||
        (R_IOPORT_PinWrite(&g_ioport_ctrl, DIS_EN, BSP_IO_LEVEL_HIGH) != FSP_SUCCESS))
    {
        SysTick->CTRL = 0U;
        (void) R_SCI_UART_Close(&DIS_UART_ctrl);
        return false;
    }
    return true;
}
