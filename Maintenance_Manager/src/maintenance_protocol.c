#include "maintenance_internal.h"
#include <string.h>

/* Dacai V5.1. Protocol CRC must be disabled on screen.
 * RTC request: EE 82 FF FC FF FF
 * RTC reply:   EE F7 YY MM WEEK DD hh mm ss FF FC FF FF (all fields BCD). */
static const uint8_t tail[4] = {0xFF, 0xFC, 0xFF, 0xFF};
static const uint8_t rtc_request[6] = {0xEE, 0x82, 0xFF, 0xFC, 0xFF, 0xFF};

static bool bcd(uint8_t raw, uint8_t *value)
{
    if (((raw & 0x0FU) > 9U) || ((raw >> 4) > 9U)) { return false; }
    *value = (uint8_t) ((raw >> 4) * 10U + (raw & 0x0FU));
    return true;
}

static void receive_frame(uint32_t now)
{
    const uint8_t *f = Maintenance_para.frame;
    Maintenance_date_t date;
    uint8_t year, week;
    if ((Maintenance_para.frame_length < 2U) || (f[1] != 0xF7U) ||
        !Maintenance_para.rtc_pending) { return; }
    if ((Maintenance_para.frame_length != 13U) ||
        !bcd(f[2], &year) || !bcd(f[3], &date.month) || !bcd(f[4], &week) ||
        !bcd(f[5], &date.day) || !bcd(f[6], &date.hour) ||
        !bcd(f[7], &date.minute) || !bcd(f[8], &date.second) || (week > 6U))
    {
        Maintenance_RejectRtc();
        return;
    }
    date.year = (uint16_t) (2000U + year);
    Maintenance_AcceptRtc(&date, now);
}

static void receive_bytes(uint32_t now)
{
    uint8_t byte;
    uint16_t budget = MAINTENANCE_RX_SIZE;
    if (Maintenance_PortRxFault())
    {
        Maintenance_para.frame_length = 0;
        Maintenance_RejectRtc();
    }
    if ((Maintenance_para.frame_length != 0U) &&
        ((uint32_t) (now - Maintenance_para.frame_last_ms) > MAINTENANCE_FRAME_TIMEOUT_MS))
    {
        Maintenance_para.frame_length = 0;
    }
    while ((budget-- != 0U) && Maintenance_PortReadByte(&byte))
    {
        if (Maintenance_para.frame_length == 0U)
        {
            if (byte != 0xEEU) { continue; }
        }
        if (Maintenance_para.frame_length >= MAINTENANCE_FRAME_SIZE)
        {
            Maintenance_para.frame_length = 0;
            Maintenance_RejectRtc();
            if (byte != 0xEEU) { continue; }
        }
        Maintenance_para.frame[Maintenance_para.frame_length++] = byte;
        Maintenance_para.frame_last_ms = now;
        if ((Maintenance_para.frame_length >= 6U) &&
            (memcmp(&Maintenance_para.frame[Maintenance_para.frame_length - 4U], tail, 4) == 0))
        {
            if (Maintenance_PortRxFault()) { Maintenance_RejectRtc(); }
            else { receive_frame(now); }
            Maintenance_para.frame_length = 0;
        }
    }
}

static bool send_text(uint16_t control, const char *text)
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
    return Maintenance_PortSend(frame, (uint16_t) (length + 11U));
}

static void decimal(uint32_t number, char *text)
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

static bool send_progress(uint16_t control, uint8_t percent)
{
    uint8_t frame[] = {0xEE,0xB1,0x10,0,0,0,0,0,0,0,0,0xFF,0xFC,0xFF,0xFF};
    if ((MAINTENANCE_SCREEN_ID == 0xFFFFU) || (control == 0xFFFFU)) { return true; }
    frame[3] = (uint8_t) (MAINTENANCE_SCREEN_ID >> 8);
    frame[4] = (uint8_t) MAINTENANCE_SCREEN_ID;
    frame[5] = (uint8_t) (control >> 8); frame[6] = (uint8_t) control;
    /* Progress value is a FOUR-byte big-endian integer, not ASCII. */
    frame[10] = percent;
    return Maintenance_PortSend(frame, sizeof(frame));
}

static bool send_bar_color(uint16_t control, uint16_t rgb565)
{
    uint8_t frame[] = {0xEE,0xB1,0x19,0,0,0,0,0,0,0xFF,0xFC,0xFF,0xFF};
    frame[3] = (uint8_t) (MAINTENANCE_SCREEN_ID >> 8);
    frame[4] = (uint8_t) MAINTENANCE_SCREEN_ID;
    frame[5] = (uint8_t) (control >> 8); frame[6] = (uint8_t) control;
    frame[7] = (uint8_t) (rgb565 >> 8); frame[8] = (uint8_t) rgb565;
    return Maintenance_PortSend(frame, sizeof(frame));
}

static const char *status_text(const Maintenance_item_para_t *para)
{
    /* GBK text, matching VisualTFT encode=1. */
    if (para->progress_percent == 100U) { return "\xB9\xFD\xC6\xDA"; } /* overdue */
    switch (para->status)
    {
        case MAINTENANCE_RUNNING: return "\xBD\xA1\xBF\xB5"; /* healthy */
        case MAINTENANCE_RTC_FAULT: return "\xCA\xB1\xD6\xD3\xD2\xEC\xB3\xA3";
        case MAINTENANCE_SAVE_FAULT: return "\xB4\xE6\xB4\xA2\xD2\xEC\xB3\xA3";
        case MAINTENANCE_HARDWARE_FAULT: return "\xD3\xB2\xBC\xFE\xD2\xEC\xB3\xA3";
        default: return "\xB5\xC8\xB4\xFD";
    }
}

static void display_task(uint32_t now)
{
    const Maintenance_item_save_t *save;
    const Maintenance_item_para_t *para;
    uint8_t field = Maintenance_para.display_field;
    bool sensor = field >= 6U;
    uint16_t bar = sensor ? MAINTENANCE_SENSOR_BAR_ID : MAINTENANCE_MACHINE_BAR_ID;
    char value[32], limit[12];
    bool sent;
    if (MAINTENANCE_SCREEN_ID == 0xFFFFU) { return; }
    if ((field == 0U) &&
        ((uint32_t) (now - Maintenance_para.display_ms) < MAINTENANCE_DISPLAY_PERIOD_MS)) { return; }
    save = sensor ? &Maintenance_save.sensor : &Maintenance_save.machine;
    para = sensor ? &Maintenance_para.sensor : &Maintenance_para.machine;
    switch (field % 6U)
    {
        case 0:
            if (para->countdown_valid) { decimal(para->elapsed_days, value); }
            else { strcpy(value, "--"); }
            strcat(value, "\xCC\xEC/"); /* days, no cap */
            sent = send_text(sensor ? MAINTENANCE_SENSOR_PROGRESS_ID : MAINTENANCE_MACHINE_PROGRESS_ID, value);
            break;
        case 1:
            if (Maintenance_para.record_valid) { decimal(save->period_days, value); }
            else { strcpy(value, "--"); }
            strcat(value, "\xCC\xEC");
            sent = send_text(sensor ? MAINTENANCE_SENSOR_PERIOD_ID : MAINTENANCE_MACHINE_PERIOD_ID, value);
            break;
        case 2:
            if (para->hours_valid) { decimal(para->used_hours, value); }
            else { strcpy(value, "--"); }
            strcat(value, "h/");
            if (Maintenance_para.record_valid) { decimal(save->period_hours, limit); }
            else { strcpy(limit, "--"); }
            strcat(value, limit); strcat(value, "h");
            sent = send_text(sensor ? MAINTENANCE_SENSOR_HOURS_ID : MAINTENANCE_MACHINE_HOURS_ID, value);
            break;
        case 3:
            sent = send_text(sensor ? MAINTENANCE_SENSOR_STATUS_ID : MAINTENANCE_MACHINE_STATUS_ID, status_text(para));
            break;
        case 4:
            sent = send_bar_color(bar, (para->progress_percent == 100U) ? MAINTENANCE_COLOR_DUE : MAINTENANCE_COLOR_NORMAL);
            break;
        default:
            sent = send_progress(bar, para->progress_percent);
            break;
    }
    if (sent && (++Maintenance_para.display_field >= 12U))
    {
        Maintenance_para.display_field = 0;
        Maintenance_para.display_ms = now;
    }
}

void Maintenance_ProtocolTask(uint32_t now)
{
    if (Maintenance_para.rtc_pending &&
        ((uint32_t) (now - Maintenance_para.last_rtc_request_ms) > MAINTENANCE_RTC_TIMEOUT_MS))
    {
        Maintenance_RejectRtc();
        Maintenance_para.frame_length = 0;
    }
    receive_bytes(now);
    Maintenance_UpdateStatus(now);
    if (!Maintenance_para.screen_ready)
    {
        if ((uint32_t) (now - Maintenance_para.started_ms) < MAINTENANCE_SCREEN_BOOT_MS) { return; }
        Maintenance_para.screen_ready = true;
    }
    if (!Maintenance_para.rtc_pending &&
        ((uint32_t) (now - Maintenance_para.last_rtc_request_ms) >= MAINTENANCE_RTC_POLL_MS))
    {
        if (Maintenance_PortSend(rtc_request, sizeof(rtc_request)))
        {
            Maintenance_para.rtc_pending = true;
            Maintenance_para.last_rtc_request_ms = now;
        }
        return;
    }
    display_task(now);
}
