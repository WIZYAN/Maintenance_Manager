#include "test_maintenance.h"
// 白盒测试包含 A/F 实现，以检查私有实例和日期辅助函数，不增加生产测试接口。
#include "Maintenance/F_Maintenance.c"

#include "Maintenance/A_Maintenance.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
/* Exit deterministically instead of opening the Windows CRT abort dialog. */
#undef assert
#define assert(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

static uint8_t eeprom[32768];
static uint8_t baseline[32768];
static uint32_t now_ms;
static unsigned writes, rtc_requests, rtc_sets, displays;
static uint8_t last_rtc_set[13];
static bool send_busy;
static int fail_after = -1;
static int fail_read_after = -1;
static bool read_failure, init_failure;
static unsigned port_initializations;
static uint8_t last_display[MAINTENANCE_TX_SIZE];
static uint16_t last_display_length;
static uint8_t screen_frames[24][MAINTENANCE_TX_SIZE];
static uint16_t screen_lengths[24];
static uint16_t bar_colors[7];

bool H_Maintenance_PortInit(Maintenance_Port *g_port) {
    (void) g_port; ++port_initializations; return !init_failure; }
uint32_t H_Maintenance_PortNow(const Maintenance_Port *g_port) {
    (void) g_port; return now_ms; }
bool H_Maintenance_PortReadByte(Maintenance_Port *g_port, uint8_t *byte)
{
    if (g_port->rx_tail == g_port->rx_head) { return false; }
    *byte = g_port->rx[g_port->rx_tail];
    g_port->rx_tail = (uint16_t) ((g_port->rx_tail + 1U) % MAINTENANCE_RX_SIZE);
    return true;
}
bool H_Maintenance_PortRxFault(Maintenance_Port *g_port)
{
    bool fault = g_port->rx_fault;
    if (fault) { g_port->rx_tail = g_port->rx_head; g_port->rx_fault = false; }
    return fault;
}
bool H_Maintenance_PortSend(Maintenance_Port *g_port, const uint8_t *data, uint16_t length)
{
    (void) g_port;
    if (send_busy) { return false; }
    assert(length <= MAINTENANCE_TX_SIZE);
    assert(data[0] == 0xEE);
    if (data[1] == 0x82)
    {
        static const uint8_t expected[] = {0xEE,0x82,0xFF,0xFC,0xFF,0xFF};
        assert(length == sizeof(expected));
        assert(memcmp(data, expected, sizeof(expected)) == 0);
        ++rtc_requests;
    }
    else if (data[1] == 0x81)
    {
        assert(length == 13U && memcmp(data + 9, tail, 4) == 0);
        memcpy(last_rtc_set, data, length); ++rtc_sets;
    }
    else
    {
        assert(data[1] == 0xB1 && (data[2] == 0x10 || data[2] == 0x19));
        assert(data[5] == 0 && ((data[6] >= 1 && data[6] <= 6) || (data[6] >= 17 && data[6] <= 22)));
        if (data[2] == 0x19)
        {
            assert(length == 13 && (data[6] == 5 || data[6] == 6));
            bar_colors[data[6]] = (uint16_t) (((uint16_t) data[7] << 8) | data[8]);
        }
        else
        {
            memcpy(screen_frames[data[6]], data, length);
            screen_lengths[data[6]] = length;
        }
        memcpy(last_display, data, length); last_display_length = length; ++displays;
    }
    return true;
}
bool H_Maintenance_EepromRead(uint16_t address, uint8_t *data, uint16_t length)
{
    if (read_failure) { return false; }
    if (fail_read_after == 0) { fail_read_after = -1; return false; }
    if (fail_read_after > 0) { --fail_read_after; }
    assert((uint32_t) address + length <= sizeof(eeprom));
    memcpy(data, &eeprom[address], length);
    return true;
}
bool H_Maintenance_EepromWrite(uint16_t address, const uint8_t *data, uint16_t length)
{
    uint16_t i;
    ++writes;
    assert((uint32_t) address + length <= sizeof(eeprom));
    assert(address >= MAINTENANCE_EEPROM_JOURNAL_BASE && address + length <= 32768U);
    assert((address / 64U) == ((address + length - 1U) / 64U));
    for (i = 0; i < length; ++i)
    {
        if (fail_after == 0) { return false; }
        if (fail_after > 0) { --fail_after; }
        eeprom[address + i] = data[i];
    }
    return true;
}

static uint8_t bcd(unsigned value) { return (uint8_t) (((value / 10U) << 4) | (value % 10U)); }
static void enqueue(const uint8_t *data, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i)
    {
        uint16_t next = (uint16_t) ((g_maintenance.port.rx_head + 1U) % MAINTENANCE_RX_SIZE);
        assert(next != g_maintenance.port.rx_tail);
        g_maintenance.port.rx[g_maintenance.port.rx_head] = data[i];
        g_maintenance.port.rx_head = next;
    }
}
static void reboot(void)
{
    memset(&g_maintenance, 0, sizeof(g_maintenance));
    memset(&g_maintenance.save, 0, sizeof(g_maintenance.save));
    now_ms = 0;
    A_Maintenance_Task();
    now_ms = MAINTENANCE_SCREEN_BOOT_MS;
    A_Maintenance_Task();
}
static void fresh(void)
{
    memset(eeprom, 0xFF, sizeof(eeprom));
    fail_after = -1; fail_read_after = -1; read_failure = false; init_failure = false;
    writes = 0; rtc_requests = 0; rtc_sets = 0; displays = 0; send_busy = false;
    reboot();
}
static void sample(unsigned year, unsigned month, unsigned day)
{
    uint8_t packet[] = {0xEE,0xF7,0,0,0,0,0x12,0x34,0x56,0xFF,0xFC,0xFF,0xFF};
    if (!g_maintenance.state.rtc_pending)
    {
        now_ms += MAINTENANCE_RTC_POLL_MS;
        A_Maintenance_Task();
    }
    assert(g_maintenance.state.rtc_pending);
    packet[2] = bcd(year - 2000U); packet[3] = bcd(month); packet[5] = bcd(day);
    enqueue(packet, sizeof(packet)); A_Maintenance_Task();
}

static void test_calendar(void)
{
    Maintenance_Date g_d = {2000,1,1,0,0,0};
    uint32_t day, expected = 0;
    unsigned y, m, dd;
    for (y = 2000; y <= 2099; ++y)
    {
        for (m = 1; m <= 12; ++m)
        {
            for (dd = 1; dd <= 31; ++dd)
            {
                g_d.year = (uint16_t) y; g_d.month = (uint8_t) m; g_d.day = (uint8_t) dd;
                if (F_Maintenance_DateToDay(&g_d, &day)) { assert(day == expected++); }
            }
        }
    }
    assert(expected == 36525U);
    g_d.year = 2025; g_d.month = 2; g_d.day = 29; assert(!F_Maintenance_DateToDay(&g_d, &day));
    g_d.year = 2024; assert(F_Maintenance_DateToDay(&g_d, &day));
    g_d.hour = 24; assert(!F_Maintenance_DateToDay(&g_d, &day));
    g_d.hour = 0; g_d.month = 0; assert(!F_Maintenance_DateToDay(&g_d, &day));
}

static void test_lifecycle(void)
{
    uint32_t start;
    unsigned old_writes;
    fresh(); assert(writes == 0); assert(g_maintenance.state.storage_blank);
    assert(A_Maintenance_SetPeriodDays(150) == MAINTENANCE_TIME_ERROR);
    sample(2026,1,1);
    start = g_maintenance.save.machine.start_calendar_seconds;
    assert(g_maintenance.state.machine.remaining_days == 180 && g_maintenance.state.countdown_valid);
    old_writes = writes;
    sample(2026,1,31); assert(g_maintenance.state.machine.remaining_days == 150);
    assert(writes == old_writes);
    assert(A_Maintenance_SetPeriodDays(150) == MAINTENANCE_OK);
    assert(g_maintenance.save.machine.start_calendar_seconds == start && g_maintenance.state.machine.remaining_days == 120);
    old_writes = writes;
    assert(A_Maintenance_SetPeriodDays(150) == MAINTENANCE_OK && writes == old_writes);
    assert(A_Maintenance_SetPeriodDays(0) == MAINTENANCE_INVALID_ARGUMENT);
    assert(A_Maintenance_SetPeriodDays(65535U) == MAINTENANCE_INVALID_ARGUMENT);
    reboot(); sample(2026,2,10);
    assert(g_maintenance.save.machine.start_calendar_seconds == start && g_maintenance.save.machine.period_days == 150);
    assert(g_maintenance.state.machine.remaining_days == 110);
    sample(2027,1,1); assert(g_maintenance.state.status == MAINTENANCE_DUE && g_maintenance.state.machine.remaining_days == 0);
    assert(A_Maintenance_Reset() == MAINTENANCE_OK && g_maintenance.state.machine.remaining_days == 150);
    assert(g_maintenance.save.machine.start_calendar_seconds != start);
    now_ms += MAINTENANCE_RTC_FRESH_MS + 1U;
    assert(A_Maintenance_Reset() == MAINTENANCE_TIME_ERROR);
    sample(2027,1,2); assert(g_maintenance.state.machine.remaining_days == 149);
    sample(2027,1,1); assert(!g_maintenance.state.rtc_valid && !g_maintenance.state.countdown_valid);
    assert(A_Maintenance_SetPeriodDays(180) == MAINTENANCE_TIME_ERROR);
    sample(2027,1,2); assert(g_maintenance.state.countdown_valid);
    assert(A_Maintenance_SetPeriodDays(1) == MAINTENANCE_OK);
    assert(g_maintenance.state.status == MAINTENANCE_DUE);
    assert(A_Maintenance_SetPeriodDays(36500) == MAINTENANCE_OK);
    assert(g_maintenance.state.machine.remaining_days == 36499);
}

static void test_protocol(void)
{
    uint8_t packet[] = {0xEE,0xF7,0x24,0x02,0x04,0x29,0x23,0x59,0x59,0xFF,0xFC,0xFF,0xFF};
    unsigned i;
    fresh();
    for (i = 0; i < sizeof(packet); ++i) { enqueue(&packet[i], 1); A_Maintenance_Task(); }
    assert(g_maintenance.state.rtc_valid && g_maintenance.state.rtc.day == 29);
    sample(2024,3,1); assert(g_maintenance.state.machine.remaining_days == 180);
    now_ms += 1000; A_Maintenance_Task();
    packet[3] = 0x1A; enqueue(packet, sizeof(packet)); A_Maintenance_Task();
    assert(!g_maintenance.state.rtc_valid);
    sample(2024,3,1); assert(g_maintenance.state.rtc_valid);
    now_ms += 1000; A_Maintenance_Task();
    packet[3] = 0x03; packet[5] = 0x01;
    enqueue(packet, 5); A_Maintenance_Task(); now_ms += MAINTENANCE_FRAME_TIMEOUT_MS + 1U;
    enqueue(packet + 5, sizeof(packet) - 5); A_Maintenance_Task();
    assert(g_maintenance.state.frame_length == 0);
    now_ms += MAINTENANCE_RTC_TIMEOUT_MS + 1U; A_Maintenance_Task();
    assert(!g_maintenance.state.rtc_valid);
    sample(2024,3,2); assert(g_maintenance.state.rtc_valid);
    g_maintenance.port.rx_fault = true; A_Maintenance_Task(); assert(!g_maintenance.state.rtc_valid);
    sample(2024,3,2); assert(g_maintenance.state.rtc_valid);
    /* Unknown frames may contain EE in payload, and must not be mistaken for RTC. */
    {
        const uint8_t other[] = {0xEE,0xB1,0x11,0xEE,0xF7,0xFF,0xFC,0xFF,0xFF};
        enqueue(other, sizeof(other)); A_Maintenance_Task(); assert(g_maintenance.state.rtc.day == 2);
    }
    /* Oversized frame and recovery. */
    for (i = 0; i < 1100; ++i)
    {
        uint8_t byte = (i == 0U) ? 0xEE : 0x55;
        enqueue(&byte, 1); A_Maintenance_Task();
    }
    sample(2024,3,3); assert(g_maintenance.state.rtc_valid);
}

static void test_storage_failures(void)
{
    int cut;
    fresh(); sample(2026,1,1);
    memcpy(baseline, eeprom, sizeof(eeprom));
    /* Interrupt every byte of invalidate/payload/commit writes. */
    for (cut = 0; cut < 62; ++cut)
    {
        memcpy(eeprom, baseline, sizeof(eeprom)); fail_after = -1;
        reboot(); sample(2026,1,31); fail_after = cut;
        assert(A_Maintenance_SetPeriodDays(150) == MAINTENANCE_STORAGE_ERROR);
        assert(g_maintenance.save.machine.period_days == 180);
        fail_after = -1; reboot(); sample(2026,2,10);
        assert(g_maintenance.save.machine.period_days == 180 && g_maintenance.state.machine.remaining_days == 140);
    }
    memcpy(eeprom, baseline, sizeof(eeprom)); reboot(); sample(2026,1,31);
    assert(A_Maintenance_SetPeriodDays(150) == MAINTENANCE_OK);
    /* Latest payload damaged: fall back to earlier complete record. */
    eeprom[MAINTENANCE_EEPROM_SLOT1 + 20] ^= 0x80;
    reboot(); sample(2026,2,10); assert(g_maintenance.save.machine.period_days == 180);
    eeprom[MAINTENANCE_EEPROM_SLOT0] ^= 1;
    reboot(); sample(2026,2,10);
    assert(g_maintenance.state.storage_corrupt && !g_maintenance.state.countdown_valid);
    assert(A_Maintenance_SetPeriodDays(150) == MAINTENANCE_STORAGE_CORRUPT);
    assert(A_Maintenance_Reset() == MAINTENANCE_OK && g_maintenance.state.machine.remaining_days == 180);
    read_failure = true; reboot(); sample(2026,2,11);
    assert(g_maintenance.state.storage_fault && !g_maintenance.state.countdown_valid);
    read_failure = false; now_ms += 5000; A_Maintenance_Task(); sample(2026,2,11);
    assert(g_maintenance.state.machine.remaining_days == 179);
    /* Saved date survives power loss and detects backward RTC at reboot. */
    reboot(); sample(2026,1,1); assert(!g_maintenance.state.rtc_valid);
    /* A torn first-ever record is not silently treated as a blank device. */
    fresh(); fail_after = 4; sample(2026,1,1); fail_after = -1;
    reboot(); sample(2026,1,2); assert(g_maintenance.state.storage_corrupt);
    /* A recoverable I/O failure must not permit reset with stale RTC. */
    now_ms += 4000; assert(A_Maintenance_Reset() == MAINTENANCE_TIME_ERROR);
}

static void test_tick_wrap_and_display(void)
{
    fresh(); sample(2026,1,1);
    /* Passing the counter origin again must not restart the screen boot wait. */
    now_ms = 100U;
    g_maintenance.state.last_rtc_ms = UINT32_MAX - 200U;
    g_maintenance.state.last_rtc_request_ms = UINT32_MAX - 1500U;
    A_Maintenance_Task(); assert(g_maintenance.state.rtc_pending);
    sample(2026,1,1);
    now_ms = UINT32_MAX - 100U;
    g_maintenance.state.started_ms = now_ms - 4000U;
    g_maintenance.state.last_rtc_ms = now_ms;
    g_maintenance.state.last_rtc_request_ms = now_ms;
    now_ms += 1001U;
    sample(2026,1,2); assert(g_maintenance.state.machine.remaining_days == 179);
    now_ms += 1000U; A_Maintenance_Task();
    sample(2026,1,2);
    A_Maintenance_Task(); A_Maintenance_Task(); A_Maintenance_Task();
    if (MAINTENANCE_SCREEN_ID == 0xFFFFU) { assert(displays == 0); }
    else
    {
        assert(displays > 0);
        assert(last_display[3] == (MAINTENANCE_SCREEN_ID >> 8));
        assert(last_display[4] == (MAINTENANCE_SCREEN_ID & 0xFF));
        assert(last_display_length >= 11);
        assert(last_display[last_display_length - 3] == 0xFC);
    }
    fresh(); init_failure = true; reboot(); assert(g_maintenance.state.status == MAINTENANCE_HARDWARE_FAULT);
}

static void test_reused_pages_and_sequence(void)
{
    int cut;
    fresh(); sample(2026,1,1);
    for (cut = 0; cut < 40; ++cut) { assert(A_Maintenance_SetPeriodDays((uint16_t) (200 + cut)) == MAINTENANCE_OK); }
    assert(A_Maintenance_SetPeriodDays(170) == MAINTENANCE_OK);
    assert(A_Maintenance_SetPeriodDays(160) == MAINTENANCE_OK);
    memcpy(baseline, eeprom, sizeof(eeprom));
    for (cut = 0; cut < 62; ++cut)
    {
        memcpy(eeprom, baseline, sizeof(eeprom)); fail_after = -1;
        reboot(); sample(2026,1,31); fail_after = cut;
        assert(A_Maintenance_SetPeriodDays(150) == MAINTENANCE_STORAGE_ERROR);
        fail_after = -1; reboot(); sample(2026,2,10);
        assert(g_maintenance.save.machine.period_days == 160 && g_maintenance.state.machine.remaining_days == 120);
    }
    /* Simulate the final readback failing after the marker was committed. */
    fresh(); sample(2026,1,1);
    fail_read_after = 2;
    assert(A_Maintenance_SetPeriodDays(150) == MAINTENANCE_STORAGE_ERROR);
    assert(g_maintenance.save.machine.period_days == 180);
    now_ms += 5000; A_Maintenance_Task(); sample(2026,1,31);
    assert(g_maintenance.save.machine.period_days == 150 && g_maintenance.state.machine.remaining_days == 120);
    /* Seed adjacent large sequence numbers, then cross UINT32_MAX. */
    memset(eeprom + MAINTENANCE_EEPROM_JOURNAL_BASE, 0xFF,
        MAINTENANCE_EEPROM_SLOT_COUNT * MAINTENANCE_EEPROM_PAGE_SIZE);
    g_maintenance.save.sequence = UINT32_MAX - 2U;
    assert(A_Maintenance_SetPeriodDays(151) == MAINTENANCE_OK);
    assert(A_Maintenance_SetPeriodDays(152) == MAINTENANCE_OK);
    reboot(); sample(2026,1,31); assert(g_maintenance.save.sequence == UINT32_MAX);
    assert(A_Maintenance_SetPeriodDays(153) == MAINTENANCE_OK && g_maintenance.save.sequence == 0);
    reboot(); sample(2026,1,31); assert(g_maintenance.save.machine.period_days == 153 && g_maintenance.save.sequence == 0);
}

static void test_independent_items(void)
{
    uint32_t machine_start, sensor_start;
    int cut;
    fresh(); sample(2026,1,1);
    machine_start = g_maintenance.save.machine.start_calendar_seconds;
    assert(A_Maintenance_SetSensorPeriodDays(90) == MAINTENANCE_OK);
    sample(2026,1,31);
    assert(g_maintenance.state.machine.remaining_days == 150);
    assert(g_maintenance.state.sensor.remaining_days == 60);
    assert(g_maintenance.state.machine.progress_percent == 16);
    assert(g_maintenance.state.sensor.progress_percent == 33);
    assert(A_Maintenance_SetSensorPeriodDays(60) == MAINTENANCE_OK);
    assert(g_maintenance.save.sensor.start_calendar_seconds == machine_start);
    assert(g_maintenance.state.sensor.remaining_days == 30);
    assert(A_Maintenance_ResetSensor() == MAINTENANCE_OK);
    sensor_start = g_maintenance.save.sensor.start_calendar_seconds;
    assert(sensor_start == machine_start + 30U * 86400U && g_maintenance.save.machine.start_calendar_seconds == machine_start);
    reboot(); sample(2026,2,10);
    assert(g_maintenance.state.machine.remaining_days == 140);
    assert(g_maintenance.state.sensor.remaining_days == 50);
    assert(A_Maintenance_Reset() == MAINTENANCE_OK);
    assert(g_maintenance.save.sensor.start_calendar_seconds == sensor_start);
    assert(g_maintenance.state.machine.remaining_days == 180);
    assert(A_Maintenance_SetSensorPeriodDays(10) == MAINTENANCE_OK);
    assert(g_maintenance.state.sensor.status == MAINTENANCE_DUE);
    assert(g_maintenance.state.machine.status == MAINTENANCE_RUNNING);
    assert(g_maintenance.state.status == MAINTENANCE_DUE);
    assert(g_maintenance.state.sensor.progress_percent == 100);
    memcpy(baseline, eeprom, sizeof(eeprom));
    for (cut = 0; cut < 62; ++cut)
    {
        memcpy(eeprom, baseline, sizeof(eeprom)); fail_after = -1;
        reboot(); sample(2026,2,10); fail_after = cut;
        assert(A_Maintenance_SetSensorPeriodDays(120) == MAINTENANCE_STORAGE_ERROR);
        fail_after = -1; reboot(); sample(2026,2,11);
        assert(g_maintenance.save.sensor.period_days == 10);
        assert(g_maintenance.save.sensor.start_calendar_seconds == sensor_start);
        assert(g_maintenance.state.machine.remaining_days == 179);
    }
}

static void legacy_record(void)
{
    uint8_t *p = &eeprom[MAINTENANCE_EEPROM_SLOT0];
    uint32_t crc = UINT32_MAX;
    unsigned i, bit;
    /* Turn a real record into the exact previous v1 format, with a valid CRC. */
    p[4] = 1; p[6] = 32;
    F_Maintenance_Put32(p + 12, F_Maintenance_Get32(p + 12) / 86400U);
    F_Maintenance_Put32(p + 16, F_Maintenance_Get32(p + 16) / 86400U);
    memset(p + 22, 0, 6);
    for (i = 0; i < 28; ++i)
    {
        crc ^= p[i];
        for (bit = 0; bit < 8; ++bit) { crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0); }
    }
    crc = ~crc;
    for (i = 0; i < 4; ++i) { p[28 + i] = (uint8_t) (crc >> (8 * i)); }
}

static void test_legacy_migration(void)
{
    uint32_t start;
    int cut;
    fresh(); sample(2026,1,1); start = g_maintenance.save.machine.start_calendar_seconds;
    legacy_record(); memcpy(baseline, eeprom, sizeof(eeprom));
    for (cut = 0; cut < 62; ++cut)
    {
        memcpy(eeprom, baseline, sizeof(eeprom)); fail_after = -1; reboot();
        assert(g_maintenance.state.migration_pending);
        fail_after = cut; sample(2026,1,31);
        assert(g_maintenance.state.storage_fault && !g_maintenance.state.countdown_valid);
        assert(eeprom[MAINTENANCE_EEPROM_SLOT0 + 4] == 1);
        fail_after = -1; reboot(); sample(2026,2,10);
        assert(!g_maintenance.state.migration_pending);
        assert(g_maintenance.save.machine.start_calendar_seconds == start);
        assert(g_maintenance.state.machine.remaining_days == 140);
        assert(g_maintenance.state.sensor.remaining_days == MAINTENANCE_SENSOR_DEFAULT_DAYS);
        assert(eeprom[MAINTENANCE_EEPROM_SLOT1 + 4] == 4);
    }
    reboot(); sample(2026,2,11);
    assert(g_maintenance.state.machine.remaining_days == 139);
    assert(g_maintenance.state.sensor.remaining_days == MAINTENANCE_SENSOR_DEFAULT_DAYS - 1);
}

static void check_text(unsigned control, const char *value)
{
    assert(screen_lengths[control] == strlen(value) + 11);
    assert(memcmp(screen_frames[control] + 7, value, strlen(value)) == 0);
}

static void refresh_display(void)
{
    unsigned i;
    g_maintenance.state.display_field = 0;
    g_maintenance.state.display_ms = now_ms - MAINTENANCE_DISPLAY_PERIOD_MS;
    for (i = 0; i < 12; ++i) { A_Maintenance_Task(); }
}

static void test_screen_values(void)
{
    if (MAINTENANCE_SCREEN_ID == 0xFFFFU) { return; }
    fresh(); sample(2026,1,1);
    refresh_display();
    check_text(1, "180\xCC\xEC"); check_text(2, "0\xCC\xEC/");
    check_text(3, "0\xCC\xEC/"); check_text(4, "180\xCC\xEC");
    assert(screen_frames[5][10] == 0 && screen_frames[6][10] == 0);
    /* A same-day restart restores the saved origin and still displays zero. */
    reboot(); sample(2026,1,1); refresh_display();
    check_text(2, "0\xCC\xEC/"); check_text(3, "0\xCC\xEC/");
    sample(2026,1,31);
    assert(A_Maintenance_SetSensorPeriodDays(60) == MAINTENANCE_OK);
    refresh_display();
    check_text(1, "180\xCC\xEC"); check_text(2, "30\xCC\xEC/");
    check_text(3, "30\xCC\xEC/"); check_text(4, "60\xCC\xEC");
    assert(screen_lengths[5] == 15 && screen_lengths[6] == 15);
    assert(memcmp(screen_frames[5] + 7, "\0\0\0\20", 4) == 0); /* 16% */
    assert(memcmp(screen_frames[6] + 7, "\0\0\0\62", 4) == 0); /* 50% */
    sample(2027,1,1); refresh_display();
    check_text(2, "365\xCC\xEC/"); check_text(3, "365\xCC\xEC/");
    assert(screen_frames[5][10] == 100 && screen_frames[6][10] == 100);
    F_Maintenance_RejectRtc(&g_maintenance); refresh_display();
    check_text(2, "--\xCC\xEC/"); check_text(3, "--\xCC\xEC/");
    assert(screen_frames[5][10] == 0 && screen_frames[6][10] == 0);
}

static void test_reset_all_request(void)
{
    uint32_t old_start;
    unsigned old_writes, i;
    int cut;
    fresh(); sample(2020,1,1);
    assert(g_maintenance.save.machine.start_calendar_seconds == (7305U * 86400U + 45296U));
    sample(2026,9,22);
    assert(g_maintenance.state.current_day == 9761);
    assert(g_maintenance.state.machine.elapsed_days == 2456);
    old_start = g_maintenance.save.machine.start_calendar_seconds;
    /* Reproduce the reported old origin; RTC must be fresh before resetting. */
    now_ms += MAINTENANCE_RTC_FRESH_MS + 1;
    g_maintenance.state.reset_all_request = true;
    A_Maintenance_Task();
    assert(g_maintenance.state.reset_all_request && g_maintenance.save.machine.start_calendar_seconds == old_start);
    sample(2026,9,22);
    assert(!g_maintenance.state.reset_all_request && g_maintenance.state.reset_all_result == MAINTENANCE_OK);
    assert(g_maintenance.save.machine.start_calendar_seconds == (9761U * 86400U + 45296U) && g_maintenance.save.sensor.start_calendar_seconds == (9761U * 86400U + 45296U));
    assert(g_maintenance.save.machine.period_days == 180 && g_maintenance.save.sensor.period_days == 180);
    assert(g_maintenance.state.machine.elapsed_days == 0 && g_maintenance.state.sensor.elapsed_days == 0);
    if (MAINTENANCE_SCREEN_ID != 0xFFFFU)
    {
        refresh_display(); check_text(2, "0\xCC\xEC/"); check_text(1, "180\xCC\xEC");
        check_text(3, "0\xCC\xEC/"); check_text(4, "180\xCC\xEC");
    }
    old_writes = writes;
    for (i = 0; i < 20; ++i) { A_Maintenance_Task(); }
    assert(writes == old_writes);
    reboot(); sample(2026,9,23);
    assert(!g_maintenance.state.reset_all_request && writes == old_writes);
    assert(g_maintenance.state.machine.elapsed_days == 1 && g_maintenance.state.sensor.elapsed_days == 1);
    assert(A_Maintenance_SetSensorPeriodDays(90) == MAINTENANCE_OK);
    memcpy(baseline, eeprom, sizeof(eeprom));
    /* Atomic two-item reset: every torn write leaves both old origins intact. */
    for (cut = 0; cut < 62; ++cut)
    {
        memcpy(eeprom, baseline, sizeof(eeprom)); fail_after = -1;
        reboot(); sample(2026,9,24);
        fail_after = cut; g_maintenance.state.reset_all_request = true; A_Maintenance_Task();
        assert(!g_maintenance.state.reset_all_request);
        assert(g_maintenance.state.reset_all_result == MAINTENANCE_STORAGE_ERROR);
        old_writes = writes;
        for (i = 0; i < 20; ++i) { A_Maintenance_Task(); }
        assert(writes == old_writes);
        fail_after = -1; reboot(); sample(2026,9,24);
        assert(g_maintenance.save.machine.start_calendar_seconds == (9761U * 86400U + 45296U) && g_maintenance.save.sensor.start_calendar_seconds == (9761U * 86400U + 45296U));
        assert(g_maintenance.save.machine.period_days == 180 && g_maintenance.save.sensor.period_days == 90);
    }
    g_maintenance.state.reset_all_request = true; A_Maintenance_Task();
    assert(g_maintenance.state.reset_all_result == MAINTENANCE_OK);
    assert(g_maintenance.save.machine.start_calendar_seconds == (9763U * 86400U + 45296U) && g_maintenance.save.sensor.start_calendar_seconds == (9763U * 86400U + 45296U));
    assert(g_maintenance.save.machine.period_days == 180 && g_maintenance.save.sensor.period_days == 90);
}

static void advance_on(uint32_t seconds)
{
    /* The calendar stays fixed to distinguish powered-on time from RTC days. */
    while (seconds != 0U)
    {
        uint32_t step = (seconds > 86400U) ? 86400U : seconds;
        now_ms += step * 1000U;
        A_Maintenance_Task(); sample(2026,1,31);
        seconds -= step;
    }
}

static void test_operating_hours(void)
{
    uint32_t before, saved;
    unsigned old_writes, i;
    fresh(); sample(2026,1,1);
    assert(g_maintenance.save.machine.period_hours == 8000 && g_maintenance.save.sensor.period_hours == 8000);
    before = g_maintenance.state.uptime_seconds;
    old_writes = writes;
    for (i = 0; i < 1000; ++i) { ++now_ms; A_Maintenance_Task(); }
    assert(g_maintenance.state.uptime_seconds == before + 1);
    assert(writes == old_writes);
    now_ms = 62000U; A_Maintenance_Task();
    assert(g_maintenance.save.uptime_seconds == 62U && writes == old_writes + 3U);
    saved = g_maintenance.save.uptime_seconds;
    now_ms += 59000U; A_Maintenance_Task();
    assert(g_maintenance.save.uptime_seconds == saved);
    /* Power off for days: only startup seconds are added on the next boot. */
    reboot(); sample(2026,1,31);
    assert(g_maintenance.state.uptime_seconds == saved + 2U);
    assert(g_maintenance.state.machine.elapsed_days == 30U);
    assert(g_maintenance.state.machine.used_hours == 0U);
    /* Counter wrap must preserve subsecond accumulation. */
    before = g_maintenance.state.uptime_seconds;
    now_ms = UINT32_MAX - 500U;
    g_maintenance.state.uptime_last_ms = now_ms;
    g_maintenance.state.last_rtc_ms = now_ms;
    now_ms += 1501U; A_Maintenance_Task();
    assert(g_maintenance.state.uptime_seconds == before + 1U);
    assert(g_maintenance.state.uptime_remainder_ms == 501U);

    fresh(); sample(2026,1,1); sample(2026,1,31);
    advance_on(4000U * 3600U);
    assert(g_maintenance.state.machine.used_hours == 4000U);
    assert(g_maintenance.state.machine.day_percent == 16U);
    assert(g_maintenance.state.machine.hour_percent == 50U && g_maintenance.state.machine.progress_percent == 50U);
    assert(A_Maintenance_SetPeriodDays(40) == MAINTENANCE_OK);
    assert(g_maintenance.state.machine.progress_percent == 75U); /* days dominate */
    assert(A_Maintenance_SetPeriodDays(180) == MAINTENANCE_OK);
    assert(A_Maintenance_SetPeriodHours(0) == MAINTENANCE_INVALID_ARGUMENT);
    assert(A_Maintenance_SetPeriodHours(16000) == MAINTENANCE_OK);
    assert(g_maintenance.state.machine.hour_percent == 25U && g_maintenance.state.sensor.hour_percent == 50U);
    assert(A_Maintenance_SetPeriodHours(8000) == MAINTENANCE_OK);
    advance_on(4001U * 3600U);
    assert(g_maintenance.state.machine.used_hours == 8001U);
    assert(g_maintenance.state.machine.progress_percent == 100U && g_maintenance.state.machine.status == MAINTENANCE_DUE);
    if (MAINTENANCE_SCREEN_ID != 0xFFFFU)
    {
        refresh_display(); check_text(17, "8001h/8000h"); check_text(18, "8001h/8000h");
        assert(bar_colors[5] == MAINTENANCE_COLOR_DUE && bar_colors[6] == MAINTENANCE_COLOR_DUE);
    }
    assert(A_Maintenance_Reset() == MAINTENANCE_OK);
    assert(g_maintenance.state.machine.used_seconds == 0 && g_maintenance.state.machine.elapsed_days == 0);
    assert(g_maintenance.state.sensor.used_hours == 8001 && g_maintenance.state.sensor.elapsed_days == 30);
    if (MAINTENANCE_SCREEN_ID != 0xFFFFU)
    {
        refresh_display(); check_text(17, "0h/8000h");
        assert(bar_colors[5] == MAINTENANCE_COLOR_NORMAL && bar_colors[6] == MAINTENANCE_COLOR_DUE);
    }
    assert(A_Maintenance_ResetSensor() == MAINTENANCE_OK);
    sample(2026,8,1); /* 182 calendar days after Jan 31, but no extra operating hours. */
    assert(g_maintenance.state.machine.elapsed_days == 182 && g_maintenance.state.machine.used_hours == 0);
    if (MAINTENANCE_SCREEN_ID != 0xFFFFU)
    {
        refresh_display(); check_text(2, "182\xCC\xEC/"); check_text(1, "180\xCC\xEC");
        assert(bar_colors[5] == MAINTENANCE_COLOR_DUE && screen_frames[5][10] == 100);
    }
}

static void test_hours_recovery(void)
{
    uint32_t saved, start;
    unsigned i;
    fresh(); sample(2026,1,31);
    advance_on(125U); saved = g_maintenance.save.uptime_seconds;
    read_failure = true; reboot();
    now_ms += 10000U; A_Maintenance_Task();
    read_failure = false; now_ms += 5000U; A_Maintenance_Task(); sample(2026,1,31);
    assert(g_maintenance.state.uptime_seconds == saved + now_ms / 1000U);
    saved = g_maintenance.state.uptime_seconds;
    fail_read_after = 2;
    assert(A_Maintenance_SetPeriodHours(9000) == MAINTENANCE_STORAGE_ERROR);
    now_ms += 5000U; A_Maintenance_Task(); sample(2026,1,31);
    assert(g_maintenance.save.machine.period_hours == 9000);
    assert(g_maintenance.state.uptime_seconds == saved + 5U);
    start = g_maintenance.save.machine.start_calendar_seconds;
    for (i = 0; i < 80; ++i) { advance_on(60U); }
    assert(g_maintenance.save.sequence > MAINTENANCE_EEPROM_SLOT_COUNT);
    for (i = 0; i < MAINTENANCE_EEPROM_SLOT_COUNT; ++i)
    { assert(eeprom[MAINTENANCE_EEPROM_JOURNAL_BASE + i * 64U + 63U] == 0xA5); }
    saved = g_maintenance.save.uptime_seconds;
    reboot(); sample(2026,1,31);
    assert(g_maintenance.state.uptime_seconds == saved + 2U && g_maintenance.save.machine.start_calendar_seconds == start);
}

static void test_v2_migration(void)
{
    unsigned slot, i, bit;
    uint32_t machine_start, sensor_start;
    fresh(); sample(2026,1,1); machine_start = g_maintenance.save.machine.start_calendar_seconds;
    sample(2026,1,15); assert(A_Maintenance_ResetSensor() == MAINTENANCE_OK);
    sensor_start = g_maintenance.save.sensor.start_calendar_seconds;
    for (slot = 0; slot < 2; ++slot)
    {
        uint8_t *p = &eeprom[MAINTENANCE_EEPROM_SLOT0 + slot * 64];
        uint32_t crc = UINT32_MAX;
        p[4] = 2; p[6] = 32;
        F_Maintenance_Put32(p + 12, F_Maintenance_Get32(p + 12) / 86400U);
        F_Maintenance_Put32(p + 16, F_Maintenance_Get32(p + 16) / 86400U);
        F_Maintenance_Put32(p + 22, F_Maintenance_Get32(p + 22) / 86400U);
        for (i = 0; i < 28; ++i)
        {
            crc ^= p[i];
            for (bit = 0; bit < 8; ++bit) { crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320UL : 0); }
        }
        crc = ~crc;
        for (i = 0; i < 4; ++i) { p[28 + i] = (uint8_t) (crc >> (8 * i)); }
    }
    reboot(); assert(g_maintenance.state.migration_pending && g_maintenance.state.loaded_version == 2);
    sample(2026,1,31);
    assert(g_maintenance.state.loaded_version == 4 && !g_maintenance.state.migration_pending);
    assert(g_maintenance.save.machine.start_calendar_seconds == machine_start && g_maintenance.save.sensor.start_calendar_seconds == sensor_start);
    assert(g_maintenance.state.machine.elapsed_days == 30 && g_maintenance.state.sensor.elapsed_days == 16);
    assert(g_maintenance.state.machine.used_hours == 0 && g_maintenance.state.sensor.used_hours == 0);
    /* RTC outage must not suspend on-time accumulation or its checkpoint. */
    now_ms += 65000U; A_Maintenance_Task();
    assert(!g_maintenance.state.rtc_valid && g_maintenance.state.machine.hours_valid);
    assert(g_maintenance.save.uptime_seconds >= 67U);
    reboot(); sample(2026,2,1);
    assert(g_maintenance.state.machine.used_seconds >= 69U);
}

// F 层仍支持显式上下文传参；操作另一上下文不得改变 A 层私有实例。
// 假硬件共用 EEPROM 和时基，本用例检查状态路由，不表示 RA 可同时运行多个硬件实例。
static void test_context_isolation(void)
{
    Maintenance_Context g_other = {0};
    Maintenance_Context g_snapshot;
    uint8_t byte;
    const uint8_t packet[] = {0xEE, 0x82};

    fresh();
    sample(2026, 1, 31);
    enqueue(packet, sizeof(packet));
    g_snapshot = g_maintenance;

    F_Maintenance_Task(&g_other);
    assert(g_other.state.initialized && g_other.state.record_valid);
    assert(F_Maintenance_SetPeriod(&g_other, 150U, false, false) == MAINTENANCE_TIME_ERROR);
    assert(!H_Maintenance_PortReadByte(&g_other.port, &byte));
    assert(memcmp(&g_snapshot, &g_maintenance, sizeof(g_snapshot)) == 0);

    g_other.port.rx_fault = true;
    assert(H_Maintenance_PortRxFault(&g_other.port));
    assert(!g_maintenance.port.rx_fault);
    assert(H_Maintenance_PortReadByte(&g_maintenance.port, &byte) && byte == 0xEE);
    assert(H_Maintenance_PortReadByte(&g_maintenance.port, &byte) && byte == 0x82);
}

/*
 * 函数名：test_module_entry
 * 说明：验证无参数入口共用私有实例，复位前置条件不变，重复调度不会重复初始化
 * 输入：无
 * 输出：无；断言失败时终止测试
 * 使用：主机回归测试，假硬件记录初始化次数和 EEPROM 写入次数
 */
static void test_module_entry(void)
{
    unsigned saved_writes;
    memset(&g_maintenance, 0, sizeof(g_maintenance));
    writes = 0;
    port_initializations = 0;
    assert(A_Maintenance_Reset() == MAINTENANCE_NOT_READY);
    assert(A_Maintenance_ResetSensor() == MAINTENANCE_NOT_READY);
    assert(writes == 0 && port_initializations == 0);

    fresh();
    assert(port_initializations == 1);
    assert(A_Maintenance_Reset() == MAINTENANCE_TIME_ERROR);
    sample(2026, 1, 1);
    assert(A_Maintenance_SetPeriodDays(150U) == MAINTENANCE_OK);
    saved_writes = writes;
    A_Maintenance_Task();
    A_Maintenance_Task();
    assert(port_initializations == 1 && writes == saved_writes);
    assert(g_maintenance.save.machine.period_days == 150U);

    sample(2026, 1, 2);
    assert(A_Maintenance_Reset() == MAINTENANCE_OK);
    assert(g_maintenance.state.machine.elapsed_days == 0U);
    assert(g_maintenance.state.sensor.elapsed_days == 1U);
    assert(A_Maintenance_ResetSensor() == MAINTENANCE_OK);
    assert(g_maintenance.state.sensor.elapsed_days == 0U);
    assert(port_initializations == 1);
}


/*
 * 函数名：sample_at
 * 说明：注入秒级 RTC 回包，覆盖原来仅按日期注入无法检查的边界
 * 输入：g_date：完整 RTC 时间
 * 输出：无；更新假硬件和模块状态，断言失败时终止测试
 * 使用：仅本测试文件调用
 */
static void sample_at(Maintenance_Date g_date)
{
    uint8_t packet[] = {0xEE,0xF7,0,0,0,0,0,0,0,0xFF,0xFC,0xFF,0xFF};
    if (!g_maintenance.state.rtc_pending) { now_ms += 1000U; A_Maintenance_Task(); }
    assert(g_maintenance.state.rtc_pending);
    packet[2] = bcd(g_date.year - 2000U); packet[3] = bcd(g_date.month); packet[5] = bcd(g_date.day);
    packet[6] = bcd(g_date.hour); packet[7] = bcd(g_date.minute); packet[8] = bcd(g_date.second);
    enqueue(packet, sizeof(packet)); A_Maintenance_Task();
}

/*
 * 函数名：test_elapsed_seconds
 * 说明：验证跨午夜、累计 24 小时边界、秒级进度和重启补时
 * 输入：无
 * 输出：无；断言验证新计时规则
 * 使用：主机回归测试
 */
static void test_elapsed_seconds(void)
{
    Maintenance_Date g_date = {2024,2,29,23,59,59};
    uint32_t origin;
    fresh(); sample_at(g_date); origin = g_maintenance.save.machine.start_calendar_seconds;
    assert(A_Maintenance_SetPeriodDays(1) == MAINTENANCE_OK);
    g_date.month = 3; g_date.day = 1; g_date.hour = 0; g_date.minute = 0; g_date.second = 0;
    sample_at(g_date);
    assert(g_maintenance.state.machine.elapsed_days == 0 && g_maintenance.state.machine.remaining_days == 1);
    g_date.hour = 11; g_date.minute = 59; g_date.second = 59; sample_at(g_date);
    assert(g_maintenance.state.machine.elapsed_days == 0 && g_maintenance.state.machine.day_percent == 50);
    assert(A_Maintenance_SetSensorPeriodDays(90) == MAINTENANCE_OK);
    reboot(); g_date.hour = 23; g_date.second = 58; sample_at(g_date);
    assert(g_maintenance.state.calendar_seconds - origin == 86399U);
    assert(g_maintenance.state.machine.elapsed_days == 0 && g_maintenance.state.machine.day_percent == 99);
    g_date.second = 59; sample_at(g_date);
    assert(g_maintenance.state.machine.elapsed_days == 1 && g_maintenance.state.machine.remaining_days == 0);
    assert(g_maintenance.state.machine.status == MAINTENANCE_DUE);
    assert(A_Maintenance_ResetSensor() == MAINTENANCE_OK);
    g_date.day = 2; g_date.hour = 0; g_date.minute = 0; g_date.second = 0; sample_at(g_date);
    assert(g_maintenance.state.sensor.elapsed_days == 0 && g_maintenance.state.machine.elapsed_days == 1);
}

/*
 * 函数名：calibrate
 * 说明：完成一次受控校时并验证下发帧、双项目起点、累计秒数和持久化结果
 * 输入：g_date：校时目标日期时间
 * 输出：无；断言检查 RTC 跳变未计入保养时间
 * 使用：仅本测试文件调用
 */
static void calibrate(Maintenance_Date g_date)
{
    uint32_t before = g_maintenance.state.calendar_seconds;
    uint32_t start = g_maintenance.save.machine.start_calendar_seconds;
    uint32_t sensor_start = g_maintenance.save.sensor.start_calendar_seconds;
    uint32_t begin_ms = g_maintenance.state.last_rtc_ms;
    uint32_t day;
    assert(A_Maintenance_SetRtc(&g_date) == MAINTENANCE_IN_PROGRESS);
    assert(A_Maintenance_GetRtcSetResult() == MAINTENANCE_IN_PROGRESS);
    assert(A_Maintenance_Reset() == MAINTENANCE_NOT_READY);
    assert(A_Maintenance_ResetSensor() == MAINTENANCE_NOT_READY);
    assert(A_Maintenance_SetPeriodDays(200) == MAINTENANCE_NOT_READY);
    A_Maintenance_Task(); assert(g_maintenance.state.calibration_phase == 2);
    assert(F_Maintenance_DateToDay(&g_date, &day));
    assert(last_rtc_set[2] == bcd(g_date.second) && last_rtc_set[3] == bcd(g_date.minute));
    assert(last_rtc_set[4] == bcd(g_date.hour) && last_rtc_set[5] == bcd(g_date.day));
    assert(last_rtc_set[6] == bcd((day + 6U) % 7U));
    assert(last_rtc_set[7] == bcd(g_date.month) && last_rtc_set[8] == bcd(g_date.year - 2000U));
    sample_at(g_date);
    assert(A_Maintenance_GetRtcSetResult() == MAINTENANCE_OK);
    assert(!g_maintenance.save.calibration_pending && g_maintenance.state.countdown_valid);
    assert(g_maintenance.state.calendar_seconds == before + (now_ms - begin_ms) / 1000U);
    assert(g_maintenance.save.machine.start_calendar_seconds == start);
    assert(g_maintenance.save.sensor.start_calendar_seconds == sensor_start);
}

/*
 * 函数名：test_calibration
 * 说明：验证前调、后调、重复校时、断电补时、串口繁忙与超时恢复
 * 输入：无
 * 输出：无；断言验证校时与累计时间独立
 * 使用：主机回归测试，串口发送由假硬件记录
 */
static void test_calibration(void)
{
    Maintenance_Date g_target = {2099,12,30,12,0,0};
    uint32_t before;
    unsigned count;
    fresh(); sample(2026,1,1); sample(2026,1,31);
    calibrate(g_target);
    assert(g_maintenance.state.machine.elapsed_days == 30 && g_maintenance.state.sensor.elapsed_days == 30);
    g_target.year = 2000; g_target.month = 1; g_target.day = 1;
    calibrate(g_target); calibrate(g_target);
    assert(g_maintenance.state.machine.elapsed_days == 30);
    before = g_maintenance.state.calendar_seconds;
    reboot(); g_target.day = 3; sample_at(g_target);
    assert(g_maintenance.state.calendar_seconds == before + 2U * 86400U);
    assert(g_maintenance.state.machine.elapsed_days == 32);
    assert(A_Maintenance_Reset() == MAINTENANCE_OK);
    assert(g_maintenance.state.machine.elapsed_days == 0 && g_maintenance.state.sensor.elapsed_days == 32);
    g_target.day = 4; sample_at(g_target);
    assert(g_maintenance.state.machine.elapsed_days == 1 && g_maintenance.state.sensor.elapsed_days == 33);
    assert(A_Maintenance_SetRtc(NULL) == MAINTENANCE_INVALID_ARGUMENT);
    g_target.month = 13;
    assert(A_Maintenance_SetRtc(&g_target) == MAINTENANCE_INVALID_ARGUMENT);
    g_target.month = 1; g_target.day = 1;
    before = g_maintenance.state.calendar_seconds; count = rtc_sets;
    assert(A_Maintenance_SetRtc(&g_target) == MAINTENANCE_IN_PROGRESS);
    assert(A_Maintenance_SetRtc(&g_target) == MAINTENANCE_NOT_READY);
    send_busy = true; now_ms += 1500U; A_Maintenance_Task(); assert(rtc_sets == count);
    send_busy = false; A_Maintenance_Task(); assert(rtc_sets == count + 1);
    sample_at(g_target);
    assert(g_maintenance.state.calendar_seconds == before + 2U);
    assert(g_maintenance.state.calibration_remainder_ms == 500U);
    assert(A_Maintenance_SetRtc(&g_target) == MAINTENANCE_IN_PROGRESS);
    A_Maintenance_Task(); now_ms += MAINTENANCE_CALIBRATION_TIMEOUT_MS + 1U; A_Maintenance_Task();
    assert(A_Maintenance_GetRtcSetResult() == MAINTENANCE_TIME_ERROR && g_maintenance.save.calibration_pending);
    sample_at(g_target); assert(!g_maintenance.state.countdown_valid);
    before = g_maintenance.save.calendar_seconds;
    now_ms += 65000U; A_Maintenance_Task();
    assert(g_maintenance.save.calibration_pending && g_maintenance.save.calendar_seconds == before);
    assert(g_maintenance.save.uptime_seconds == g_maintenance.state.uptime_seconds);
    sample_at(g_target);
    calibrate(g_target);
}

/*
 * 函数名：test_calibration_power_cuts
 * 说明：逐字节模拟校时前保存失败、校时后保存失败和最终回读失败
 * 输入：无
 * 输出：无；确认没有已保存意图就不改 RTC，未确认事务重启后不猜测时间差
 * 使用：主机回归测试，不替代 EEPROM 实际断电测试
 */
static void test_calibration_power_cuts(void)
{
    Maintenance_Date g_target = {2000,1,1,12,34,56};
    int cut;
    uint32_t before;
    fresh(); sample(2026,1,1); memcpy(baseline, eeprom, sizeof(eeprom));
    for (cut = 0; cut < 62; ++cut)
    {
        memcpy(eeprom, baseline, sizeof(eeprom)); fail_after = -1; reboot(); sample(2026,1,31);
        rtc_sets = 0; fail_after = cut;
        assert(A_Maintenance_SetRtc(&g_target) == MAINTENANCE_STORAGE_ERROR);
        A_Maintenance_Task(); assert(rtc_sets == 0);
        fail_after = -1; reboot(); sample(2026,2,1);
        assert(g_maintenance.state.machine.elapsed_days == 31 && !g_maintenance.save.calibration_pending);
    }
    for (cut = 0; cut < 62; ++cut)
    {
        memcpy(eeprom, baseline, sizeof(eeprom)); fail_after = -1; reboot(); sample(2026,1,31);
        before = g_maintenance.state.calendar_seconds;
        assert(A_Maintenance_SetRtc(&g_target) == MAINTENANCE_IN_PROGRESS);
        A_Maintenance_Task(); fail_after = cut; sample_at(g_target);
        assert(A_Maintenance_GetRtcSetResult() == MAINTENANCE_STORAGE_ERROR);
        fail_after = -1; reboot(); sample_at(g_target);
        assert(g_maintenance.save.calibration_pending && !g_maintenance.state.countdown_valid);
        assert(g_maintenance.state.calendar_seconds == before);
        calibrate(g_target); assert(g_maintenance.state.machine.elapsed_days == 30);
    }
    fresh(); sample(2026,1,1); sample(2026,1,31);
    fail_read_after = 2;
    assert(A_Maintenance_SetRtc(&g_target) == MAINTENANCE_STORAGE_ERROR);
    A_Maintenance_Task(); assert(rtc_sets == 0);
    reboot(); sample(2026,1,31);
    assert(g_maintenance.save.calibration_pending && !g_maintenance.state.countdown_valid);
    calibrate(g_target);
    assert(A_Maintenance_SetRtc(&g_target) == MAINTENANCE_IN_PROGRESS);
    A_Maintenance_Task(); fail_read_after = 2; sample_at(g_target);
    assert(A_Maintenance_GetRtcSetResult() == MAINTENANCE_STORAGE_ERROR);
    reboot(); g_target.day = 2; sample_at(g_target);
    assert(!g_maintenance.save.calibration_pending && g_maintenance.state.countdown_valid);
    assert(g_maintenance.state.machine.elapsed_days == 31);
    before = g_maintenance.state.calendar_seconds;
    assert(A_Maintenance_SetRtc(&g_target) == MAINTENANCE_IN_PROGRESS);
    reboot(); sample_at(g_target);
    assert(g_maintenance.save.calibration_pending && g_maintenance.state.calendar_seconds == before);
    calibrate(g_target);
}

/*
 * 函数名：test_calibration_input
 * 说明：验证屏幕校时通知的页面、控件、类型、长度、数字和日期校验
 * 输入：无
 * 输出：无；合法通知受理校时，其余通知不得修改 RTC
 * 使用：主机回归测试，同时覆盖禁用屏幕配置
 */
static void test_calibration_input(void)
{
    uint8_t frame[27] = {0xEE,0xB1,0x11,0,0,0,MAINTENANCE_RTC_INPUT_ID,0x11};
    unsigned i, old_writes;
    fresh(); sample(2026,1,1);
    memcpy(frame + 8, "20301231125959", 15); memcpy(frame + 23, tail, 4);
    old_writes = writes;
    for (i = 3; i <= 7; ++i)
    {
        frame[i] ^= 1; enqueue(frame, sizeof(frame)); A_Maintenance_Task(); frame[i] ^= 1;
        assert(writes == old_writes && rtc_sets == 0);
    }
    frame[8] = 'x'; enqueue(frame, sizeof(frame)); A_Maintenance_Task(); frame[8] = '2';
    frame[12] = '9'; enqueue(frame, sizeof(frame)); A_Maintenance_Task(); frame[12] = '1';
    frame[22] = '0'; enqueue(frame, sizeof(frame)); A_Maintenance_Task(); frame[22] = 0;
    enqueue(frame, 20); enqueue(tail, 4); A_Maintenance_Task();
    assert(writes == old_writes && rtc_sets == 0);
    enqueue(frame, sizeof(frame)); A_Maintenance_Task();
    if (MAINTENANCE_SCREEN_ID == 0xFFFFU) { assert(rtc_sets == 0 && writes == old_writes); }
    else
    {
        Maintenance_Date g_target = {2030,12,31,12,59,59};
        assert(rtc_sets == 1 && g_maintenance.save.calibration_pending);
        enqueue(frame, sizeof(frame)); A_Maintenance_Task(); assert(rtc_sets == 1);
        sample_at(g_target);
        assert(A_Maintenance_GetRtcSetResult() == MAINTENANCE_OK && g_maintenance.state.machine.elapsed_days == 0);
    }
}

/*
 * 函数名：test_v3_migration
 * 说明：验证旧版日期记录升级保留整天数及开机小时，未知的日内起点从迁移时开始
 * 输入：无
 * 输出：无；断言验证 v3 到 v4 的字段单位转换
 * 使用：主机回归测试
 */
static void test_v3_migration(void)
{
    uint8_t *p;
    uint32_t uptime;
    Maintenance_Date g_date = {2026,2,1,0,0,0};
    fresh(); sample(2026,1,1); sample(2026,1,31); advance_on(7200U);
    assert(A_Maintenance_ResetSensor() == MAINTENANCE_OK);
    uptime = g_maintenance.save.uptime_seconds;
    p = eeprom + F_Maintenance_SlotAddress((uint8_t) g_maintenance.state.active_slot);
    memmove(baseline, p, 64);
    memset(eeprom, 0xFF, sizeof(eeprom)); p = eeprom + MAINTENANCE_EEPROM_SLOT0;
    memcpy(p, baseline, 64); p[4] = 3; p[6] = 48;
    F_Maintenance_Put32(p + 12, F_Maintenance_Get32(p + 12) / 86400U);
    F_Maintenance_Put32(p + 16, F_Maintenance_Get32(p + 16) / 86400U);
    F_Maintenance_Put32(p + 22, F_Maintenance_Get32(p + 22) / 86400U);
    F_Maintenance_Put32(p + 44, F_Maintenance_Crc32(p, 44));
    reboot(); assert(g_maintenance.state.loaded_version == 3); sample_at(g_date);
    assert(g_maintenance.state.loaded_version == 4 && !g_maintenance.state.migration_pending);
    assert(g_maintenance.state.machine.elapsed_days == 31 && g_maintenance.state.sensor.elapsed_days == 1);
    assert(g_maintenance.state.machine.used_hours == 2 && g_maintenance.state.sensor.used_hours == 0);
    assert(g_maintenance.save.uptime_seconds == uptime + 2);
    g_date.hour = 23; g_date.minute = 59; g_date.second = 59; sample_at(g_date);
    assert(g_maintenance.state.machine.elapsed_days == 31);
    g_date.day = 2; g_date.hour = 0; g_date.minute = 0; g_date.second = 0; sample_at(g_date);
    assert(g_maintenance.state.machine.elapsed_days == 32);
}

int main(void)
{
    test_calendar(); test_lifecycle(); test_protocol(); test_storage_failures(); test_tick_wrap_and_display();
    test_reused_pages_and_sequence();
    test_independent_items(); test_legacy_migration(); test_screen_values();
    test_reset_all_request();
    test_operating_hours(); test_hours_recovery(); test_v2_migration();
    test_context_isolation();
    test_module_entry();
    test_elapsed_seconds(); test_calibration(); test_calibration_power_cuts();
    test_calibration_input(); test_v3_migration();
    puts("PASS: 24-hour boundaries, RTC calibration, input validation, calibration power cuts, v1/v2/v3 migration, uptime, display, journal, reset, module entry");
    return 0;
}
