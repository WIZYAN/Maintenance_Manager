#include "A_Maintenance.h"

/*
 * 函数名：A_Maintenance_SetPeriodDays
 * 说明：设置整机维保周期，保留现有日历和开机时间起点；相同周期不重复写入
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       days：天数周期，范围 1～36500
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；g_context：更新保存参数或最近操作结果
 * 使用：供 hal_entry.c 或其他应用模块跨文件调用，仅限主循环；设置或复位前需有最近 3 秒内的有效 RTC
 */
Maintenance_Result A_Maintenance_SetPeriodDays(Maintenance_Context *g_context, uint16_t days)
{
    return F_Maintenance_SetPeriod(g_context, days, false, false);
}

/*
 * 函数名：A_Maintenance_SetSensorPeriodDays
 * 说明：设置传感器维保周期，保留现有日历和开机时间起点；相同周期不重复写入
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       days：天数周期，范围 1～36500
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；g_context：更新保存参数或最近操作结果
 * 使用：供 hal_entry.c 或其他应用模块跨文件调用，仅限主循环；设置或复位前需有最近 3 秒内的有效 RTC
 */
Maintenance_Result A_Maintenance_SetSensorPeriodDays(Maintenance_Context *g_context, uint16_t days)
{
    return F_Maintenance_SetPeriod(g_context, days, true, false);
}

/*
 * 函数名：A_Maintenance_SetPeriodHours
 * 说明：设置整机维保周期，保留现有日历和开机时间起点；相同周期不重复写入
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       hours：小时数周期，范围 1～65535
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；g_context：更新保存参数或最近操作结果
 * 使用：供 hal_entry.c 或其他应用模块跨文件调用，仅限主循环；设置或复位前需有最近 3 秒内的有效 RTC
 */
Maintenance_Result A_Maintenance_SetPeriodHours(Maintenance_Context *g_context, uint16_t hours)
{
    return F_Maintenance_SetPeriod(g_context, hours, false, true);
}

/*
 * 函数名：A_Maintenance_SetSensorPeriodHours
 * 说明：设置传感器维保周期，保留现有日历和开机时间起点；相同周期不重复写入
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       hours：小时数周期，范围 1～65535
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；g_context：更新保存参数或最近操作结果
 * 使用：供 hal_entry.c 或其他应用模块跨文件调用，仅限主循环；设置或复位前需有最近 3 秒内的有效 RTC
 */
Maintenance_Result A_Maintenance_SetSensorPeriodHours(Maintenance_Context *g_context, uint16_t hours)
{
    return F_Maintenance_SetPeriod(g_context, hours, true, true);
}

/*
 * 函数名：A_Maintenance_Reset
 * 说明：确认整机保养完成，重置该项目的日历和开机时间起点；记录损坏时以默认周期重建两项目
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；g_context：更新保存参数或最近操作结果
 * 使用：供 hal_entry.c 或其他应用模块跨文件调用，仅限主循环；设置或复位前需有最近 3 秒内的有效 RTC，由确认保养完成的事件触发
 */
Maintenance_Result A_Maintenance_Reset(Maintenance_Context *g_context)
{
    return F_Maintenance_ResetItem(g_context, false);
}

/*
 * 函数名：A_Maintenance_ResetSensor
 * 说明：确认传感器保养完成，重置该项目的日历和开机时间起点；记录损坏时以默认周期重建两项目
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；g_context：更新保存参数或最近操作结果
 * 使用：供 hal_entry.c 或其他应用模块跨文件调用，仅限主循环；设置或复位前需有最近 3 秒内的有效 RTC，由确认保养完成的事件触发
 */
Maintenance_Result A_Maintenance_ResetSensor(Maintenance_Context *g_context)
{
    return F_Maintenance_ResetItem(g_context, true);
}

/*
 * 函数名：A_Maintenance_Task
 * 说明：持续调度维保模块，首次调用初始化硬件，后续完成计时、屏幕通信和记录保存
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 * 输出：无返回值；g_context：更新保存参数、运行状态和硬件状态
 * 使用：由 hal_entry.c 或其他应用模块在主循环持续跨文件调用；不要在中断中调用，EEPROM 操作为有超时的同步读写
 */
void A_Maintenance_Task(Maintenance_Context *g_context)
{
    F_Maintenance_Task(g_context);
}
