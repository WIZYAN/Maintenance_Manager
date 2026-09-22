/* generated configuration header file - do not edit */
#ifndef BSP_PIN_CFG_H_
#define BSP_PIN_CFG_H_
#include "r_ioport.h"

/* Common macro for FSP header files. There is also a corresponding FSP_FOOTER macro at the end of this file. */
FSP_HEADER

#define DIS_TX (BSP_IO_PORT_01_PIN_09) /* DIS_TX */
#define DIS_RX (BSP_IO_PORT_01_PIN_10) /* DIS_RX */
#define DIS_POWER (BSP_IO_PORT_01_PIN_13) /* DIS_POWER */
#define DIS_EN (BSP_IO_PORT_01_PIN_14) /* DIS_EN */
#define WP_24C (BSP_IO_PORT_01_PIN_15) /* WP_24C */
#define SCL_24C (BSP_IO_PORT_06_PIN_08) /* SCL_24C */
#define SDA_24C (BSP_IO_PORT_06_PIN_09) /* SDA_24C */
extern const ioport_cfg_t g_bsp_pin_cfg; /* R7FA4M1AB3CFP.pincfg */

void BSP_PinConfigSecurityInit();

/* Common macro for FSP header files. There is also a corresponding FSP_HEADER macro at the top of this file. */
FSP_FOOTER

#endif /* BSP_PIN_CFG_H_ */
