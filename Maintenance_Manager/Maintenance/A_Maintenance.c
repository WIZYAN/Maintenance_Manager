#include "A_Maintenance.h"

static Maintenance_Context g_maintenance; // 模块私有实例，静态零初始化；所有 A 接口共用，内部通过指针向 F 层传参。

/*
 * 函数名：A_Maintenance_SetPeriodDays
 * 说明：设置模块内部整机项目的日历天数周期，保留现有起点；相同周期不重复写入
 * 输入：days：日历天数周期，范围 1～36500
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；主循环先持续调度 A_Maintenance_Task，需最近 3 秒内的有效 RTC，无需传入上下文
 */
Maintenance_Result A_Maintenance_SetPeriodDays(uint16_t days)
{
    return F_Maintenance_SetPeriod(&g_maintenance, days, false, false);
}

/*
 * 函数名：A_Maintenance_SetSensorPeriodDays
 * 说明：设置模块内部传感器项目的日历天数周期，保留现有起点；相同周期不重复写入
 * 输入：days：日历天数周期，范围 1～36500
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；主循环先持续调度 A_Maintenance_Task，需最近 3 秒内的有效 RTC，无需传入上下文
 */
Maintenance_Result A_Maintenance_SetSensorPeriodDays(uint16_t days)
{
    return F_Maintenance_SetPeriod(&g_maintenance, days, true, false);
}

/*
 * 函数名：A_Maintenance_SetPeriodHours
 * 说明：设置模块内部整机项目的开机小时周期，保留现有起点；相同周期不重复写入
 * 输入：hours：开机小时周期，范围 1～65535
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；主循环先持续调度 A_Maintenance_Task，需最近 3 秒内的有效 RTC，无需传入上下文
 */
Maintenance_Result A_Maintenance_SetPeriodHours(uint16_t hours)
{
    return F_Maintenance_SetPeriod(&g_maintenance, hours, false, true);
}

/*
 * 函数名：A_Maintenance_SetSensorPeriodHours
 * 说明：设置模块内部传感器项目的开机小时周期，保留现有起点；相同周期不重复写入
 * 输入：hours：开机小时周期，范围 1～65535
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；主循环先持续调度 A_Maintenance_Task，需最近 3 秒内的有效 RTC，无需传入上下文
 */
Maintenance_Result A_Maintenance_SetSensorPeriodHours(uint16_t hours)
{
    return F_Maintenance_SetPeriod(&g_maintenance, hours, true, true);
}

/*
 * 函数名：A_Maintenance_Reset
 * 说明：确认整机保养完成，重置该项目的日历和开机时间起点；记录损坏时以默认周期重建两项目
 * 输入：无
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；先持续调度 A_Maintenance_Task，在主循环保养确认事件中调用一次；需最近 3 秒内的有效 RTC，未初始化时返回 MAINTENANCE_NOT_READY
 */
Maintenance_Result A_Maintenance_Reset(void)
{
    return F_Maintenance_ResetItem(&g_maintenance, false);
}

/*
 * 函数名：A_Maintenance_ResetSensor
 * 说明：确认传感器保养完成，重置该项目的日历和开机时间起点；记录损坏时以默认周期重建两项目
 * 输入：无
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示就绪、时间或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；先持续调度 A_Maintenance_Task，在主循环保养确认事件中调用一次；需最近 3 秒内的有效 RTC，未初始化时返回 MAINTENANCE_NOT_READY
 */
Maintenance_Result A_Maintenance_ResetSensor(void)
{
    return F_Maintenance_ResetItem(&g_maintenance, true);
}

/*
 * 函数名：A_Maintenance_Task
 * 说明：持续调度模块内部实例，首次调用自动初始化，后续完成计时、屏幕通信和记录保存
 * 输入：无
 * 输出：无返回值；无输出参数，运行结果保存在模块内部
 * 使用：供 hal_entry.c 或其他应用模块跨文件调用；主循环持续执行，无需创建上下文或单独初始化，EEPROM 操作为有超时的同步读写
 */
void A_Maintenance_Task(void)
{
    F_Maintenance_Task(&g_maintenance);
}

/*
 * 函数名：A_Maintenance_SetRtc
 * 说明：保存校时事务并发起异步 RTC 校准，保留两个保养项目已累计的时间
 * 输入：g_date：2000～2099 年的合法目标日期时间
 * 输出：返回 MAINTENANCE_IN_PROGRESS 表示已受理，其他值表示参数、时间、就绪或存储错误；无输出参数
 * 使用：供其他应用模块跨文件调用；主循环继续调度直到查询结果结束，校时过程中禁止复位或修改周期
 */
Maintenance_Result A_Maintenance_SetRtc(const Maintenance_Date *g_date)
{
    return F_Maintenance_SetRtc(&g_maintenance, g_date);
}

/*
 * 函数名：A_Maintenance_GetRtcSetResult
 * 说明：读取最近一次已受理的异步校时结果
 * 输入：无
 * 输出：返回 MAINTENANCE_NOT_READY 表示尚未校时，MAINTENANCE_IN_PROGRESS 表示进行中，MAINTENANCE_OK 表示已回读并保存，其余值表示失败
 * 使用：供其他应用模块跨文件调用；拒绝受理的请求通过 SetRtc 的返回值判断
 */
Maintenance_Result A_Maintenance_GetRtcSetResult(void)
{
    return F_Maintenance_GetRtcSetResult(&g_maintenance);
}
