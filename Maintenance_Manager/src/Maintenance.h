#ifndef MAINTENANCE_H
#define MAINTENANCE_H

#include <stdbool.h>
#include <stdint.h>
#include "maintenance_config.h"

typedef enum
{
    MAINTENANCE_OK = 0,
    MAINTENANCE_NOT_READY,
    MAINTENANCE_INVALID_ARGUMENT,
    MAINTENANCE_TIME_ERROR,
    MAINTENANCE_STORAGE_ERROR,
    MAINTENANCE_STORAGE_CORRUPT,
    MAINTENANCE_PORT_ERROR
} Maintenance_result_t;

typedef enum
{
    MAINTENANCE_WAITING = 0,
    MAINTENANCE_RUNNING,
    MAINTENANCE_DUE,
    MAINTENANCE_RTC_FAULT,
    MAINTENANCE_SAVE_FAULT,
    MAINTENANCE_HARDWARE_FAULT
} Maintenance_status_t;

typedef struct
{
    uint16_t year;                     /* 2000..2099 */
    uint8_t month, day, hour, minute, second;
} Maintenance_date_t;

typedef struct
{
    uint16_t period_days;
    uint32_t start_day;
    uint16_t period_hours;
    uint32_t start_uptime_seconds;
} Maintenance_item_save_t;

typedef struct
{
    uint32_t due_day, elapsed_days;
    uint16_t remaining_days;
    uint32_t used_seconds, used_hours;
    uint8_t day_percent, hour_percent;
    uint8_t progress_percent;
    bool countdown_valid, hours_valid;
    Maintenance_status_t status;
} Maintenance_item_para_t;

/* 第一组：掉电保存的数据。由接口修改，禁止直接赋值绕过保存流程。
 * EEPROM uses an explicit byte format, NOT the raw struct layout. */
typedef struct
{
    Maintenance_item_save_t machine, sensor;
    uint32_t saved_day;                /* 最近设置时的日期，用于校时检查 */
    uint32_t sequence;                 /* 循环记录的序号 */
    uint32_t uptime_seconds;           /* 累计开机秒数，周期保存 */
} Maintenance_save_t;

/* 第二组：过程变量。主循环可读取显示字段；不要直接修改内部状态。
 * Only millisecond/RX/TX fields marked volatile are shared with ISRs. */
typedef struct
{
    Maintenance_date_t rtc;
    uint32_t current_day;
    Maintenance_item_para_t machine, sensor;
    bool rtc_valid, record_valid, countdown_valid;
    Maintenance_status_t status;
    Maintenance_result_t last_result;
    uint32_t rtc_error_count;
    uint32_t uptime_seconds, uptime_last_ms;
    uint16_t uptime_remainder_ms;
    bool uptime_loaded;
    uint8_t loaded_version;

    /* Debugger command: set to true AFTER initialization to restart both items.
     * Defaults to false on every boot. Task consumes one request and records
     * the result; it never retries a failed write in a tight loop. */
    volatile bool reset_all_request;
    Maintenance_result_t reset_all_result;

    /* Internal task/storage/protocol state, also kept in this group. */
    bool initialized, port_ready, screen_ready, storage_loaded, storage_blank;
    bool storage_fault, storage_corrupt, rtc_pending, have_rtc;
    bool migration_pending;
    int8_t active_slot;
    uint32_t started_ms, last_rtc_request_ms, last_rtc_ms;
    uint32_t storage_retry_ms, frame_last_ms, display_ms;
    uint32_t highest_day;
    uint16_t frame_length;
    uint8_t display_field;
    uint8_t frame[MAINTENANCE_FRAME_SIZE];
    uint8_t tx[MAINTENANCE_TX_SIZE];

    volatile uint32_t milliseconds;
    volatile uint16_t rx_head, rx_tail;
    volatile uint32_t uart_error_count;
    volatile bool rx_fault, tx_busy;
    uint32_t tx_started_ms;
    volatile uint8_t rx[MAINTENANCE_RX_SIZE];
} Maintenance_para_t;

extern Maintenance_save_t Maintenance_save;
extern Maintenance_para_t Maintenance_para;

/* Call continuously from the main loop. First call initializes hardware.
 * Owns SCI9 and SysTick (1 ms) in this bare-metal project.
 * No delay for RTC replies; EEPROM operations are bounded synchronous I/O. */
void Maintenance_Task(void);

/* Main-loop context only, never call from an ISR/UART callback.
 * Returns OK only after EEPROM readback succeeds. Needs a fresh valid RTC.
 * Preserves start_day; changing 180 -> 150 after 30 days leaves 120 days. */
Maintenance_result_t Maintenance_SetPeriodDays(uint16_t days);

/* Confirm completed maintenance: restart the calendar from today's RTC date
 * and restart this item's operating hours from the current uptime counter.
 * Also explicitly recovers corrupt records using the default period. */
Maintenance_result_t Maintenance_Reset(void);

/* Independent sensor interval and completed-maintenance confirmation.
 * The original SetPeriodDays/Reset functions apply to the machine. */
Maintenance_result_t Maintenance_SetSensorPeriodDays(uint16_t days);
Maintenance_result_t Maintenance_ResetSensor(void);
/* Hour limits: 1..65535, preserving the existing operating-time origin. */
Maintenance_result_t Maintenance_SetPeriodHours(uint16_t hours);
Maintenance_result_t Maintenance_SetSensorPeriodHours(uint16_t hours);

#endif
