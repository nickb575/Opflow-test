#pragma once
#include "AfbrS50.h"

/* Host stand-in for the AFBR-S50 driver. The real one links Broadcom's
 * Cortex-M3 library and cannot run natively; this replaces AfbrS50.cpp. */
namespace FakeAfbrS50 {
void Reset();
void Ranging(int32_t rangeMm, uint32_t amplitude, uint8_t quality);
void InitFailed(int32_t argusError);
}
