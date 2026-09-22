#include "maintenance_internal.h"
#include <string.h>

/* Explicit little-endian storage layout (32 bytes + marker at page byte 63):
 * 0 magic MNT1; 4 version u16; 6 length u16; 8 sequence u32;
 * 12 machine start_day u32; 16 saved_day u32; 20 machine period u16;
 * v2: 22 sensor start_day u32; 26 sensor period u16; 28 CRC32 of bytes 0..27.
 * v1: 22..27 reserved zero. Migrated after a fresh RTC sample. */
#define RECORD_SIZE 32U
#define COMMIT_OFFSET 63U
#define COMMIT_MARKER 0xA5U

#if ((MAINTENANCE_EEPROM_SLOT0 % 64U) != 0U) || ((MAINTENANCE_EEPROM_SLOT1 % 64U) != 0U) || \
    (MAINTENANCE_EEPROM_SLOT0 == MAINTENANCE_EEPROM_SLOT1) || \
    (MAINTENANCE_EEPROM_SLOT0 > (32768U - 64U)) || (MAINTENANCE_EEPROM_SLOT1 > (32768U - 64U))
#error EEPROM slots must be distinct aligned 64-byte pages within 24C256
#endif

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
    if ((page[COMMIT_OFFSET] != COMMIT_MARKER) || (memcmp(page, "MNT1", 4) != 0) ||
        ((page[4] != 1U) && (page[4] != 2U)) || (page[5] != 0U) || (page[6] != RECORD_SIZE) || (page[7] != 0U) ||
        (get32(&page[28]) != crc32(page, 28U))) { return false; }
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
    uint8_t pages[2][MAINTENANCE_EEPROM_PAGE_SIZE];
    Maintenance_save_t records[2];
    bool valid[2];
    uint8_t slot;
    if (!Maintenance_EepromRead(MAINTENANCE_EEPROM_SLOT0, pages[0], sizeof(pages[0])) ||
        !Maintenance_EepromRead(MAINTENANCE_EEPROM_SLOT1, pages[1], sizeof(pages[1])))
    {
        Maintenance_para.storage_fault = true;
        Maintenance_para.last_result = MAINTENANCE_STORAGE_ERROR;
        return false;
    }
    valid[0] = decode(pages[0], &records[0]);
    valid[1] = decode(pages[1], &records[1]);
    Maintenance_para.storage_loaded = true;
    Maintenance_para.storage_fault = false;
    Maintenance_para.storage_corrupt = false;
    Maintenance_para.storage_blank = false;
    Maintenance_para.record_valid = false;
    Maintenance_para.migration_pending = false;
    Maintenance_para.active_slot = -1;
    if (valid[0] || valid[1])
    {
        /* Modular comparison handles sequence UINT32_MAX -> 0. */
        slot = (!valid[0] || (valid[1] &&
                ((uint32_t) (records[1].sequence - records[0].sequence) != 0U) &&
                ((uint32_t) (records[1].sequence - records[0].sequence) < 0x80000000UL))) ? 1U : 0U;
        Maintenance_save = records[slot];
        Maintenance_para.migration_pending = (pages[slot][4] == 1U);
        Maintenance_para.active_slot = (int8_t) slot;
        Maintenance_para.record_valid = true;
        Maintenance_para.last_result = MAINTENANCE_OK;
        return true;
    }
    memset(&Maintenance_save, 0, sizeof(Maintenance_save));
    Maintenance_save.machine.period_days = MAINTENANCE_DEFAULT_DAYS;
    Maintenance_save.sensor.period_days = MAINTENANCE_SENSOR_DEFAULT_DAYS;
    if (erased(pages[0]) && erased(pages[1]))
    {
        Maintenance_para.storage_blank = true;
        Maintenance_para.last_result = MAINTENANCE_NOT_READY;
        return true;
    }
    /* Damaged/foreign data must never silently restart the maintenance period. */
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
    int8_t next_slot = (Maintenance_para.active_slot == 0) ? 1 : 0;
    uint16_t address = (next_slot == 0) ? MAINTENANCE_EEPROM_SLOT0 : MAINTENANCE_EEPROM_SLOT1;
    uint32_t sequence = Maintenance_para.record_valid ? Maintenance_save.sequence + 1U : 1U;
    memcpy(record, "MNT1", 4);
    record[4] = 2U;
    record[6] = RECORD_SIZE;
    put32(&record[8], sequence);
    put32(&record[12], candidate->machine.start_day);
    put32(&record[16], candidate->saved_day);
    record[20] = (uint8_t) candidate->machine.period_days;
    record[21] = (uint8_t) (candidate->machine.period_days >> 8);
    put32(&record[22], candidate->sensor.start_day);
    record[26] = (uint8_t) candidate->sensor.period_days;
    record[27] = (uint8_t) (candidate->sensor.period_days >> 8);
    put32(&record[28], crc32(record, 28U));

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
