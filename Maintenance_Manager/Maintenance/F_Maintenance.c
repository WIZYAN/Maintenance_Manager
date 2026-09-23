#include "F_Maintenance.h"
#include <stddef.h>
#include <string.h>

// 下列为本文件内部辅助函数声明；跨文件的功能层入口在头文件声明。
static bool F_Maintenance_DateToDay(const Maintenance_Date *g_date, uint32_t *day);
static void F_Maintenance_RejectRtc(Maintenance_Context *g_context);
static void F_Maintenance_AcceptRtc(Maintenance_Context *g_context, const Maintenance_Date *g_date, uint32_t now);
static uint32_t F_Maintenance_SaturatingAdd(uint32_t a, uint32_t b);
static void F_Maintenance_UpdateUptime(Maintenance_Context *g_context, uint32_t now);
static void F_Maintenance_UpdateHours(Maintenance_Context *g_context, const Maintenance_Item_Save *g_save, Maintenance_Item_State *g_para);
static void F_Maintenance_UpdateItem(Maintenance_Context *g_context, const Maintenance_Item_Save *g_save, Maintenance_Item_State *g_para);
static void F_Maintenance_UpdateStatus(Maintenance_Context *g_context, uint32_t now);
static Maintenance_Result F_Maintenance_TimeReady(Maintenance_Context *g_context);
static Maintenance_Result F_Maintenance_Commit(Maintenance_Context *g_context, Maintenance_Save *g_candidate);
static void F_Maintenance_ServiceResetRequest(Maintenance_Context *g_context);
static uint16_t F_Maintenance_SlotAddress(uint8_t slot);
static uint16_t F_Maintenance_Get16(const uint8_t *p);
static void F_Maintenance_Put16(uint8_t *p, uint16_t v);
static void F_Maintenance_Put32(uint8_t *p, uint32_t v);
static uint32_t F_Maintenance_Get32(const uint8_t *p);
static uint32_t F_Maintenance_Crc32(const uint8_t *data, uint16_t length);
static bool F_Maintenance_Erased(const uint8_t *page);
static bool F_Maintenance_Decode(const uint8_t *page, Maintenance_Save *g_save);
static bool F_Maintenance_StorageLoad(Maintenance_Context *g_context);
static bool F_Maintenance_StorageCommit(Maintenance_Context *g_context, Maintenance_Save *g_candidate);
static bool F_Maintenance_Bcd(uint8_t raw, uint8_t *value);
static void F_Maintenance_ReceiveFrame(Maintenance_Context *g_context, uint32_t now);
static void F_Maintenance_ReceiveBytes(Maintenance_Context *g_context, uint32_t now);
static bool F_Maintenance_SendText(Maintenance_Context *g_context, uint16_t control, const char *text);
static void F_Maintenance_Decimal(uint32_t number, char *text);
static bool F_Maintenance_SendProgress(Maintenance_Context *g_context, uint16_t control, uint8_t percent);
static bool F_Maintenance_SendBarColor(Maintenance_Context *g_context, uint16_t control, uint16_t rgb565);
static const char *F_Maintenance_StatusText(const Maintenance_Item_State *g_para);
static void F_Maintenance_DisplayTask(Maintenance_Context *g_context, uint32_t now);
static void F_Maintenance_ProtocolTask(Maintenance_Context *g_context, uint32_t now);
static void F_Maintenance_CalibrationTask(Maintenance_Context *g_context, uint32_t now);
static void F_Maintenance_CalibrationInput(Maintenance_Context *g_context);

/*
 * 函数名：F_Maintenance_DateToDay
 * 说明：校验日期时间并换算为从 2000-01-01 起算的日期序号
 * 输入：g_date：待校验的日期时间，年份范围 2000～2099
 * 输出：day：成功时写入从零开始的日期序号；返回 true 表示有效，false 表示空指针或日期时间非法，失败不修改 day
 * 使用：仅在本 .c 文件内部调用，用于接收 RTC 日期后的合法性检查
 */
static bool F_Maintenance_DateToDay(const Maintenance_Date *g_date, uint32_t *day)
{
    static const uint8_t month_days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t total = 0;
    uint16_t year;
    uint8_t month, limit;
    if ((g_date == NULL) || (day == NULL) || (g_date->year < 2000U) ||
        (g_date->year > 2099U) || (g_date->month < 1U) || (g_date->month > 12U) ||
        (g_date->hour > 23U) || (g_date->minute > 59U) || (g_date->second > 59U))
    {
        return false;
    }
    limit = month_days[g_date->month - 1U];
    if ((g_date->month == 2U) && ((g_date->year % 4U) == 0U)) { ++limit; } // 在限定的 2000～2099 年范围内，年份能被 4 整除即可判断闰年。
    if ((g_date->day == 0U) || (g_date->day > limit)) { return false; }
    for (year = 2000U; year < g_date->year; ++year)
    {
        total += ((year % 4U) == 0U) ? 366U : 365U;
    }
    for (month = 1U; month < g_date->month; ++month)
    {
        total += month_days[month - 1U];
        if ((month == 2U) && ((g_date->year % 4U) == 0U)) { ++total; }
    }
    *day = total + g_date->day - 1U;
    return true;
}

/*
 * 函数名：F_Maintenance_RejectRtc
 * 说明：将本次 RTC 结果标为无效，结束等待并累计时间错误次数
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 * 输出：无返回值；g_context：清除 rtc_valid、rtc_pending，并增加 rtc_error_count
 * 使用：仅在本 .c 文件内部调用，用于帧错误、接收故障或 RTC 超时
 */
static void F_Maintenance_RejectRtc(Maintenance_Context *g_context)
{
    g_context->state.rtc_valid = false;
    g_context->state.rtc_pending = false;
    ++g_context->state.rtc_error_count;
}

/*
 * 函数名：F_Maintenance_AcceptRtc
 * 说明：接收 RTC 秒数，正常走时累计差值，受控校时仅累计操作期间实际经过的时间
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       g_date：解析出的 RTC 日期时间
 *       now：本次调度的毫秒计数，允许无符号回绕
 * 输出：无返回值；g_context：成功时更新时间和有效标志，失败时记录 RTC 错误
 * 使用：仅在本 .c 文件内部调用，用于完整 RTC 回包处理
 */
static void F_Maintenance_AcceptRtc(Maintenance_Context *g_context, const Maintenance_Date *g_date, uint32_t now)
{
    uint32_t day, seconds;
    Maintenance_State *g_state = &g_context->state;
    if (!F_Maintenance_DateToDay(g_date, &day)) { F_Maintenance_RejectRtc(g_context); return; }
    seconds = day * 86400U + (uint32_t) g_date->hour * 3600U + (uint32_t) g_date->minute * 60U + g_date->second;
    if (g_state->calibration_phase == 1U) { return; } // 已保存校时意图后不再接受旧查询的回包。
    if (g_state->calibration_phase == 2U)
    {
        uint32_t passed = (now - g_state->calibration_sent_ms) / 1000U;
        uint32_t expected = F_Maintenance_SaturatingAdd(g_context->save.rtc_target_seconds, passed);
        uint32_t difference = (seconds >= expected) ? seconds - expected : expected - seconds;
        uint32_t delta = now - g_state->calibration_base_ms;
        uint32_t fraction = delta % 1000U + g_state->calibration_remainder_ms;
        if (difference > 2U) { return; } // 忽略校时前滞留回包，目标回读超时由任务统一处理。
        g_state->calendar_seconds = F_Maintenance_SaturatingAdd(g_state->calibration_base_seconds,
            delta / 1000U + fraction / 1000U);
        g_state->calibration_remainder_ms = (uint16_t) (fraction % 1000U);
        g_state->calendar_ready = true;
        g_state->calibration_phase = 3U; // RTC 已确认，只有新锚点成功落盘后才报告校时成功。
    }
    else if (g_context->save.calibration_pending)
    {
        g_state->calendar_ready = false; // 校时中断后无法区分断电前后的时域，不猜测断电时长。
        g_state->calendar_seconds = g_context->save.calendar_seconds;
    }
    else if (g_state->calendar_ready && !g_state->migration_pending)
    {
        if (seconds < g_state->rtc_seconds) { F_Maintenance_RejectRtc(g_context); return; }
        g_state->calendar_seconds = F_Maintenance_SaturatingAdd(g_state->calendar_seconds, seconds - g_state->rtc_seconds);
    }
    else if (g_state->record_valid && !g_state->migration_pending)
    {
        if (seconds < g_context->save.rtc_anchor_seconds) { F_Maintenance_RejectRtc(g_context); return; }
        g_state->calendar_seconds = F_Maintenance_SaturatingAdd(g_context->save.calendar_seconds,
            seconds - g_context->save.rtc_anchor_seconds); // 普通重启使用成对锚点补计断电期间实际经过的秒数。
        g_state->calendar_ready = true;
    }
    else
    {
        if (g_state->migration_pending && (day < g_context->save.rtc_anchor_seconds))
        { F_Maintenance_RejectRtc(g_context); return; }
        g_state->calendar_seconds = seconds;
        g_state->calendar_ready = true;
    }
    g_state->rtc = *g_date;
    g_state->current_day = day;
    g_state->rtc_seconds = seconds;
    g_state->have_rtc = true;
    g_state->rtc_valid = true;
    g_state->rtc_pending = false;
    g_state->last_rtc_ms = now;
}

/*
 * 函数名：F_Maintenance_SaturatingAdd
 * 说明：执行无符号饱和加法，避免累计开机秒数溢出回绕
 * 输入：a：当前累计值
 *       b：本次增加值
 * 输出：返回相加结果，超过 uint32_t 上限时返回 UINT32_MAX；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于开机时间累计和上电恢复
 */
static uint32_t F_Maintenance_SaturatingAdd(uint32_t a, uint32_t b)
{
    return (b > UINT32_MAX - a) ? UINT32_MAX : a + b; // 达到计数上限后饱和，避免回绕造成开机时间减少。
}

/*
 * 函数名：F_Maintenance_UpdateUptime
 * 说明：根据毫秒差值累计开机秒数，并保留不足一秒的余数
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       now：本次调度的毫秒计数，允许无符号回绕
 * 输出：无返回值；g_context：更新 uptime_last_ms、uptime_remainder_ms 和 uptime_seconds
 * 使用：仅在本 .c 文件内部调用，在任务调度及设置前更新时间
 */
static void F_Maintenance_UpdateUptime(Maintenance_Context *g_context, uint32_t now)
{
    uint32_t delta = now - g_context->state.uptime_last_ms; // 无符号差值允许毫秒计数正常回绕。
    uint32_t fraction = g_context->state.uptime_remainder_ms + delta % 1000U;
    g_context->state.uptime_last_ms = now;
    g_context->state.uptime_remainder_ms = (uint16_t) (fraction % 1000U); // 不足一秒的余数保留到下一轮累计。
    g_context->state.uptime_seconds = F_Maintenance_SaturatingAdd(g_context->state.uptime_seconds,
        delta / 1000U + fraction / 1000U);
}

/*
 * 函数名：F_Maintenance_UpdateHours
 * 说明：检查存储状态，计算单项目已用开机时间及小时进度
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       g_save：该项目已保存的周期和开机时间起点
 *       g_para：该项目的运行状态指针
 * 输出：无返回值；g_para：更新小时有效标志、使用秒数和小时数、小时百分比，并初始化总进度
 * 使用：仅在本 .c 文件内部调用，在更新日历进度前调用；使用数值不封顶，百分比最大为 100
 */
static void F_Maintenance_UpdateHours(Maintenance_Context *g_context, const Maintenance_Item_Save *g_save, Maintenance_Item_State *g_para)
{
    g_para->hours_valid = g_context->state.uptime_loaded && g_context->state.record_valid &&
        !g_context->state.storage_fault && !g_context->state.storage_corrupt && !g_context->state.migration_pending;
    g_para->day_percent = 0;
    g_para->hour_percent = 0;
    if (g_para->hours_valid)
    {
        uint32_t limit = (uint32_t) g_save->period_hours * 3600U;
        g_para->used_seconds = g_context->state.uptime_seconds - g_save->start_uptime_seconds;
        g_para->used_hours = g_para->used_seconds / 3600U;
        g_para->hour_percent = (g_para->used_seconds >= limit) ? 100U :
            (uint8_t) (((uint64_t) g_para->used_seconds * 100U) / limit); // 先扩展为 64 位再乘，实际小时数不封顶，仅百分比限制为 100。
    }
    g_para->progress_percent = g_para->hour_percent;
}

/*
 * 函数名：F_Maintenance_UpdateItem
 * 说明：按累计秒数计算单项目日历进度，并取日历与开机时间进度中的较大值
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       g_save：该项目已保存的日历周期与起点
 *       g_para：已计算小时进度的项目运行状态
 * 输出：无返回值；g_para：更新累计时间轴到期日序号、已过与剩余天数、总进度和项目状态
 * 使用：仅在本 .c 文件内部调用，仅在累计时间和记录有效且无待确认校时事务时调用
 */
static void F_Maintenance_UpdateItem(Maintenance_Context *g_context, const Maintenance_Item_Save *g_save, Maintenance_Item_State *g_para)
{
    uint32_t elapsed = g_context->state.calendar_seconds - g_save->start_calendar_seconds;
    uint32_t limit = (uint32_t) g_save->period_days * 86400U;
    g_para->due_day = g_save->start_calendar_seconds / 86400U + g_save->period_days; // 累计时间轴上的日序号，不再表示屏幕上的公历到期日期。
    g_para->elapsed_days = elapsed / 86400U; // 满 24 小时才增加一天，跨午夜不足 24 小时仍为零天。
    g_para->remaining_days = (g_para->elapsed_days >= g_save->period_days) ? 0U :
        (uint16_t) (g_save->period_days - g_para->elapsed_days);
    g_para->day_percent = (elapsed >= limit) ? 100U : (uint8_t) (((uint64_t) elapsed * 100U) / limit);
    g_para->progress_percent = (g_para->day_percent > g_para->hour_percent) ? g_para->day_percent : g_para->hour_percent;
    g_para->countdown_valid = true;
    g_para->status = (g_para->progress_percent == 100U) ? MAINTENANCE_DUE : MAINTENANCE_RUNNING;
}

/*
 * 函数名：F_Maintenance_UpdateStatus
 * 说明：汇总两项目维保状态，按硬件、存储、时间和记录有效性更新显示结果
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       now：本次调度的毫秒计数，允许无符号回绕
 * 输出：无返回值；g_context：更新两项目进度、有效标志及整机汇总状态，过旧的 RTC 被标为无效
 * 使用：仅在本 .c 文件内部调用，在任务调度、RTC 处理和参数保存后刷新状态
 */
static void F_Maintenance_UpdateStatus(Maintenance_Context *g_context, uint32_t now)
{
    F_Maintenance_UpdateHours(g_context, &g_context->save.machine, &g_context->state.machine);
    F_Maintenance_UpdateHours(g_context, &g_context->save.sensor, &g_context->state.sensor);
    g_context->state.countdown_valid = false;
    g_context->state.machine.countdown_valid = false;
    g_context->state.sensor.countdown_valid = false;
    if (!g_context->state.port_ready)
    {
        g_context->state.status = MAINTENANCE_HARDWARE_FAULT;
    }
    else if (g_context->state.storage_fault || g_context->state.storage_corrupt)
    {
        g_context->state.status = MAINTENANCE_SAVE_FAULT;
    }
    else if (!g_context->state.rtc_valid ||
             ((uint32_t) (now - g_context->state.last_rtc_ms) > MAINTENANCE_RTC_FRESH_MS))
    {
        g_context->state.rtc_valid = false;
        g_context->state.status = g_context->state.have_rtc ? MAINTENANCE_RTC_FAULT : MAINTENANCE_WAITING;
        if (g_context->state.rtc_error_count != 0U) { g_context->state.status = MAINTENANCE_RTC_FAULT; }
    }
    else if (!g_context->state.record_valid || g_context->state.migration_pending)
    {
        g_context->state.status = MAINTENANCE_WAITING;
    }
    else if (!g_context->state.calendar_ready || g_context->save.calibration_pending ||
             (g_context->state.calendar_seconds < g_context->save.machine.start_calendar_seconds) ||
             (g_context->state.calendar_seconds < g_context->save.sensor.start_calendar_seconds))
    {
        g_context->state.status = MAINTENANCE_RTC_FAULT;
    }
    else
    {
        F_Maintenance_UpdateItem(g_context, &g_context->save.machine, &g_context->state.machine);
        F_Maintenance_UpdateItem(g_context, &g_context->save.sensor, &g_context->state.sensor);
        g_context->state.countdown_valid = true;
        g_context->state.status = ((g_context->state.machine.status == MAINTENANCE_DUE) ||
            (g_context->state.sensor.status == MAINTENANCE_DUE)) ? MAINTENANCE_DUE : MAINTENANCE_RUNNING;
    }
    if (!g_context->state.countdown_valid)
    {
        g_context->state.machine.status = g_context->state.status;
        g_context->state.sensor.status = g_context->state.status;
    }
}

/*
 * 函数名：F_Maintenance_TimeReady
 * 说明：检查初始化和 RTC 新鲜度，同时补计开机时间并刷新状态
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 * 输出：返回 MAINTENANCE_OK、MAINTENANCE_NOT_READY 或 MAINTENANCE_TIME_ERROR；g_context：更新计时和状态
 * 使用：仅在本 .c 文件内部调用，在修改周期或确认保养完成前检查前置条件
 */
static Maintenance_Result F_Maintenance_TimeReady(Maintenance_Context *g_context)
{
    uint32_t now;
    if (!g_context->state.initialized || !g_context->state.port_ready) { return MAINTENANCE_NOT_READY; }
    now = H_Maintenance_PortNow(&g_context->port);
    F_Maintenance_UpdateUptime(g_context, now);
    if ((uint32_t) (now - g_context->state.last_rtc_ms) > MAINTENANCE_RTC_FRESH_MS)
    {
        g_context->state.rtc_valid = false;
    }
    F_Maintenance_UpdateStatus(g_context, now);
    if (g_context->save.calibration_pending || (g_context->state.calibration_phase != 0U)) { return MAINTENANCE_NOT_READY; }
    return g_context->state.rtc_valid && g_context->state.calendar_ready ? MAINTENANCE_OK : MAINTENANCE_TIME_ERROR;
}

/*
 * 函数名：F_Maintenance_Commit
 * 说明：补齐候选记录的开机时间及成对的 RTC 和累计时间锚点，写入 EEPROM 并更新运行状态
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       g_candidate：准备保存的候选记录指针
 * 输出：返回 MAINTENANCE_OK 或 MAINTENANCE_STORAGE_ERROR；g_candidate：更新累计秒数、配对锚点及成功提交的序号
 *       g_context：成功时采用新记录并触发显示刷新，失败时标记存储故障
 * 使用：仅在本 .c 文件内部调用，用于周期修改、复位、记录初始化、迁移和分钟保存
 */
static Maintenance_Result F_Maintenance_Commit(Maintenance_Context *g_context, Maintenance_Save *g_candidate)
{
    g_candidate->uptime_seconds = g_context->state.uptime_seconds;

    if (g_context->state.rtc_valid && g_context->state.calendar_ready && (g_context->state.calibration_phase != 2U))
    {
        g_candidate->rtc_anchor_seconds = g_context->state.rtc_seconds;
        g_candidate->calendar_seconds = g_context->state.calendar_seconds;
    } // 原始 RTC 和累计秒数必须成对保存；RTC 异常时只更新开机小时计数。
    if (!F_Maintenance_StorageCommit(g_context, g_candidate))
    {
        g_context->state.storage_fault = true;
        g_context->state.last_result = MAINTENANCE_STORAGE_ERROR;
    }
    else
    {
        g_context->save = *g_candidate;
        g_context->state.record_valid = true;
        g_context->state.uptime_loaded = true;
        g_context->state.loaded_version = 4U;
        g_context->state.storage_loaded = true;
        g_context->state.storage_blank = false;
        g_context->state.storage_fault = false;
        g_context->state.storage_corrupt = false;
        g_context->state.migration_pending = false;
        g_context->state.display_field = 0;
        g_context->state.display_ms = H_Maintenance_PortNow(&g_context->port) - MAINTENANCE_DISPLAY_PERIOD_MS;
        g_context->state.last_result = MAINTENANCE_OK;
    }
    F_Maintenance_UpdateStatus(g_context, H_Maintenance_PortNow(&g_context->port));
    return g_context->state.last_result;
}

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
Maintenance_Result F_Maintenance_SetPeriod(Maintenance_Context *g_context, uint16_t value, bool sensor, bool hours)
{
    Maintenance_Save g_candidate;
    Maintenance_Result result;
    if ((value == 0U) || (!hours && (value > MAINTENANCE_MAX_DAYS))) { result = MAINTENANCE_INVALID_ARGUMENT; }
    else if ((result = F_Maintenance_TimeReady(g_context)) != MAINTENANCE_OK) { } // 保留就绪检查返回的具体错误，交由函数末尾记录和返回。
    else if (g_context->state.storage_corrupt) { result = MAINTENANCE_STORAGE_CORRUPT; }
    else if (g_context->state.storage_fault) { result = MAINTENANCE_STORAGE_ERROR; }
    else if (!g_context->state.record_valid || g_context->state.migration_pending) { result = MAINTENANCE_NOT_READY; }
    else if (value == (hours ? (sensor ? g_context->save.sensor.period_hours : g_context->save.machine.period_hours) :
        (sensor ? g_context->save.sensor.period_days : g_context->save.machine.period_days)))
    { result = MAINTENANCE_OK; } // 周期没有变化时直接成功，避免重复写入 EEPROM。
    else
    {
        Maintenance_Item_Save *g_item;
        g_candidate = g_context->save;
        g_item = sensor ? &g_candidate.sensor : &g_candidate.machine;
        if (hours) { g_item->period_hours = value; }
        else { g_item->period_days = value; }
        return F_Maintenance_Commit(g_context, &g_candidate);
    }
    g_context->state.last_result = result;
    return result;
}

/*
 * 函数名：F_Maintenance_ResetItem
 * 说明：重启指定项目的本轮维保；无有效记录时初始化两个项目的起点
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       sensor：true 选择传感器，false 选择整机
 * 输出：返回 Maintenance_Result：MAINTENANCE_OK 表示成功，其余值表示参数、就绪、时间或存储错误；g_context：更新保存参数或最近操作结果
 * 使用：仅供 A_Maintenance.c 跨文件调用；主循环确认保养完成时使用，要求有效 RTC 和已加载的存储状态
 */
Maintenance_Result F_Maintenance_ResetItem(Maintenance_Context *g_context, bool sensor)
{
    Maintenance_Save g_candidate;
    Maintenance_Result result = F_Maintenance_TimeReady(g_context);
    if (result == MAINTENANCE_OK)
    {
        if (!g_context->state.storage_loaded || g_context->state.storage_fault)
        {
            result = MAINTENANCE_STORAGE_ERROR;
        }
        else if (g_context->state.migration_pending) { result = MAINTENANCE_NOT_READY; }
        else
        {
            g_candidate = g_context->save;

            if (!g_context->state.record_valid)
            {
                g_candidate.machine.start_calendar_seconds = g_context->state.calendar_seconds;
                g_candidate.sensor.start_calendar_seconds = g_context->state.calendar_seconds;
                g_candidate.machine.start_uptime_seconds = g_context->state.uptime_seconds;
                g_candidate.sensor.start_uptime_seconds = g_context->state.uptime_seconds; // 无有效记录时同时重建两个项目的起点。
            }
            else
            {
                Maintenance_Item_Save *g_item = sensor ? &g_candidate.sensor : &g_candidate.machine;
                g_item->start_calendar_seconds = g_context->state.calendar_seconds;
                g_item->start_uptime_seconds = g_context->state.uptime_seconds;
            }
            return F_Maintenance_Commit(g_context, &g_candidate);
        }
    }
    g_context->state.last_result = result;
    return result;
}

/*
 * 函数名：F_Maintenance_ServiceResetRequest
 * 说明：处理调试器发起的双项目一次性复位请求
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 * 输出：无返回值；g_context：条件不足时保留请求，满足条件时清除请求并记录 reset_all_result，成功后更新两项目起点
 * 使用：仅在本 .c 文件内部调用，在本轮已刷新 RTC 有效性后执行；写入失败不自动重复复位
 */
static void F_Maintenance_ServiceResetRequest(Maintenance_Context *g_context)
{
    Maintenance_Save g_candidate;
    if (!g_context->state.reset_all_request) { return; }

    if (!g_context->state.rtc_valid || !g_context->state.storage_loaded ||
        g_context->state.storage_fault || g_context->state.migration_pending ||
        !g_context->state.calendar_ready || g_context->save.calibration_pending) { return; } // 本轮已检查 RTC 新鲜度；条件不足时保留调试请求，暂不复位。
    g_candidate = g_context->save;
    g_candidate.machine.start_calendar_seconds = g_context->state.calendar_seconds;
    g_candidate.sensor.start_calendar_seconds = g_context->state.calendar_seconds;
    g_candidate.machine.start_uptime_seconds = g_context->state.uptime_seconds;
    g_candidate.sensor.start_uptime_seconds = g_context->state.uptime_seconds;

    g_context->state.reset_all_request = false; // 先消费请求再写入，失败后必须重新显式请求，避免循环重试复位。
    g_context->state.reset_all_result = F_Maintenance_Commit(g_context, &g_candidate); // 两项目起点在同一记录中提交，保留原有周期。
}

/*
 * 函数名：F_Maintenance_Task
 * 说明：执行维保模块初始化、计时、记录恢复、协议调度、复位和周期保存
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 * 输出：无返回值；g_context：更新保存参数、运行状态和硬件状态
 * 使用：仅供 A_Maintenance.c 跨文件持续调用；主循环运行，存储错误按间隔重试，硬件初始化失败时只更新故障状态
 */
void F_Maintenance_Task(Maintenance_Context *g_context)
{
    uint32_t now;
    if (!g_context->state.initialized)
    {
        memset(&g_context->state, 0, sizeof(g_context->state));
        memset(&g_context->port, 0, sizeof(g_context->port));
        memset(&g_context->save, 0, sizeof(g_context->save));
        g_context->save.machine.period_days = MAINTENANCE_DEFAULT_DAYS;
        g_context->save.sensor.period_days = MAINTENANCE_SENSOR_DEFAULT_DAYS;
        g_context->save.machine.period_hours = MAINTENANCE_DEFAULT_HOURS;
        g_context->save.sensor.period_hours = MAINTENANCE_SENSOR_DEFAULT_HOURS;
        g_context->state.reset_all_result = MAINTENANCE_NOT_READY;
        g_context->state.calibration_result = MAINTENANCE_NOT_READY;
        g_context->state.active_slot = -1;
        g_context->state.initialized = true;
        g_context->state.port_ready = H_Maintenance_PortInit(&g_context->port);
        g_context->state.started_ms = H_Maintenance_PortNow(&g_context->port);
        g_context->state.uptime_last_ms = g_context->state.started_ms;
        g_context->state.storage_retry_ms = g_context->state.started_ms - MAINTENANCE_STORAGE_RETRY_MS;
        if (!g_context->state.port_ready) { g_context->state.last_result = MAINTENANCE_PORT_ERROR; }
    }
    now = H_Maintenance_PortNow(&g_context->port);
    F_Maintenance_UpdateUptime(g_context, now);
    if (!g_context->state.port_ready) { F_Maintenance_UpdateStatus(g_context, now); return; }
    if ((!g_context->state.storage_loaded || g_context->state.storage_fault) &&
        ((uint32_t) (now - g_context->state.storage_retry_ms) >= MAINTENANCE_STORAGE_RETRY_MS))
    {
        g_context->state.storage_retry_ms = now;
        if (F_Maintenance_StorageLoad(g_context))
        {
            if (!g_context->state.uptime_loaded)
            {
                g_context->state.calendar_ready = false;
                g_context->state.rtc_valid = false;
                g_context->state.uptime_seconds = F_Maintenance_SaturatingAdd(g_context->save.uptime_seconds,
                    g_context->state.uptime_seconds);
                g_context->state.uptime_loaded = true;
            } // 仅首次加载叠加本次启动已运行的秒数，重读记录时不重复累计。
            else if (g_context->save.uptime_seconds > g_context->state.uptime_seconds)
            { g_context->state.uptime_seconds = g_context->save.uptime_seconds; }
        }
    }
    F_Maintenance_ProtocolTask(g_context, now);
    F_Maintenance_UpdateStatus(g_context, now);
    if (((g_context->state.storage_blank && !g_context->state.record_valid) || g_context->state.migration_pending) &&
        !g_context->state.storage_fault && g_context->state.rtc_valid &&
        ((uint32_t) (now - g_context->state.last_rtc_ms) <= MAINTENANCE_RTC_FRESH_MS))
    {
        Maintenance_Save g_candidate = g_context->save;
        if (g_context->state.migration_pending)
        {
            g_candidate.machine.start_calendar_seconds = g_context->state.calendar_seconds -
                (g_context->state.current_day - g_candidate.machine.start_calendar_seconds) * 86400U;
            g_candidate.sensor.start_calendar_seconds = (g_candidate.sensor.start_calendar_seconds == UINT32_MAX) ?
                g_context->state.calendar_seconds : g_context->state.calendar_seconds -
                (g_context->state.current_day - g_candidate.sensor.start_calendar_seconds) * 86400U;
        } // 旧记录仅有日期，保留原有整天数；升级后才精确累计不足一天的部分。
        else
        {
            g_candidate.machine.start_calendar_seconds = g_context->state.calendar_seconds;
            g_candidate.sensor.start_calendar_seconds = g_context->state.calendar_seconds;
        }
        (void) F_Maintenance_Commit(g_context, &g_candidate);
    }
    F_Maintenance_CalibrationTask(g_context, H_Maintenance_PortNow(&g_context->port));
    F_Maintenance_ServiceResetRequest(g_context);
    if (g_context->state.record_valid && g_context->state.uptime_loaded && !g_context->state.migration_pending &&
        !g_context->state.storage_fault && !g_context->state.storage_corrupt &&
        (g_context->state.calibration_phase == 0U) &&
        (g_context->state.uptime_seconds - g_context->save.uptime_seconds >= MAINTENANCE_SAVE_INTERVAL_SECONDS))
    {
        Maintenance_Save g_candidate = g_context->save;
        (void) F_Maintenance_Commit(g_context, &g_candidate);
    }
}

// EEPROM 循环日志使用显式的小端字节布局，不直接写入结构体内存。
// 旧版 v1/v2 有效数据为 32 字节，提交标记位于页内第 63 字节：
// 偏移 0 为标识 MNT1，4 为 16 位版本，6 为 16 位长度，8 为 32 位序号；
// 12 为整机起始日期，16 为保存日期，20 为整机天数周期；
// v2 的 22 为传感器起始日期，26 为传感器天数周期，28 为前 28 字节的 CRC32；
// v1 的 22～27 保留为零，等待有效 RTC 后迁移传感器起点。
// v3 保留前 28 字节布局，28 为累计开机秒数，32/36 为两项目开机秒数起点；
// 40/42 为两项目小时周期，44 为前 44 字节的 CRC32；提交标记位置不变。
// v4 的 12/22 改为累计日历秒数起点，16 为原始 RTC 秒数锚点；
// 44 为累计日历秒数，48 为校时目标，52 为待确认标志，53～55 保留为零，56 为前 56 字节 CRC32。
// 逻辑槽 0/1 保留旧地址映射，兼容历史记录。
#define RECORD_SIZE 60U
#define COMMIT_OFFSET 63U
#define COMMIT_MARKER 0xA5U

#if (MAINTENANCE_EEPROM_SLOT_COUNT < 2U) || (MAINTENANCE_EEPROM_SLOT_COUNT > 127U) || \
    ((MAINTENANCE_EEPROM_JOURNAL_BASE % 64U) != 0U) || \
    ((MAINTENANCE_EEPROM_JOURNAL_BASE + MAINTENANCE_EEPROM_SLOT_COUNT * 64U) > 32768U) || \
    (MAINTENANCE_EEPROM_SLOT0 < MAINTENANCE_EEPROM_JOURNAL_BASE) || \
    (MAINTENANCE_EEPROM_SLOT1 != MAINTENANCE_EEPROM_SLOT0 + 64U) || \
    (MAINTENANCE_EEPROM_SLOT1 >= MAINTENANCE_EEPROM_JOURNAL_BASE + MAINTENANCE_EEPROM_SLOT_COUNT * 64U)
#error Invalid EEPROM journal layout
#endif

/*
 * 函数名：F_Maintenance_SlotAddress
 * 说明：将日志逻辑槽号转换为 EEPROM 页起始地址，保留旧版槽地址映射
 * 输入：slot：逻辑槽号，范围为零至 MAINTENANCE_EEPROM_SLOT_COUNT 减一
 * 输出：返回对应 EEPROM 页地址；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于循环记录扫描和写入
 */
static uint16_t F_Maintenance_SlotAddress(uint8_t slot)
{
    uint16_t offset = (uint16_t) ((MAINTENANCE_EEPROM_SLOT0 - MAINTENANCE_EEPROM_JOURNAL_BASE) / 64U);
    return (uint16_t) (MAINTENANCE_EEPROM_JOURNAL_BASE +
        ((offset + slot) % MAINTENANCE_EEPROM_SLOT_COUNT) * 64U);
}

/*
 * 函数名：F_Maintenance_Get16
 * 说明：从小端字节序列读取 16 位无符号整数
 * 输入：p：可读缓冲区，至少有 2 字节
 * 输出：返回转换后的 16 位数值；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于 EEPROM 记录解码
 */
static uint16_t F_Maintenance_Get16(const uint8_t *p) { return (uint16_t) (p[0] | ((uint16_t) p[1] << 8)); }

/*
 * 函数名：F_Maintenance_Put16
 * 说明：将 16 位无符号整数编码为小端字节序列
 * 输入：p：可写缓冲区，至少有 2 字节
 *       v：待编码的 16 位数值
 * 输出：无返回值；p：写入 2 字节小端数据
 * 使用：仅在本 .c 文件内部调用，用于 EEPROM 记录编码
 */
static void F_Maintenance_Put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }

/*
 * 函数名：F_Maintenance_Put32
 * 说明：将 32 位无符号整数编码为小端字节序列
 * 输入：p：可写缓冲区，至少有 4 字节
 *       v：待编码的 32 位数值
 * 输出：无返回值；p：写入 4 字节小端数据
 * 使用：仅在本 .c 文件内部调用，用于 EEPROM 记录编码
 */
static void F_Maintenance_Put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}

/*
 * 函数名：F_Maintenance_Get32
 * 说明：从小端字节序列读取 32 位无符号整数
 * 输入：p：可读缓冲区，至少有 4 字节
 * 输出：返回转换后的 32 位数值；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于 EEPROM 记录解码
 */
static uint32_t F_Maintenance_Get32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/*
 * 函数名：F_Maintenance_Crc32
 * 说明：使用反射多项式 0xEDB88320 计算 CRC32，初值全一，结果取反
 * 输入：data：待校验的数据缓冲区
 *       length：参与校验的字节数
 * 输出：返回 32 位校验值；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于 EEPROM 记录完整性校验
 */
static uint32_t F_Maintenance_Crc32(const uint8_t *data, uint16_t length)
{
    uint32_t crc = UINT32_MAX;
    uint16_t i;
    uint8_t bit;
    for (i = 0; i < length; ++i)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8U; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0U);
        }
    }
    return ~crc;
}

/*
 * 函数名：F_Maintenance_Erased
 * 说明：检查一整页 EEPROM 数据是否全部为擦除值 0xFF
 * 输入：page：至少包含 MAINTENANCE_EEPROM_PAGE_SIZE 字节的页缓冲区
 * 输出：返回 true 表示整页为空白，false 表示存在非空白字节；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于区分空白设备与损坏记录
 */
static bool F_Maintenance_Erased(const uint8_t *page)
{
    uint16_t i;
    for (i = 0; i < MAINTENANCE_EEPROM_PAGE_SIZE; ++i)
    {
        if (page[i] != 0xFFU) { return false; }
    }
    return true;
}

/*
 * 函数名：F_Maintenance_Decode
 * 说明：校验提交标记、版本、长度、CRC 和参数范围，解析 v1/v2/v3/v4 记录
 * 输入：page：包含整页数据的可读缓冲区
 *       g_save：解码结果缓冲区
 * 输出：返回 true 表示有效记录，false 表示校验失败；g_save：有效时写入记录，失败时可能已被部分修改，不可使用
 * 使用：仅在本 .c 文件内部调用，用于启动扫描与提交后回读校验；旧版记录补默认小时周期和零开机秒数
 */
static bool F_Maintenance_Decode(const uint8_t *page, Maintenance_Save *g_save)
{
    uint8_t i;
    uint16_t length = (page[4] == 4U) ? RECORD_SIZE : ((page[4] == 3U) ? 48U : 32U);
    uint32_t ceiling;
    memset(g_save, 0, sizeof(*g_save));
    if ((page[COMMIT_OFFSET] != COMMIT_MARKER) || (memcmp(page, "MNT1", 4) != 0) ||
        (page[4] < 1U) || (page[4] > 4U) || (page[5] != 0U) || (F_Maintenance_Get16(page + 6) != length) ||
        (F_Maintenance_Get32(page + length - 4U) != F_Maintenance_Crc32(page, (uint16_t) (length - 4U)))) { return false; }
    g_save->uptime_seconds = 0;
    g_save->machine.start_uptime_seconds = 0;
    g_save->sensor.start_uptime_seconds = 0;
    g_save->machine.period_hours = MAINTENANCE_DEFAULT_HOURS;
    g_save->sensor.period_hours = MAINTENANCE_SENSOR_DEFAULT_HOURS;
    if (page[4] >= 3U)
    {
        g_save->uptime_seconds = F_Maintenance_Get32(page + 28);
        g_save->machine.start_uptime_seconds = F_Maintenance_Get32(page + 32);
        g_save->sensor.start_uptime_seconds = F_Maintenance_Get32(page + 36);
        g_save->machine.period_hours = F_Maintenance_Get16(page + 40);
        g_save->sensor.period_hours = F_Maintenance_Get16(page + 42);
        if ((g_save->machine.start_uptime_seconds > g_save->uptime_seconds) ||
            (g_save->sensor.start_uptime_seconds > g_save->uptime_seconds) ||
            (g_save->machine.period_hours == 0U) || (g_save->sensor.period_hours == 0U)) { return false; }
    }
    ceiling = F_Maintenance_Get32(page + 16);
    if (page[4] == 4U)
    {
        g_save->calendar_seconds = F_Maintenance_Get32(page + 44);
        g_save->rtc_target_seconds = F_Maintenance_Get32(page + 48);
        g_save->calibration_pending = page[52] != 0U;
        if ((page[52] > 1U) || (page[53] != 0U) || (page[54] != 0U) || (page[55] != 0U) ||
            (ceiling >= 3155760000UL) || (g_save->rtc_target_seconds >= 3155760000UL) ||
            (!g_save->calibration_pending && (g_save->rtc_target_seconds != 0U))) { return false; }
        ceiling = g_save->calendar_seconds;
    }
    else if (ceiling >= 36525U) { return false; }
    if (page[4] == 1U)
    {
        for (i = 22U; i < 28U; ++i) { if (page[i] != 0U) { return false; } }
        g_save->sensor.start_calendar_seconds = UINT32_MAX;
        g_save->sensor.period_days = MAINTENANCE_SENSOR_DEFAULT_DAYS;
    }
    else
    {
        g_save->sensor.start_calendar_seconds = F_Maintenance_Get32(&page[22]);
        g_save->sensor.period_days = (uint16_t) ((uint16_t) page[26] | ((uint16_t) page[27] << 8));
        if ((g_save->sensor.period_days == 0U) || (g_save->sensor.period_days > MAINTENANCE_MAX_DAYS) ||
            (g_save->sensor.start_calendar_seconds > ceiling)) { return false; }
    }
    g_save->sequence = F_Maintenance_Get32(&page[8]);
    g_save->machine.start_calendar_seconds = F_Maintenance_Get32(&page[12]);
    g_save->rtc_anchor_seconds = F_Maintenance_Get32(&page[16]);
    g_save->machine.period_days = (uint16_t) ((uint16_t) page[20] | ((uint16_t) page[21] << 8));
    return (g_save->machine.period_days > 0U) && (g_save->machine.period_days <= MAINTENANCE_MAX_DAYS) &&
           (g_save->machine.start_calendar_seconds <= ceiling);
}

/*
 * 函数名：F_Maintenance_StorageLoad
 * 说明：逐页扫描循环日志并加载最新完整记录，区分空白、损坏和读取失败
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 * 输出：返回 true 表示读取到有效记录或整个区域为空白，false 表示读取失败或记录损坏
 *       g_context：更新保存参数、活动槽、版本、迁移标志和存储状态
 * 使用：仅在本 .c 文件内部调用，用于上电加载和存储故障后的定期重试
 */
static bool F_Maintenance_StorageLoad(Maintenance_Context *g_context)
{
    uint8_t page[MAINTENANCE_EEPROM_PAGE_SIZE]; // 扫描时每次仅读取一页，控制 MCU 栈占用。
    Maintenance_Save g_record, g_best;
    int8_t best_slot = -1;
    uint8_t slot, version = 0;
    bool all_erased = true;

    for (slot = 0; slot < MAINTENANCE_EEPROM_SLOT_COUNT; ++slot)
    {
        if (!H_Maintenance_EepromRead(F_Maintenance_SlotAddress(slot), page, sizeof(page)))
        {
            g_context->state.storage_fault = true;
            g_context->state.last_result = MAINTENANCE_STORAGE_ERROR;
            return false;
        }
        if (!F_Maintenance_Erased(page)) { all_erased = false; }
        if (F_Maintenance_Decode(page, &g_record) && ((best_slot < 0) ||
            (((uint32_t) (g_record.sequence - g_best.sequence) != 0U) &&
             ((uint32_t) (g_record.sequence - g_best.sequence) < 0x80000000UL)))) // 使用半区间序号比较，兼容记录序号回绕。
        {
            g_best = g_record;
            best_slot = (int8_t) slot;
            version = page[4];
        }
    }
    g_context->state.storage_loaded = true;
    g_context->state.storage_fault = false;
    g_context->state.storage_corrupt = false;
    g_context->state.storage_blank = false;
    g_context->state.record_valid = false;
    g_context->state.migration_pending = false;
    g_context->state.active_slot = best_slot;
    g_context->state.loaded_version = version;
    if (best_slot >= 0)
    {
        g_context->save = g_best;
        if (g_best.calibration_pending && (g_context->state.calibration_phase == 0U))
        {
            g_context->state.calendar_ready = false;
            g_context->state.calibration_result = MAINTENANCE_TIME_ERROR;
            g_context->state.calibration_notice = true;
        }
        g_context->state.migration_pending = (version < 4U);
        g_context->state.record_valid = true;
        g_context->state.last_result = MAINTENANCE_OK;
        return true;
    }
    memset(&g_context->save, 0, sizeof(g_context->save));
    g_context->save.machine.period_days = MAINTENANCE_DEFAULT_DAYS;
    g_context->save.sensor.period_days = MAINTENANCE_SENSOR_DEFAULT_DAYS;
    g_context->save.machine.period_hours = MAINTENANCE_DEFAULT_HOURS;
    g_context->save.sensor.period_hours = MAINTENANCE_SENSOR_DEFAULT_HOURS;
    if (all_erased)
    {
        g_context->state.storage_blank = true;
        g_context->state.last_result = MAINTENANCE_NOT_READY;
        return true;
    }
    g_context->state.storage_corrupt = true;
    g_context->state.last_result = MAINTENANCE_STORAGE_CORRUPT;
    return false;
}

/*
 * 函数名：F_Maintenance_StorageCommit
 * 说明：将候选记录写入下一日志页，按失效、写入校验、提交校验的顺序保存
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       g_candidate：准备写入的 v4 参数记录，累计时间和配对锚点已补齐
 * 输出：返回 true 表示最终回读校验成功，false 表示写入或校验失败
 *       g_candidate：成功时更新序号；g_context：成功时更新活动槽，失败时 EEPROM 可能已有完整新记录
 * 使用：仅在本 .c 文件内部调用，由统一保存流程调用，不覆盖当前有效页
 */
static bool F_Maintenance_StorageCommit(Maintenance_Context *g_context, Maintenance_Save *g_candidate)
{
    uint8_t g_record[RECORD_SIZE] = {0};
    uint8_t verify[MAINTENANCE_EEPROM_PAGE_SIZE];
    Maintenance_Save g_decoded;
    uint8_t marker = 0U;
    uint8_t read_marker;
    int8_t next_slot = (int8_t) ((g_context->state.active_slot + 1) % MAINTENANCE_EEPROM_SLOT_COUNT);
    uint16_t address = F_Maintenance_SlotAddress((uint8_t) next_slot); // 选择下一页写入，保留当前有效页作为掉电恢复依据。
    uint32_t sequence = g_context->state.record_valid ? g_context->save.sequence + 1U : 1U;
    memcpy(g_record, "MNT1", 4);
    g_record[4] = 4U;
    g_record[6] = RECORD_SIZE;
    F_Maintenance_Put32(&g_record[8], sequence);
    F_Maintenance_Put32(&g_record[12], g_candidate->machine.start_calendar_seconds);
    F_Maintenance_Put32(&g_record[16], g_candidate->rtc_anchor_seconds);
    g_record[20] = (uint8_t) g_candidate->machine.period_days;
    g_record[21] = (uint8_t) (g_candidate->machine.period_days >> 8);
    F_Maintenance_Put32(&g_record[22], g_candidate->sensor.start_calendar_seconds);
    g_record[26] = (uint8_t) g_candidate->sensor.period_days;
    g_record[27] = (uint8_t) (g_candidate->sensor.period_days >> 8);
    F_Maintenance_Put32(g_record + 28, g_candidate->uptime_seconds);
    F_Maintenance_Put32(g_record + 32, g_candidate->machine.start_uptime_seconds);
    F_Maintenance_Put32(g_record + 36, g_candidate->sensor.start_uptime_seconds);
    F_Maintenance_Put16(g_record + 40, g_candidate->machine.period_hours);
    F_Maintenance_Put16(g_record + 42, g_candidate->sensor.period_hours);
    F_Maintenance_Put32(g_record + 44, g_candidate->calendar_seconds);
    F_Maintenance_Put32(g_record + 48, g_candidate->rtc_target_seconds);
    g_record[52] = g_candidate->calibration_pending ? 1U : 0U;
    F_Maintenance_Put32(g_record + 56, F_Maintenance_Crc32(g_record, 56U));

    if (!H_Maintenance_EepromWrite((uint16_t) (address + COMMIT_OFFSET), &marker, 1U) ||
        !H_Maintenance_EepromRead((uint16_t) (address + COMMIT_OFFSET), &read_marker, 1U) ||
        (read_marker != 0U) ||
        !H_Maintenance_EepromWrite(address, g_record, sizeof(g_record)) ||
        !H_Maintenance_EepromRead(address, verify, sizeof(g_record)) ||
        (memcmp(g_record, verify, sizeof(g_record)) != 0)) { return false; } // 先使目标页失效并确认，再写入数据并回读，任何一步失败都不提交。
    marker = COMMIT_MARKER; // 有效载荷校验通过后，最后写入提交标记。
    if (!H_Maintenance_EepromWrite((uint16_t) (address + COMMIT_OFFSET), &marker, 1U) ||
        !H_Maintenance_EepromRead(address, verify, sizeof(verify)) ||
        !F_Maintenance_Decode(verify, &g_decoded) || (memcmp(g_record, verify, sizeof(g_record)) != 0)) { return false; } // 最终回读失败时新记录可能已提交，后续重载以完整页为准。
    g_candidate->sequence = sequence;
    g_context->state.active_slot = next_slot;
    return true;
}

// 屏幕采用大彩 V5.1 协议，屏端必须关闭协议 CRC 校验。
// RTC 查询帧：EE 82 FF FC FF FF。
// RTC 回包：EE F7 年 月 星期 日 时 分 秒 FF FC FF FF，日期时间字段采用 BCD 编码。
static const uint8_t tail[4] = {0xFF, 0xFC, 0xFF, 0xFF};
static const uint8_t rtc_request[6] = {0xEE, 0x82, 0xFF, 0xFC, 0xFF, 0xFF};

/*
 * 函数名：F_Maintenance_Bcd
 * 说明：检查两位 BCD 编码并转换为十进制数值
 * 输入：raw：包含两位 BCD 数字的字节
 * 输出：value：成功时写入 0～99 的数值；返回 true 表示转换成功，false 表示非法 BCD，失败不修改 value
 * 使用：仅在本 .c 文件内部调用，用于 RTC 回包字段解析
 */
static bool F_Maintenance_Bcd(uint8_t raw, uint8_t *value)
{
    if (((raw & 0x0FU) > 9U) || ((raw >> 4) > 9U)) { return false; }
    *value = (uint8_t) ((raw >> 4) * 10U + (raw & 0x0FU));
    return true;
}

/*
 * 函数名：F_Maintenance_ReceiveFrame
 * 说明：处理完整 RTC 回包或校时文本通知，校验字段后提交时间
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       now：本次调度的毫秒计数，允许无符号回绕
 * 输出：无返回值；g_context：更新 RTC 数据或错误状态，其他帧及无待处理查询的 RTC 帧被忽略
 * 使用：仅在本 .c 文件内部调用，接收缓冲区已匹配帧尾后调用
 */
static void F_Maintenance_ReceiveFrame(Maintenance_Context *g_context, uint32_t now)
{
    const uint8_t *f = g_context->state.frame;
    Maintenance_Date g_date;
    uint8_t year, week;
    if ((g_context->state.frame_length >= 8U) && (f[1] == 0xB1U) && (f[2] == 0x11U))
    {
        F_Maintenance_CalibrationInput(g_context);
        return;
    }
    if ((g_context->state.frame_length < 2U) || (f[1] != 0xF7U) ||
        !g_context->state.rtc_pending) { return; }
    if ((g_context->state.frame_length != 13U) ||
        !F_Maintenance_Bcd(f[2], &year) || !F_Maintenance_Bcd(f[3], &g_date.month) || !F_Maintenance_Bcd(f[4], &week) ||
        !F_Maintenance_Bcd(f[5], &g_date.day) || !F_Maintenance_Bcd(f[6], &g_date.hour) ||
        !F_Maintenance_Bcd(f[7], &g_date.minute) || !F_Maintenance_Bcd(f[8], &g_date.second) || (week > 6U))
    {
        F_Maintenance_RejectRtc(g_context);
        return;
    }
    g_date.year = (uint16_t) (2000U + year);
    F_Maintenance_AcceptRtc(g_context, &g_date, now);
}

/*
 * 函数名：F_Maintenance_ReceiveBytes
 * 说明：按单轮字节预算读取串口数据，处理接收故障、帧超时和帧尾匹配
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       now：本次调度的毫秒计数，允许无符号回绕
 * 输出：无返回值；g_context：消耗接收队列，更新组帧缓冲区、时间戳和 RTC 状态
 * 使用：仅在本 .c 文件内部调用，由协议任务每轮调用，避免持续收包占用整个主循环
 */
static void F_Maintenance_ReceiveBytes(Maintenance_Context *g_context, uint32_t now)
{
    uint8_t byte;
    uint16_t budget = MAINTENANCE_RX_SIZE; // 限制单轮接收量，持续数据流不会一直占用主循环。
    if (H_Maintenance_PortRxFault(&g_context->port))
    {
        g_context->state.frame_length = 0;
        F_Maintenance_RejectRtc(g_context);
    }
    if ((g_context->state.frame_length != 0U) &&
        ((uint32_t) (now - g_context->state.frame_last_ms) > MAINTENANCE_FRAME_TIMEOUT_MS))
    {
        g_context->state.frame_length = 0;
    }
    while ((budget-- != 0U) && H_Maintenance_PortReadByte(&g_context->port, &byte))
    {
        if (g_context->state.frame_length == 0U)
        {
            if (byte != 0xEEU) { continue; }
        }
        if (g_context->state.frame_length >= MAINTENANCE_FRAME_SIZE)
        {
            g_context->state.frame_length = 0;
            F_Maintenance_RejectRtc(g_context);
            if (byte != 0xEEU) { continue; }
        }
        g_context->state.frame[g_context->state.frame_length++] = byte;
        g_context->state.frame_last_ms = now;
        if ((g_context->state.frame_length >= 6U) &&
            (memcmp(&g_context->state.frame[g_context->state.frame_length - 4U], tail, 4) == 0))
        {
            if (H_Maintenance_PortRxFault(&g_context->port)) { F_Maintenance_RejectRtc(g_context); }
            else { F_Maintenance_ReceiveFrame(g_context, now); }
            g_context->state.frame_length = 0;
        }
    }
}

/*
 * 函数名：F_Maintenance_SendText
 * 说明：构造并尝试发送指定控件的文本更新帧
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       control：控件编号，0xFFFF 表示禁用该控件输出
 *       text：以零结尾的字符串，中文必须已编码为 GBK
 * 输出：返回 true 表示已受理发送或输出已禁用，false 表示文本过长或暂时无法发送；g_context：发送时更新硬件发送状态
 * 使用：仅在本 .c 文件内部调用，用于刷新日历、小时比值和状态文本
 */
static bool F_Maintenance_SendText(Maintenance_Context *g_context, uint16_t control, const char *text)
{
    uint8_t frame[MAINTENANCE_TX_SIZE];
    size_t length = strlen(text);
    if ((MAINTENANCE_SCREEN_ID == 0xFFFFU) || (control == 0xFFFFU)) { return true; }
    if (length > (sizeof(frame) - 11U)) { return false; }
    frame[0] = 0xEE; frame[1] = 0xB1; frame[2] = 0x10;
    frame[3] = (uint8_t) (MAINTENANCE_SCREEN_ID >> 8);
    frame[4] = (uint8_t) MAINTENANCE_SCREEN_ID;
    frame[5] = (uint8_t) (control >> 8); frame[6] = (uint8_t) control;
    memcpy(&frame[7], text, length);
    memcpy(&frame[7U + length], tail, sizeof(tail));
    return H_Maintenance_PortSend(&g_context->port, frame, (uint16_t) (length + 11U));
}

/*
 * 函数名：F_Maintenance_Decimal
 * 说明：将无符号整数转换为无前导零的十进制字符串
 * 输入：number：待转换的 32 位无符号数
 *       text：至少 11 字节的可写字符缓冲区
 * 输出：无返回值；text：写入以零结尾的十进制字符串
 * 使用：仅在本 .c 文件内部调用，用于屏幕显示数值格式化
 */
static void F_Maintenance_Decimal(uint32_t number, char *text)
{
    char reverse[10];
    uint8_t count = 0, index = 0;
    do
    {
        reverse[count++] = (char) ('0' + number % 10U);
        number /= 10U;
    } while (number != 0U);
    while (count != 0U) { text[index++] = reverse[--count]; }
    text[index] = '\0';
}

/*
 * 函数名：F_Maintenance_SendProgress
 * 说明：构造并尝试发送进度条数值更新帧，数值采用四字节大端格式
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       control：进度条控件编号，0xFFFF 表示禁用输出
 *       percent：已限制在 0～100 的进度百分比
 * 输出：返回 true 表示已受理发送或输出已禁用，false 表示暂时无法发送；g_context：发送时更新硬件发送状态
 * 使用：仅在本 .c 文件内部调用，用于刷新整机和传感器的进度条
 */
static bool F_Maintenance_SendProgress(Maintenance_Context *g_context, uint16_t control, uint8_t percent)
{
    uint8_t frame[] = {0xEE,0xB1,0x10,0,0,0,0,0,0,0,0,0xFF,0xFC,0xFF,0xFF};
    if ((MAINTENANCE_SCREEN_ID == 0xFFFFU) || (control == 0xFFFFU)) { return true; }
    frame[3] = (uint8_t) (MAINTENANCE_SCREEN_ID >> 8);
    frame[4] = (uint8_t) MAINTENANCE_SCREEN_ID;
    frame[5] = (uint8_t) (control >> 8); frame[6] = (uint8_t) control;

    frame[10] = percent; // 进度使用四字节大端整数，前三字节为零，不发送十进制文本。
    return H_Maintenance_PortSend(&g_context->port, frame, sizeof(frame));
}

/*
 * 函数名：F_Maintenance_SendBarColor
 * 说明：构造并尝试发送进度条前景色设置帧
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       control：有效的进度条控件编号
 *       rgb565：16 位 RGB565 颜色值
 * 输出：返回 true 表示发送已受理，false 表示暂时无法发送；g_context：更新硬件发送状态
 * 使用：仅在本 .c 文件内部调用，由已启用的显示任务切换正常蓝色和到期红色
 */
static bool F_Maintenance_SendBarColor(Maintenance_Context *g_context, uint16_t control, uint16_t rgb565)
{
    uint8_t frame[] = {0xEE,0xB1,0x19,0,0,0,0,0,0,0xFF,0xFC,0xFF,0xFF};
    frame[3] = (uint8_t) (MAINTENANCE_SCREEN_ID >> 8);
    frame[4] = (uint8_t) MAINTENANCE_SCREEN_ID;
    frame[5] = (uint8_t) (control >> 8); frame[6] = (uint8_t) control;
    frame[7] = (uint8_t) (rgb565 >> 8); frame[8] = (uint8_t) rgb565;
    return H_Maintenance_PortSend(&g_context->port, frame, sizeof(frame));
}

/*
 * 函数名：F_Maintenance_StatusText
 * 说明：根据项目进度和状态选择中文提示，满进度优先显示过期
 * 输入：g_para：包含总进度和状态的项目运行数据
 * 输出：返回只读 GBK 字符串指针，不得修改或释放；无输出参数
 * 使用：仅在本 .c 文件内部调用，用于状态控件显示，编码与 VisualTFT 的 encode=1 一致
 */
static const char *F_Maintenance_StatusText(const Maintenance_Item_State *g_para)
{
    if (g_para->progress_percent == 100U) { return "\xB9\xFD\xC6\xDA"; } // 过期；中文使用 GBK 编码，与屏幕 encode=1 一致。
    switch (g_para->status)
    {
        case MAINTENANCE_RUNNING: return "\xBD\xA1\xBF\xB5"; // 健康。
        case MAINTENANCE_RTC_FAULT: return "\xCA\xB1\xD6\xD3\xD2\xEC\xB3\xA3"; // 时钟异常。
        case MAINTENANCE_SAVE_FAULT: return "\xB4\xE6\xB4\xA2\xD2\xEC\xB3\xA3"; // 存储异常。
        case MAINTENANCE_HARDWARE_FAULT: return "\xD3\xB2\xBC\xFE\xD2\xEC\xB3\xA3"; // 硬件异常。
        default: return "\xB5\xC8\xB4\xFD"; // 等待。
    }
}

/*
 * 函数名：F_Maintenance_DisplayTask
 * 说明：按字段轮流刷新两项目的日历、小时、状态、颜色和进度条
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       now：本次调度的毫秒计数，允许无符号回绕
 * 输出：无返回值；g_context：成功发送后推进 display_field，一轮结束后更新 display_ms
 * 使用：仅在本 .c 文件内部调用，在无需优先查询 RTC 时执行；串口忙时保留当前字段供下轮重试
 */
static void F_Maintenance_DisplayTask(Maintenance_Context *g_context, uint32_t now)
{
    const Maintenance_Item_Save *g_save;
    const Maintenance_Item_State *g_para;
    uint8_t field = g_context->state.display_field;
    bool sensor = field >= 6U;
    uint16_t bar = sensor ? MAINTENANCE_SENSOR_BAR_ID : MAINTENANCE_MACHINE_BAR_ID;
    char value[32], limit[12];
    bool sent;
    if (MAINTENANCE_SCREEN_ID == 0xFFFFU) { return; }
    if ((field == 0U) &&
        ((uint32_t) (now - g_context->state.display_ms) < MAINTENANCE_DISPLAY_PERIOD_MS)) { return; }
    g_save = sensor ? &g_context->save.sensor : &g_context->save.machine;
    g_para = sensor ? &g_context->state.sensor : &g_context->state.machine;
    switch (field % 6U)
    {
        case 0:
            if (g_para->countdown_valid) { F_Maintenance_Decimal(g_para->elapsed_days, value); }
            else { strcpy(value, "--"); }
            strcat(value, "\xCC\xEC/"); // 添加“天/”，已过天数按实际值显示，不在周期处封顶。
            sent = F_Maintenance_SendText(g_context, sensor ? MAINTENANCE_SENSOR_PROGRESS_ID : MAINTENANCE_MACHINE_PROGRESS_ID, value);
            break;
        case 1:
            if (g_context->state.record_valid) { F_Maintenance_Decimal(g_save->period_days, value); }
            else { strcpy(value, "--"); }
            strcat(value, "\xCC\xEC");
            sent = F_Maintenance_SendText(g_context, sensor ? MAINTENANCE_SENSOR_PERIOD_ID : MAINTENANCE_MACHINE_PERIOD_ID, value);
            break;
        case 2:
            if (g_para->hours_valid) { F_Maintenance_Decimal(g_para->used_hours, value); }
            else { strcpy(value, "--"); }
            strcat(value, "h/");
            if (g_context->state.record_valid) { F_Maintenance_Decimal(g_save->period_hours, limit); }
            else { strcpy(limit, "--"); }
            strcat(value, limit); strcat(value, "h");
            sent = F_Maintenance_SendText(g_context, sensor ? MAINTENANCE_SENSOR_HOURS_ID : MAINTENANCE_MACHINE_HOURS_ID, value);
            break;
        case 3:
            sent = F_Maintenance_SendText(g_context, sensor ? MAINTENANCE_SENSOR_STATUS_ID : MAINTENANCE_MACHINE_STATUS_ID, F_Maintenance_StatusText(g_para));
            break;
        case 4:
            sent = F_Maintenance_SendBarColor(g_context, bar, (g_para->progress_percent == 100U) ? MAINTENANCE_COLOR_DUE : MAINTENANCE_COLOR_NORMAL);
            break;
        default:
            sent = F_Maintenance_SendProgress(g_context, bar, g_para->progress_percent);
            break;
    }
    if (sent && (++g_context->state.display_field >= 12U)) // 发送忙时停留在当前字段，成功后才推进显示步骤。
    {
        g_context->state.display_field = 0;
        g_context->state.display_ms = now;
    }
}

/*
 * 函数名：F_Maintenance_ProtocolTask
 * 说明：处理 RTC 超时和接收数据，等待屏幕启动后调度 RTC 查询及显示刷新
 * 输入：g_context：非空维保上下文指针，首次任务调用前清零，运行期间地址保持有效
 *       now：本次调度的毫秒计数，允许无符号回绕
 * 输出：无返回值；g_context：更新通信、RTC、显示调度及维保状态
 * 使用：仅在本 .c 文件内部调用，由维保任务每轮执行，发送 RTC 查询优先于显示刷新
 */
static void F_Maintenance_ProtocolTask(Maintenance_Context *g_context, uint32_t now)
{
    if (g_context->state.rtc_pending &&
        ((uint32_t) (now - g_context->state.last_rtc_request_ms) > MAINTENANCE_RTC_TIMEOUT_MS))
    {
        F_Maintenance_RejectRtc(g_context);
        g_context->state.frame_length = 0;
    }
    F_Maintenance_ReceiveBytes(g_context, now);
    F_Maintenance_UpdateStatus(g_context, now);
    if (!g_context->state.screen_ready)
    {
        if ((uint32_t) (now - g_context->state.started_ms) < MAINTENANCE_SCREEN_BOOT_MS) { return; }
        g_context->state.screen_ready = true;
    }
    if ((g_context->state.calibration_phase == 1U) || (g_context->state.calibration_phase == 3U)) { return; }
    if (!g_context->state.rtc_pending &&
        ((uint32_t) (now - g_context->state.last_rtc_request_ms) >= MAINTENANCE_RTC_POLL_MS))
    {
        if (H_Maintenance_PortSend(&g_context->port, rtc_request, sizeof(rtc_request)))
        {
            g_context->state.rtc_pending = true;
            g_context->state.last_rtc_request_ms = now;
        }
        return;
    }
    F_Maintenance_DisplayTask(g_context, now);
}

/*
 * 函数名：F_Maintenance_SetRtc
 * 说明：保存校时事务并发起异步 RTC 校准，保留两个保养项目已累计的时间
 * 输入：g_context：非空模块上下文；g_date：2000～2099 年的合法目标日期时间
 * 输出：返回 MAINTENANCE_IN_PROGRESS 表示已受理，其他值表示参数、时间、就绪或存储错误；g_context：更新校时状态
 * 使用：供 A_Maintenance.c 跨文件调用，或本文件处理屏幕输入；主循环继续调度直到查询结果结束，校时过程中禁止复位或修改周期
 */
Maintenance_Result F_Maintenance_SetRtc(Maintenance_Context *g_context, const Maintenance_Date *g_date)
{
    Maintenance_Save g_candidate;
    Maintenance_State *g_state = &g_context->state;
    uint32_t day, now = H_Maintenance_PortNow(&g_context->port);
    Maintenance_Result result;
    if (!F_Maintenance_DateToDay(g_date, &day)) { return MAINTENANCE_INVALID_ARGUMENT; }
    if (!g_state->initialized || !g_state->port_ready || !g_state->record_valid ||
        g_state->migration_pending || (g_state->calibration_phase != 0U)) { return MAINTENANCE_NOT_READY; }
    if (g_state->storage_fault || g_state->storage_corrupt) { return MAINTENANCE_STORAGE_ERROR; }
    if (!g_state->rtc_valid || ((uint32_t) (now - g_state->last_rtc_ms) > MAINTENANCE_RTC_FRESH_MS))
    { return MAINTENANCE_TIME_ERROR; }
    F_Maintenance_UpdateUptime(g_context, now);
    g_candidate = g_context->save;
    g_candidate.calibration_pending = true;
    g_candidate.rtc_target_seconds = day * 86400U + (uint32_t) g_date->hour * 3600U +
        (uint32_t) g_date->minute * 60U + g_date->second;
    g_state->calibration_base_seconds = g_state->calendar_ready ? g_state->calendar_seconds : g_candidate.calendar_seconds;
    g_state->calibration_base_ms = g_state->calendar_ready ? g_state->last_rtc_ms : now;
    result = F_Maintenance_Commit(g_context, &g_candidate);
    if (result != MAINTENANCE_OK) { return result; } // 未确认校时意图已落盘时，绝不发送 RTC 修改指令。
    g_state->calibration_target = *g_date;
    g_state->calibration_phase = 1U;
    g_state->calibration_result = MAINTENANCE_IN_PROGRESS;
    g_state->calibration_notice = true;
    g_state->rtc_pending = false;
    return MAINTENANCE_IN_PROGRESS;
}

/*
 * 函数名：F_Maintenance_GetRtcSetResult
 * 说明：读取最近一次已受理的异步校时结果
 * 输入：g_context：非空模块上下文指针
 * 输出：返回 MAINTENANCE_NOT_READY 表示尚未校时，MAINTENANCE_IN_PROGRESS 表示进行中，MAINTENANCE_OK 表示已回读并保存，其余值表示失败
 * 使用：仅供 A_Maintenance.c 跨文件调用；拒绝受理的请求通过 SetRtc 的返回值判断
 */
Maintenance_Result F_Maintenance_GetRtcSetResult(const Maintenance_Context *g_context)
{
    return g_context->state.initialized ? g_context->state.calibration_result : MAINTENANCE_NOT_READY;
}

/*
 * 函数名：F_Maintenance_CalibrationInput
 * 说明：校验屏幕校时输入通知，并将十四位年月日时分秒交给校时入口
 * 输入：g_context：包含完整接收帧的非空上下文，输入格式为 YYYYMMDDhhmmss
 * 输出：无返回值；g_context：合法请求进入校时流程，非法输入只更新提示，不修改 RTC 或保养起点
 * 使用：仅在本 .c 文件内部调用，只处理配置页面的校时文本控件通知
 */
static void F_Maintenance_CalibrationInput(Maintenance_Context *g_context)
{
    const uint8_t *f = g_context->state.frame;
    uint16_t values[7];
    uint8_t i;
    Maintenance_Date g_date;
    Maintenance_Result result = MAINTENANCE_INVALID_ARGUMENT;
    if ((MAINTENANCE_SCREEN_ID == 0xFFFFU) ||
        (((uint16_t) f[3] << 8 | f[4]) != MAINTENANCE_SCREEN_ID) ||
        (((uint16_t) f[5] << 8 | f[6]) != MAINTENANCE_RTC_INPUT_ID) || (f[7] != 0x11U)) { return; }
    if (g_context->state.calibration_phase != 0U) { return; } // 重复提交不覆盖正在执行事务的目标和结果。
    if ((g_context->state.frame_length == 27U) && (f[22] == 0U))
    {
        for (i = 0; i < 14U; ++i)
        { if ((f[8U + i] < '0') || (f[8U + i] > '9')) { break; } }
        if (i == 14U)
        {
            for (i = 0; i < 7U; ++i) { values[i] = (uint16_t) ((f[8U + i * 2U] - '0') * 10U + f[9U + i * 2U] - '0'); }
            g_date.year = (uint16_t) (values[0] * 100U + values[1]);
            g_date.month = (uint8_t) values[2]; g_date.day = (uint8_t) values[3];
            g_date.hour = (uint8_t) values[4]; g_date.minute = (uint8_t) values[5]; g_date.second = (uint8_t) values[6];
            result = F_Maintenance_SetRtc(g_context, &g_date);
        }
    }
    g_context->state.calibration_result = result;
    g_context->state.calibration_notice = true;
}

/*
 * 函数名：F_Maintenance_CalibrationTask
 * 说明：发送 RTC 修改指令、等待回读、保存新锚点并刷新中文校时结果
 * 输入：g_context：非空模块上下文；now：当前毫秒计数，允许无符号回绕
 * 输出：无返回值；g_context：推进校时状态，超时或保存失败时保留待确认记录并提示重新校准
 * 使用：仅在本 .c 文件内部调用；每轮任务执行，发送忙时重试，重启后不会自动重发旧校时目标
 */
static void F_Maintenance_CalibrationTask(Maintenance_Context *g_context, uint32_t now)
{
    Maintenance_State *g_state = &g_context->state;
    if (g_state->calibration_phase == 3U)
    {
        Maintenance_Save g_candidate = g_context->save;
        g_candidate.calibration_pending = false;
        g_candidate.rtc_target_seconds = 0;
        F_Maintenance_UpdateUptime(g_context, now);
        g_state->calibration_result = F_Maintenance_Commit(g_context, &g_candidate);
        if (g_state->calibration_result != MAINTENANCE_OK) { g_state->calendar_ready = false; }
        g_state->calibration_phase = 0;
        g_state->calibration_notice = true;
    }
    else if ((g_state->calibration_phase != 0U) &&
        ((uint32_t) (now - g_state->calibration_base_ms) > MAINTENANCE_CALIBRATION_TIMEOUT_MS))
    {
        g_state->calibration_phase = 0;
        g_state->calendar_ready = false;
        g_state->calibration_result = MAINTENANCE_TIME_ERROR;
        g_state->calibration_notice = true;
    }
    else if (g_state->calibration_phase == 1U)
    {
        const Maintenance_Date *g_date = &g_state->calibration_target;
        uint32_t day = 0;
        uint8_t i, frame[] = {0xEE,0x81,0,0,0,0,0,0,0,0xFF,0xFC,0xFF,0xFF};
        (void) F_Maintenance_DateToDay(g_date, &day);
        frame[2] = g_date->second; frame[3] = g_date->minute; frame[4] = g_date->hour;
        frame[5] = g_date->day; frame[6] = (uint8_t) ((day + 6U) % 7U); // 2000-01-01 为星期六，协议规定星期日为零。
        frame[7] = g_date->month; frame[8] = (uint8_t) (g_date->year - 2000U);
        for (i = 2U; i <= 8U; ++i) { frame[i] = (uint8_t) ((frame[i] / 10U) * 16U + frame[i] % 10U); }
        if (H_Maintenance_PortSend(&g_context->port, frame, sizeof(frame)))
        {
            g_state->calibration_phase = 2U;
            g_state->calibration_sent_ms = now;
            g_state->last_rtc_request_ms = now; // 下一轮轮询读回目标，给屏幕完成 RTC 写入的时间。
        }
        return;
    }
    if (g_state->calibration_notice && g_state->screen_ready)
    {
        const char *text;
        switch (g_state->calibration_result)
        {
            case MAINTENANCE_OK: text = "\xD0\xA3\xD7\xBC\xB3\xC9\xB9\xA6"; break; // 校准成功。
            case MAINTENANCE_IN_PROGRESS: text = "\xD0\xA3\xD7\xBC\xD6\xD0"; break; // 校准中。
            case MAINTENANCE_INVALID_ARGUMENT: text = "\xCA\xB1\xBC\xE4\xCE\xDE\xD0\xA7"; break; // 时间无效。
            case MAINTENANCE_STORAGE_ERROR: text = "\xB4\xE6\xB4\xA2\xD2\xEC\xB3\xA3"; break; // 存储异常。
            default: text = "\xC7\xEB\xD6\xD8\xD0\xC2\xD0\xA3\xD7\xBC"; break; // 请重新校准。
        }
        if (F_Maintenance_SendText(g_context, MAINTENANCE_RTC_RESULT_ID, text)) { g_state->calibration_notice = false; }
    }
}
