#ifndef ECE480_FLOW_BUS_H
#define ECE480_FLOW_BUS_H

#include <stdint.h>

/* Exclusive SPI1 owner. Each call finishes within a bounded hardware deadline;
 * multi-millisecond sensor reset/configuration waits belong in the driver. */
bool FlowBus_Read(uint8_t reg, uint8_t* bytes, uint16_t length);
bool FlowBus_Write(uint8_t reg, uint8_t value);
void FlowBus_SetReset(bool asserted);
uint32_t FlowBus_LastError();

#endif
