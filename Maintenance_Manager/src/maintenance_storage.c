#include "maintenance_internal.h"
#include <string.h>

/* Legacy little-endian storage layout (32 bytes + marker at page byte 63):
 * 0 magic MNT1; 4 version u16; 6 length u16; 8 sequence u32;
 * 12 machine start_day u32; 16 saved_day u32; 20 machine period u16;
 * v2: 22 sensor start_day u32; 26 sensor period u16; 28 CRC32 of bytes 0..27.
 * v1: 22..27 reserved zero. Migrated after a fresh RTC sample. */
#define RECORD_SIZE 48U
#define COMMIT_OFFSET 63U
#define COMMIT_MARKER 0xA5U

/* v3: bytes 0..27 retain v2 fields; 28 lifetime uptime u32;
 * 32/36 machine/sensor uptime origins u32; 40/42 hour periods u16;
 * 44 CRC32 of bytes 0..43; marker remains at byte 63.
 * Logical slots 0/1 retain legacy addresses for upgrade compatibility. */
#if (MAINTENANCE_EEPROM_SLOT_COUNT < 2U) || (MAINTENANCE_EEPROM_SLOT_COUNT > 127U) || \
    ((MAINTENANCE_EEPROM_JOURNAL_BASE % 64U) != 0U) || \
    ((MAINTENANCE_EEPROM_JOURNAL_BASE + MAINTENANCE_EEPROM_SLOT_COUNT * 64U) > 32768U) || \
    (MAINTENANCE_EEPROM_SLOT0 < MAINTENANCE_EEPROM_JOURNAL_BASE) || \
    (MAINTENANCE_EEPROM_SLOT1 != MAINTENANCE_EEPROM_SLOT0 + 64U) || \
    (MAINTENANCE_EEPROM_SLOT1 >= MAINTENANCE_EEPROM_JOURNAL_BASE + MAINTENANCE_EEPROM_SLOT_COUNT * 64U)
#error Invalid EEPROM journal layout
#endif

static uint16_t slot_address(uint8_t slot)
{
    uint16_t offset = (uint16_t) ((MAINTENANCE_EEPROM_SLOT0 - MAINTENANCE_EEPROM_JOURNAL_BASE) / 64U);
    return (uint16_t) (MAINTENANCE_EEPROM_JOURNAL_BASE +
        ((offset + slot) % MAINTENANCE_EEPROM_SLOT_COUNT) * 64U);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t) (p[0] | ((uint16_t) p[1] << 8)); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t crc32(const uint8_t *data, uint16_t length)
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

static bool erased(const uint8_t *page)
{
    uint16_t i;
    for (i = 0; i < MAINTENANCE_EEPROM_PAGE_SIZE; ++i)
    {
        if (page[i] != 0xFFU) { return false; }
    }
    return true;
}

static bool decode(const uint8_t *page, Maintenance_save_t *save)
{
    uint8_t i;
    uint16_t length = (page[4] == 3U) ? RECORD_SIZE : 32U;
    if ((page[COMMIT_OFFSET] != COMMIT_MARKER) || (memcmp(page, "MNT1", 4) != 0) ||
        (page[4] < 1U) || (page[4] > 3U) || (page[5] != 0U) || (get16(page + 6) != length) ||
        (get32(page + length - 4U) != crc32(page, (uint16_t) (length - 4U)))) { return false; }
    save->uptime_seconds = 0;
    save->machine.start_uptime_seconds = 0;
    save->sensor.start_uptime_seconds = 0;
    save->machine.period_hours = MAINTENANCE_DEFAULT_HOURS;
    save->sensor.period_hours = MAINTENANCE_SENSOR_DEFAULT_HOURS;
    if (page[4] == 3U)
    {
        save->uptime_seconds = get32(page + 28);
        save->machine.start_uptime_seconds = get32(page + 32);
        save->sensor.start_uptime_seconds = get32(page + 36);
        save->machine.period_hours = get16(page + 40);
        save->sensor.period_hours = get16(page + 42);
        if ((save->machine.start_uptime_seconds > save->uptime_seconds) ||
            (save->sensor.start_uptime_seconds > save->uptime_seconds) ||
            (save->machine.period_hours == 0U) || (save->sensor.period_hours == 0U)) { return false; }
    }
    if (page[4] == 1U)
    {
        for (i = 22U; i < 28U; ++i) { if (page[i] != 0U) { return false; } }
        save->sensor.start_day = UINT32_MAX;
        save->sensor.period_days = MAINTENANCE_SENSOR_DEFAULT_DAYS;
    }
    else
    {
        save->sensor.start_day = get32(&page[22]);
        save->sensor.period_days = (uint16_t) ((uint16_t) page[26] | ((uint16_t) page[27] << 8));
        if ((save->sensor.period_days == 0U) || (save->sensor.period_days > MAINTENANCE_MAX_DAYS) ||
            (save->sensor.start_day > get32(&page[16]))) { return false; }
    }
    save->sequence = get32(&page[8]);
    save->machine.start_day = get32(&page[12]);
    save->saved_day = get32(&page[16]);
    save->machine.period_days = (uint16_t) ((uint16_t) page[20] | ((uint16_t) page[21] << 8));
    return (save->machine.period_days > 0U) && (save->machine.period_days <= MAINTENANCE_MAX_DAYS) &&
           (save->machine.start_day <= save->saved_day) && (save->saved_day < 36525U);
}

bool Maintenance_StorageLoad(void)
{
    uint8_t page[MAINTENANCE_EEPROM_PAGE_SIZE];
    Maintenance_save_t record, best;
    int8_t best_slot = -1;
    uint8_t slot, version = 0;
    bool all_erased = true;
    /* Read one page at a time to keep MCU stack usage bounded. */
    for (slot = 0; slot < MAINTENANCE_EEPROM_SLOT_COUNT; ++slot)
    {
        if (!Maintenance_EepromRead(slot_address(slot), page, sizeof(page)))
        {
            Maintenance_para.storage_fault = true;
            Maintenance_para.last_result = MAINTENANCE_STORAGE_ERROR;
            return false;
        }
        if (!erased(page)) { all_erased = false; }
        if (decode(page, &record) && ((best_slot < 0) ||
            (((uint32_t) (record.sequence - best.sequence) != 0U) &&
             ((uint32_t) (record.sequence - best.sequence) < 0x80000000UL))))
        {
            best = record;
            best_slot = (int8_t) slot;
            version = page[4];
        }
    }
    Maintenance_para.storage_loaded = true;
    Maintenance_para.storage_fault = false;
    Maintenance_para.storage_corrupt = false;
    Maintenance_para.storage_blank = false;
    Maintenance_para.record_valid = false;
    Maintenance_para.migration_pending = false;
    Maintenance_para.active_slot = best_slot;
    Maintenance_para.loaded_version = version;
    if (best_slot >= 0)
    {
        Maintenance_save = best;
        Maintenance_para.migration_pending = (version < 3U);
        Maintenance_para.record_valid = true;
        Maintenance_para.last_result = MAINTENANCE_OK;
        return true;
    }
    memset(&Maintenance_save, 0, sizeof(Maintenance_save));
    Maintenance_save.machine.period_days = MAINTENANCE_DEFAULT_DAYS;
    Maintenance_save.sensor.period_days = MAINTENANCE_SENSOR_DEFAULT_DAYS;
    Maintenance_save.machine.period_hours = MAINTENANCE_DEFAULT_HOURS;
    Maintenance_save.sensor.period_hours = MAINTENANCE_SENSOR_DEFAULT_HOURS;
    if (all_erased)
    {
        Maintenance_para.storage_blank = true;
        Maintenance_para.last_result = MAINTENANCE_NOT_READY;
        return true;
    }
    Maintenance_para.storage_corrupt = true;
    Maintenance_para.last_result = MAINTENANCE_STORAGE_CORRUPT;
    return false;
}

bool Maintenance_StorageCommit(Maintenance_save_t *candidate)
{
    uint8_t record[RECORD_SIZE] = {0};
    uint8_t verify[MAINTENANCE_EEPROM_PAGE_SIZE];
    Maintenance_save_t decoded;
    uint8_t marker = 0U;
    uint8_t read_marker;
    int8_t next_slot = (int8_t) ((Maintenance_para.active_slot + 1) % MAINTENANCE_EEPROM_SLOT_COUNT);
    uint16_t address = slot_address((uint8_t) next_slot);
    uint32_t sequence = Maintenance_para.record_valid ? Maintenance_save.sequence + 1U : 1U;
    memcpy(record, "MNT1", 4);
    record[4] = 3U;
    record[6] = RECORD_SIZE;
    put32(&record[8], sequence);
    put32(&record[12], candidate->machine.start_day);
    put32(&record[16], candidate->saved_day);
    record[20] = (uint8_t) candidate->machine.period_days;
    record[21] = (uint8_t) (candidate->machine.period_days >> 8);
    put32(&record[22], candidate->sensor.start_day);
    record[26] = (uint8_t) candidate->sensor.period_days;
    record[27] = (uint8_t) (candidate->sensor.period_days >> 8);
    put32(record + 28, candidate->uptime_seconds);
    put32(record + 32, candidate->machine.start_uptime_seconds);
    put32(record + 36, candidate->sensor.start_uptime_seconds);
    put16(record + 40, candidate->machine.period_hours);
    put16(record + 42, candidate->sensor.period_hours);
    put32(record + 44, crc32(record, 44U));

    /* Never invalidate the currently active page. Invalidate destination first,
     * write/verify payload, then commit marker, then verify the complete record. */
    if (!Maintenance_EepromWrite((uint16_t) (address + COMMIT_OFFSET), &marker, 1U) ||
        !Maintenance_EepromRead((uint16_t) (address + COMMIT_OFFSET), &read_marker, 1U) ||
        (read_marker != 0U) ||
        !Maintenance_EepromWrite(address, record, sizeof(record)) ||
        !Maintenance_EepromRead(address, verify, sizeof(record)) ||
        (memcmp(record, verify, sizeof(record)) != 0)) { return false; }
    marker = COMMIT_MARKER;
    if (!Maintenance_EepromWrite((uint16_t) (address + COMMIT_OFFSET), &marker, 1U) ||
        !Maintenance_EepromRead(address, verify, sizeof(verify)) ||
        !decode(verify, &decoded) || (memcmp(record, verify, sizeof(record)) != 0)) { return false; }
    candidate->sequence = sequence;
    Maintenance_para.active_slot = next_slot;
    return true;
}
