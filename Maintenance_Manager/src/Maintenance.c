#include "maintenance_internal.h"
#include <string.h>

Maintenance_save_t Maintenance_save;
Maintenance_para_t Maintenance_para;

bool Maintenance_DateToDay(const Maintenance_date_t *date, uint32_t *day)
{
    static const uint8_t month_days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint32_t total = 0;
    uint16_t year;
    uint8_t month, limit;
    if ((date == NULL) || (day == NULL) || (date->year < 2000U) ||
        (date->year > 2099U) || (date->month < 1U) || (date->month > 12U) ||
        (date->hour > 23U) || (date->minute > 59U) || (date->second > 59U))
    {
        return false;
    }
    limit = month_days[date->month - 1U];
    if ((date->month == 2U) && ((date->year % 4U) == 0U)) { ++limit; }
    if ((date->day == 0U) || (date->day > limit)) { return false; }
    for (year = 2000U; year < date->year; ++year)
    {
        total += ((year % 4U) == 0U) ? 366U : 365U;
    }
    for (month = 1U; month < date->month; ++month)
    {
        total += month_days[month - 1U];
        if ((month == 2U) && ((date->year % 4U) == 0U)) { ++total; }
    }
    *day = total + date->day - 1U;
    return true;
}

void Maintenance_RejectRtc(void)
{
    Maintenance_para.rtc_valid = false;
    Maintenance_para.rtc_pending = false;
    ++Maintenance_para.rtc_error_count;
}

void Maintenance_AcceptRtc(const Maintenance_date_t *date, uint32_t now)
{
    uint32_t day;
    if (!Maintenance_DateToDay(date, &day) ||
        (Maintenance_para.have_rtc && (day < Maintenance_para.highest_day)) ||
        (Maintenance_para.record_valid && (day < Maintenance_save.saved_day)))
    {
        Maintenance_RejectRtc();
        return;
    }
    Maintenance_para.rtc = *date;
    Maintenance_para.current_day = day;
    Maintenance_para.highest_day = day;
    Maintenance_para.have_rtc = true;
    Maintenance_para.rtc_valid = true;
    Maintenance_para.rtc_pending = false;
    Maintenance_para.last_rtc_ms = now;
}

static void update_item(const Maintenance_item_save_t *save, Maintenance_item_para_t *para)
{
    para->due_day = save->start_day + save->period_days;
    para->elapsed_days = Maintenance_para.current_day - save->start_day;
    para->remaining_days = (Maintenance_para.current_day >= para->due_day) ?
        0U : (uint16_t) (para->due_day - Maintenance_para.current_day);
    para->progress_percent = (para->elapsed_days >= save->period_days) ? 100U :
        (uint8_t) ((para->elapsed_days * 100U) / save->period_days);
    para->countdown_valid = true;
    para->status = (para->remaining_days == 0U) ? MAINTENANCE_DUE : MAINTENANCE_RUNNING;
}

void Maintenance_UpdateStatus(uint32_t now)
{
    Maintenance_para.countdown_valid = false;
    Maintenance_para.machine.countdown_valid = false;
    Maintenance_para.sensor.countdown_valid = false;
    if (!Maintenance_para.port_ready)
    {
        Maintenance_para.status = MAINTENANCE_HARDWARE_FAULT;
    }
    else if (Maintenance_para.storage_fault || Maintenance_para.storage_corrupt)
    {
        Maintenance_para.status = MAINTENANCE_SAVE_FAULT;
    }
    else if (!Maintenance_para.rtc_valid ||
             ((uint32_t) (now - Maintenance_para.last_rtc_ms) > MAINTENANCE_RTC_FRESH_MS))
    {
        Maintenance_para.rtc_valid = false;
        Maintenance_para.status = Maintenance_para.have_rtc ? MAINTENANCE_RTC_FAULT : MAINTENANCE_WAITING;
        if (Maintenance_para.rtc_error_count != 0U) { Maintenance_para.status = MAINTENANCE_RTC_FAULT; }
    }
    else if (!Maintenance_para.record_valid || Maintenance_para.migration_pending)
    {
        Maintenance_para.status = MAINTENANCE_WAITING;
    }
    else if (Maintenance_para.current_day < Maintenance_save.saved_day)
    {
        Maintenance_para.rtc_valid = false;
        Maintenance_para.status = MAINTENANCE_RTC_FAULT;
    }
    else
    {
        update_item(&Maintenance_save.machine, &Maintenance_para.machine);
        update_item(&Maintenance_save.sensor, &Maintenance_para.sensor);
        Maintenance_para.countdown_valid = true;
        Maintenance_para.status = ((Maintenance_para.machine.status == MAINTENANCE_DUE) ||
            (Maintenance_para.sensor.status == MAINTENANCE_DUE)) ? MAINTENANCE_DUE : MAINTENANCE_RUNNING;
    }
    if (!Maintenance_para.countdown_valid)
    {
        Maintenance_para.machine.status = Maintenance_para.status;
        Maintenance_para.sensor.status = Maintenance_para.status;
    }
}

static Maintenance_result_t time_ready(void)
{
    uint32_t now;
    if (!Maintenance_para.initialized || !Maintenance_para.port_ready) { return MAINTENANCE_NOT_READY; }
    now = Maintenance_PortNow();
    if ((uint32_t) (now - Maintenance_para.last_rtc_ms) > MAINTENANCE_RTC_FRESH_MS)
    {
        Maintenance_para.rtc_valid = false;
    }
    Maintenance_UpdateStatus(now);
    return Maintenance_para.rtc_valid ? MAINTENANCE_OK : MAINTENANCE_TIME_ERROR;
}

static Maintenance_result_t commit(Maintenance_save_t *candidate)
{
    candidate->saved_day = Maintenance_para.current_day;
    if (!Maintenance_StorageCommit(candidate))
    {
        Maintenance_para.storage_fault = true;
        Maintenance_para.last_result = MAINTENANCE_STORAGE_ERROR;
    }
    else
    {
        Maintenance_save = *candidate;
        Maintenance_para.record_valid = true;
        Maintenance_para.storage_loaded = true;
        Maintenance_para.storage_blank = false;
        Maintenance_para.storage_fault = false;
        Maintenance_para.storage_corrupt = false;
        Maintenance_para.migration_pending = false;
        Maintenance_para.display_field = 0;
        Maintenance_para.display_ms = Maintenance_PortNow() - MAINTENANCE_DISPLAY_PERIOD_MS;
        Maintenance_para.last_result = MAINTENANCE_OK;
    }
    Maintenance_UpdateStatus(Maintenance_PortNow());
    return Maintenance_para.last_result;
}

static Maintenance_result_t set_period(uint16_t days, bool sensor)
{
    Maintenance_save_t candidate;
    Maintenance_result_t result;
    if ((days == 0U) || (days > MAINTENANCE_MAX_DAYS)) { result = MAINTENANCE_INVALID_ARGUMENT; }
    else if ((result = time_ready()) != MAINTENANCE_OK) { /* Return specific readiness error. */ }
    else if (Maintenance_para.storage_corrupt) { result = MAINTENANCE_STORAGE_CORRUPT; }
    else if (Maintenance_para.storage_fault) { result = MAINTENANCE_STORAGE_ERROR; }
    else if (!Maintenance_para.record_valid || Maintenance_para.migration_pending) { result = MAINTENANCE_NOT_READY; }
    else if (days == (sensor ? Maintenance_save.sensor.period_days : Maintenance_save.machine.period_days))
    { result = MAINTENANCE_OK; }
    else
    {
        candidate = Maintenance_save;
        if (sensor) { candidate.sensor.period_days = days; }
        else { candidate.machine.period_days = days; }
        return commit(&candidate);
    }
    Maintenance_para.last_result = result;
    return result;
}

static Maintenance_result_t reset_item(bool sensor)
{
    Maintenance_save_t candidate;
    Maintenance_result_t result = time_ready();
    if (result == MAINTENANCE_OK)
    {
        if (!Maintenance_para.storage_loaded || Maintenance_para.storage_fault)
        {
            result = MAINTENANCE_STORAGE_ERROR;
        }
        else if (Maintenance_para.migration_pending) { result = MAINTENANCE_NOT_READY; }
        else
        {
            candidate = Maintenance_save;
            /* Explicit recovery of missing/corrupt records initializes BOTH items. */
            if (!Maintenance_para.record_valid)
            {
                candidate.machine.start_day = Maintenance_para.current_day;
                candidate.sensor.start_day = Maintenance_para.current_day;
            }
            else if (sensor) { candidate.sensor.start_day = Maintenance_para.current_day; }
            else { candidate.machine.start_day = Maintenance_para.current_day; }
            return commit(&candidate);
        }
    }
    Maintenance_para.last_result = result;
    return result;
}

Maintenance_result_t Maintenance_SetPeriodDays(uint16_t days) { return set_period(days, false); }
Maintenance_result_t Maintenance_SetSensorPeriodDays(uint16_t days) { return set_period(days, true); }
Maintenance_result_t Maintenance_Reset(void) { return reset_item(false); }
Maintenance_result_t Maintenance_ResetSensor(void) { return reset_item(true); }

void Maintenance_Task(void)
{
    uint32_t now;
    if (!Maintenance_para.initialized)
    {
        memset(&Maintenance_para, 0, sizeof(Maintenance_para));
        memset(&Maintenance_save, 0, sizeof(Maintenance_save));
        Maintenance_save.machine.period_days = MAINTENANCE_DEFAULT_DAYS;
        Maintenance_save.sensor.period_days = MAINTENANCE_SENSOR_DEFAULT_DAYS;
        Maintenance_para.active_slot = -1;
        Maintenance_para.initialized = true;
        Maintenance_para.port_ready = Maintenance_PortInit();
        Maintenance_para.started_ms = Maintenance_PortNow();
        Maintenance_para.storage_retry_ms = Maintenance_para.started_ms - MAINTENANCE_STORAGE_RETRY_MS;
        if (!Maintenance_para.port_ready) { Maintenance_para.last_result = MAINTENANCE_PORT_ERROR; }
    }
    now = Maintenance_PortNow();
    if (!Maintenance_para.port_ready) { Maintenance_UpdateStatus(now); return; }
    if ((!Maintenance_para.storage_loaded || Maintenance_para.storage_fault) &&
        ((uint32_t) (now - Maintenance_para.storage_retry_ms) >= MAINTENANCE_STORAGE_RETRY_MS))
    {
        Maintenance_para.storage_retry_ms = now;
        (void) Maintenance_StorageLoad();
    }
    Maintenance_ProtocolTask(now);
    Maintenance_UpdateStatus(now);
    if (((Maintenance_para.storage_blank && !Maintenance_para.record_valid) || Maintenance_para.migration_pending) &&
        !Maintenance_para.storage_fault && Maintenance_para.rtc_valid &&
        ((uint32_t) (now - Maintenance_para.last_rtc_ms) <= MAINTENANCE_RTC_FRESH_MS))
    {
        Maintenance_save_t candidate = Maintenance_save;
        if (!Maintenance_para.migration_pending) { candidate.machine.start_day = Maintenance_para.current_day; }
        candidate.sensor.start_day = Maintenance_para.current_day;
        (void) commit(&candidate);
    }
}
