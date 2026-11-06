#pragma once
#include <stdint.h>

namespace FakeImuBus {
void Reset();
void Advance(uint32_t nowMs);
void Present(bool present);
void Identity(uint8_t identity);
void DataReady(bool enabled);
void Hold(bool hold);
void Busy(bool busy);
void FailNext();
void MismatchConfig(bool mismatch);
uint8_t Register(uint8_t reg);
unsigned Reads(uint8_t reg);
unsigned Resets();
}
