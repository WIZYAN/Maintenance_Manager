#ifndef MAINTENANCE_CONFIG_H
#define MAINTENANCE_CONFIG_H

/* All application-specific settings are collected here. */
#define MAINTENANCE_DEFAULT_DAYS       180U
#define MAINTENANCE_MAX_DAYS           36500U
#define MAINTENANCE_SCREEN_BOOT_MS     2000U
#define MAINTENANCE_RTC_POLL_MS        1000U
#define MAINTENANCE_RTC_TIMEOUT_MS     500U
#define MAINTENANCE_RTC_FRESH_MS       3000U
#define MAINTENANCE_STORAGE_RETRY_MS   5000U
#define MAINTENANCE_FRAME_TIMEOUT_MS   200U
#define MAINTENANCE_DISPLAY_PERIOD_MS 1000U

/* 24C256: A0/A1/A2 are grounded. Reserve two separate 64-byte pages.
 * Do not allocate these addresses to other application settings. */
#define MAINTENANCE_EEPROM_ADDRESS     0x50U
#define MAINTENANCE_EEPROM_SLOT0       0x7F00U
#define MAINTENANCE_EEPROM_SLOT1       0x7F40U
#define MAINTENANCE_EEPROM_PAGE_SIZE   64U

/* The screen page has not been created yet. 0xFFFF disables an output.
 * Use TEXT controls for all three outputs. Set real IDs before enabling.
 * Screen protocol: Dacai configuration protocol, UART CRC DISABLED, 115200 8N1. */
#ifndef MAINTENANCE_SCREEN_ID
#define MAINTENANCE_SCREEN_ID          0xFFFFU
#endif
#ifndef MAINTENANCE_REMAIN_CONTROL_ID
#define MAINTENANCE_REMAIN_CONTROL_ID  0xFFFFU
#endif
#ifndef MAINTENANCE_PERIOD_CONTROL_ID
#define MAINTENANCE_PERIOD_CONTROL_ID  0xFFFFU
#endif
#ifndef MAINTENANCE_STATUS_CONTROL_ID
#define MAINTENANCE_STATUS_CONTROL_ID  0xFFFFU
#endif

#define MAINTENANCE_RX_SIZE            512U
#define MAINTENANCE_FRAME_SIZE         1024U
#define MAINTENANCE_TX_SIZE            64U

#endif
