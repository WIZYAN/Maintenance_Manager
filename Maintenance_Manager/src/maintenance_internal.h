#ifndef MAINTENANCE_INTERNAL_H
#define MAINTENANCE_INTERNAL_H

#include <stddef.h>
#include "Maintenance.h"

/* Hardware boundary; replaced by a deterministic fake in host tests. */
bool Maintenance_PortInit(void);
uint32_t Maintenance_PortNow(void);
bool Maintenance_PortReadByte(uint8_t *byte);
bool Maintenance_PortRxFault(void);
bool Maintenance_PortSend(const uint8_t *data, uint16_t length);
bool Maintenance_EepromRead(uint16_t address, uint8_t *data, uint16_t length);
bool Maintenance_EepromWrite(uint16_t address, const uint8_t *data, uint16_t length);

bool Maintenance_DateToDay(const Maintenance_date_t *date, uint32_t *day);
bool Maintenance_StorageLoad(void);
bool Maintenance_StorageCommit(Maintenance_save_t *candidate);
void Maintenance_ProtocolTask(uint32_t now);
void Maintenance_AcceptRtc(const Maintenance_date_t *date, uint32_t now);
void Maintenance_RejectRtc(void);
void Maintenance_UpdateStatus(uint32_t now);

#endif
