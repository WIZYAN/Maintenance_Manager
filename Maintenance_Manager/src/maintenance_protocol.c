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

static void decimal(uint16_t number, char *text)
{
    char reverse[5];
    uint8_t count = 0, index = 0;
    do
    {
        reverse[count++] = (char) ('0' + number % 10U);
        number = (uint16_t) (number / 10U);
    } while (number != 0U);
    while (count != 0U) { text[index++] = reverse[--count]; }
    text[index] = '\0';
}

static void display_task(uint32_t now)
{
    static const char *const status_text[] =
        {"WAIT RTC", "RUNNING", "DUE", "RTC ERROR", "EEPROM ERROR", "HW ERROR"};
    char value[16];
    bool sent;
    if (MAINTENANCE_SCREEN_ID == 0xFFFFU) { return; }
    if ((Maintenance_para.display_field == 0U) &&
        ((uint32_t) (now - Maintenance_para.display_ms) < MAINTENANCE_DISPLAY_PERIOD_MS)) { return; }
    switch (Maintenance_para.display_field)
    {
        case 0:
            if (Maintenance_para.countdown_valid)
            {
                decimal(Maintenance_para.remaining_days, value);
            }
            else { value[0] = '-'; value[1] = '-'; value[2] = '\0'; }
            sent = send_text(MAINTENANCE_REMAIN_CONTROL_ID, value);
            break;
        case 1:
            decimal(Maintenance_save.period_days, value);
            sent = send_text(MAINTENANCE_PERIOD_CONTROL_ID, value);
            break;
        default:
            sent = send_text(MAINTENANCE_STATUS_CONTROL_ID, status_text[Maintenance_para.status]);
            break;
    }
    if (sent)
    {
        if (++Maintenance_para.display_field >= 3U)
        {
            Maintenance_para.display_field = 0;
            Maintenance_para.display_ms = now;
        }
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
