#ifndef F_MAINTENANCE_H
#define F_MAINTENANCE_H

#include "H_Maintenance.h"

typedef enum
{
    MAINTENANCE_OK = 0,
    MAINTENANCE_NOT_READY,
    MAINTENANCE_INVALID_ARGUMENT,
    MAINTENANCE_TIME_ERROR,
    MAINTENANCE_STORAGE_ERROR,
    MAINTENANCE_STORAGE_CORRUPT,
    MAINTENANCE_PORT_ERROR
} Maintenance_Result;

typedef enum
{
    MAINTENANCE_WAITING = 0,
    MAINTENANCE_RUNNING,
    MAINTENANCE_DUE,
    MAINTENANCE_RTC_FAULT,
    MAINTENANCE_SAVE_FAULT,
    MAINTENANCE_HARDWARE_FAULT
} Maintenance_Status;

typedef struct
{
    uint16_t year; // 年份范围为 2000～2099。
    uint8_t month, day, hour, minute, second;
} Maintenance_Date;

typedef struct
{
    uint16_t period_days;
    uint32_t start_day;
    uint16_t period_hours;
    uint32_t start_uptime_seconds;
} Maintenance_Item_Save;

typedef struct
{
    uint32_t due_day, elapsed_days;
    uint16_t remaining_days;
    uint32_t used_seconds, used_hours;
    uint8_t day_percent, hour_percent;
    uint8_t progress_percent;
    bool countdown_valid, hours_valid;
    Maintenance_Status status;
} Maintenance_Item_State;

typedef struct
{
    Maintenance_Item_Save machine, sensor;
    uint32_t saved_day; // 保存日期水位，用于检测日期回退。
    uint32_t sequence; // 循环记录序号，比较时允许正常回绕。
    uint32_t uptime_seconds; // 最近成功保存的累计开机秒数。
} Maintenance_Save; // 持久化参数只通过接口修改；EEPROM 使用显式字节布局，不依赖结构体内存布局。

typedef struct
{
    Maintenance_Date rtc;
    uint32_t current_day;
    Maintenance_Item_State machine, sensor;
    bool rtc_valid, record_valid, countdown_valid;
    Maintenance_Status status;
    Maintenance_Result last_result;
    uint32_t rtc_error_count;
    uint32_t uptime_seconds, uptime_last_ms;
    uint16_t uptime_remainder_ms;
    bool uptime_loaded;
    uint8_t loaded_version;

    volatile bool reset_all_request; // 调试器初始化后置位；每次开机默认关闭，任务只消费一次有效请求。
    Maintenance_Result reset_all_result; // 记录本次双项目复位结果；失败时不会在主循环反复复位。

    bool initialized, port_ready, screen_ready, storage_loaded, storage_blank; // 任务、存储和协议内部状态，外部不要直接修改。
    bool storage_fault, storage_corrupt, rtc_pending, have_rtc;
    bool migration_pending;
    int8_t active_slot;
    uint32_t started_ms, last_rtc_request_ms, last_rtc_ms;
    uint32_t storage_retry_ms, frame_last_ms, display_ms;
    uint32_t highest_day;
    uint16_t frame_length;
    uint8_t display_field;
    uint8_t frame[MAINTENANCE_FRAME_SIZE];
} Maintenance_State; // 运行与显示状态供主循环读取；中断共享数据单独保存在 Maintenance_Port。

typedef struct
{
    Maintenance_Save save; // 掉电保存参数。
    Maintenance_State state; // 过程变量及模块内部状态。
    Maintenance_Port port; // 串口与时基硬件状态。
} Maintenance_Context; // 调用方持有；首次任务前清零，硬件运行期间地址必须保持有效。

/*
 * 函数名：F_Maintenance_Task
 * 说明：执行维保模块初始化、计时、记录恢复、协议调度、复位和周期保存
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 * 输出：无返回值；g_context：更新保存参数、运行状态和硬件状态
 * 使用：仅供 A_Maintenance.c 跨文件持续调用；主循环运行，存储错误按间隔重试，硬件初始化失败时只更新故障状态
 */
void F_Maintenance_Task(Maintenance_Context *g_context);

/*
 * 函数名：F_Maintenance_SetPeriod
 * 说明：检查参数和运行条件，修改指定项目的天数或小时周期并保存
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       value：周期值；天数为 1～36500，小时为 1～65535
 *       sensor：true 选择传感器，false 选择整机
 *       hours：true 修改小时周期，false 修改天数周期
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；g_context：更新保存参数或最近操作结果
 * 使用：仅供 A_Maintenance.c 跨文件调用；主循环上下文，保留原起点，相同周期不重复写入
 */
Maintenance_Result F_Maintenance_SetPeriod(Maintenance_Context *g_context, uint16_t value, bool sensor, bool hours);

/*
 * 函数名：F_Maintenance_ResetItem
 * 说明：重启指定项目的本轮维保；无有效记录时初始化两个项目的起点
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       sensor：true 选择传感器，false 选择整机
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；g_context：更新保存参数或最近操作结果
 * 使用：仅供 A_Maintenance.c 跨文件调用；主循环确认保养完成时使用，要求有效 RTC 和已加载的存储状态
 */
Maintenance_Result F_Maintenance_ResetItem(Maintenance_Context *g_context, bool sensor);

#endif
