#ifndef ECE480_IMU_BUS_H
#define ECE480_IMU_BUS_H

#include <stdint.h>

enum class ImuBusResult { Idle, Pending, Complete, Error };
enum class ImuBusStart { Started, Busy, Error };

/* One owner, one transfer; buffers must live until completion or reset. */
bool ImuBus_Reset();
ImuBusStart ImuBus_Read(uint8_t reg, uint8_t* bytes, uint16_t length);
ImuBusStart ImuBus_Write(uint8_t reg, uint8_t* bytes, uint16_t length);
ImuBusResult ImuBus_Result();
uint32_t ImuBus_LastError();

#endif
