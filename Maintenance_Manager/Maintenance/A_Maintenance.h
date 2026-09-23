#ifndef A_MAINTENANCE_H
#define A_MAINTENANCE_H

#include "F_Maintenance.h"

/*
 * 函数名：A_Maintenance_Task
 * 说明：持续调度模块内部实例，首次调用自动初始化，后续完成计时、屏幕通信和记录保存
 * 输入：无
 * 输出：无返回值；无输出参数，运行结果保存在模块内部
 * 使用：供 hal_entry.c 或其他应用模块跨文件调用；主循环持续执行，无需创建上下文或单独初始化，EEPROM 操作为有超时的同步读写
 */
void A_Maintenance_Task(void);

/*
 * 函数名：A_Maintenance_SetPeriodDays
 * 说明：设置模块内部整机项目的日历天数周期，保留现有起点；相同周期不重复写入
 * 输入：days：日历天数周期，范围 1～36500
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；主循环先持续调度 A_Maintenance_Task，需最近 3 秒内的有效 RTC，无需传入上下文
 */
Maintenance_Result A_Maintenance_SetPeriodDays(uint16_t days);

/*
 * 函数名：A_Maintenance_SetSensorPeriodDays
 * 说明：设置模块内部传感器项目的日历天数周期，保留现有起点；相同周期不重复写入
 * 输入：days：日历天数周期，范围 1～36500
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；主循环先持续调度 A_Maintenance_Task，需最近 3 秒内的有效 RTC，无需传入上下文
 */
Maintenance_Result A_Maintenance_SetSensorPeriodDays(uint16_t days);

/*
 * 函数名：A_Maintenance_SetPeriodHours
 * 说明：设置模块内部整机项目的开机小时周期，保留现有起点；相同周期不重复写入
 * 输入：hours：开机小时周期，范围 1～65535
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；主循环先持续调度 A_Maintenance_Task，需最近 3 秒内的有效 RTC，无需传入上下文
 */
Maintenance_Result A_Maintenance_SetPeriodHours(uint16_t hours);

/*
 * 函数名：A_Maintenance_SetSensorPeriodHours
 * 说明：设置模块内部传感器项目的开机小时周期，保留现有起点；相同周期不重复写入
 * 输入：hours：开机小时周期，范围 1～65535
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；主循环先持续调度 A_Maintenance_Task，需最近 3 秒内的有效 RTC，无需传入上下文
 */
Maintenance_Result A_Maintenance_SetSensorPeriodHours(uint16_t hours);

/*
 * 函数名：A_Maintenance_Reset
 * 说明：确认整机保养完成，重置该项目的日历和开机时间起点；记录损坏时以默认周期重建两项目
 * 输入：无
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；先持续调度 A_Maintenance_Task，在主循环保养确认事件中调用一次；需最近 3 秒内的有效 RTC，未初始化时返回 MAINTENANCE_NOT_READY
 */
Maintenance_Result A_Maintenance_Reset(void);

/*
 * 函数名：A_Maintenance_ResetSensor
 * 说明：确认传感器保养完成，重置该项目的日历和开机时间起点；记录损坏时以默认周期重建两项目
 * 输入：无
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；先持续调度 A_Maintenance_Task，在主循环保养确认事件中调用一次；需最近 3 秒内的有效 RTC，未初始化时返回 MAINTENANCE_NOT_READY
 */
Maintenance_Result A_Maintenance_ResetSensor(void);

#endif
