#pragma once
#include <stdint.h>

namespace FakeFlowBus {
void Reset();
void SetTime(uint32_t nowMs);
void Present(bool present);
void Identity(uint8_t product, uint8_t revision, uint8_t inverse);
void Burst(const uint8_t* twelveBytes);
void FailNextRead();
void FailNextWrite();
bool ResetAsserted();
unsigned ReadCount(uint8_t reg);
unsigned WriteCount();
uint8_t Register(uint8_t bank, uint8_t reg);
uint32_t FirstWriteMs();
uint32_t FirstBurstMs();
}
