/* generated HAL header file - do not edit */
#ifndef HAL_DATA_H_
#define HAL_DATA_H_
#include <stdint.h>
#include "bsp_api.h"
#include "common_data.h"
#include "r_sci_uart.h"
#include "r_uart_api.h"
FSP_HEADER
/** UART on SCI Instance. */
extern const uart_instance_t DIS_UART;

/** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
extern sci_uart_instance_ctrl_t DIS_UART_ctrl;
extern const uart_cfg_t DIS_UART_cfg;
extern const sci_uart_extended_cfg_t DIS_UART_cfg_extend;

#ifndef DIS_Callback
void DIS_Callback(uart_callback_args_t *p_args);
#endif
void hal_entry(void);
void g_hal_init(void);
FSP_FOOTER
#endif /* HAL_DATA_H_ */
