#include "FakeFlowBus.h"
#include "FlowBus.h"
#include <cstring>

namespace {
uint8_t registers[32][128];
uint8_t burst[12];
uint8_t bank;
unsigned readCount[128];
unsigned writeCount;
uint32_t now, firstWrite, firstBurst, lastError;
bool resetAsserted, present, failRead, failWrite;
uint8_t product, revision, inverse;
}

namespace FakeFlowBus {
void Reset()
{
  std::memset(registers, 0, sizeof(registers));
  std::memset(readCount, 0, sizeof(readCount));
  const uint8_t restingBurst[12] = {0U, 0x3FU, 0U, 0U, 0U, 0U, 40U, 80U, 200U, 10U, 4U, 0U};
  std::memcpy(burst, restingBurst, sizeof(burst));
  bank = 0U; writeCount = 0U; now = firstWrite = firstBurst = lastError = 0U;
  resetAsserted = failRead = failWrite = false;
  present = true; product = 0x49U; revision = 0x01U; inverse = 0xB6U;
}
void SetTime(uint32_t nowMs) { now = nowMs; }
void Present(bool value) { present = value; }
void Identity(uint8_t id, uint8_t rev, uint8_t inv) { product = id; revision = rev; inverse = inv; }
void Burst(const uint8_t* bytes) { std::memcpy(burst, bytes, sizeof(burst)); }
void FailNextRead() { failRead = true; }
void FailNextWrite() { failWrite = true; }
bool ResetAsserted() { return resetAsserted; }
unsigned ReadCount(uint8_t reg) { return readCount[reg & 0x7FU]; }
unsigned WriteCount() { return writeCount; }
uint8_t Register(uint8_t page, uint8_t reg) { return registers[page & 0x1FU][reg & 0x7FU]; }
uint32_t FirstWriteMs() { return firstWrite; }
uint32_t FirstBurstMs() { return firstBurst; }
}

bool FlowBus_Read(uint8_t reg, uint8_t* bytes, uint16_t length)
{
  if (failRead) { failRead = false; lastError = 1U; return false; }
  ++readCount[reg & 0x7FU];
  if (!present || resetAsserted)
  {
    /* No SPI ACK exists: a disconnected slave can clock back all zeroes. */
    std::memset(bytes, 0, length);
    return true;
  }
  if (reg == 0x16U && length == sizeof(burst))
  {
    if (readCount[reg] == 1U) { firstBurst = now; }
    std::memcpy(bytes, burst, length);
  }
  else if (length == 1U && bank == 0U && reg == 0x00U) { *bytes = product; }
  else if (length == 1U && bank == 0U && reg == 0x01U) { *bytes = revision; }
  else if (length == 1U && bank == 0U && reg == 0x5FU) { *bytes = inverse; }
  else { std::memcpy(bytes, registers[bank] + reg, length); }
  return true;
}

bool FlowBus_Write(uint8_t reg, uint8_t value)
{
  if (failWrite) { failWrite = false; lastError = 2U; return false; }
  if (++writeCount == 1U) { firstWrite = now; }
  if (!present || resetAsserted) { return true; }
  if (reg == 0x7FU) { bank = value & 0x1FU; }
  else if (bank == 0U && reg == 0x3AU && value == 0x5AU)
  {
    std::memset(registers, 0, sizeof(registers));
    bank = 0U;
  }
  else { registers[bank][reg & 0x7FU] = value; }
  return true;
}

void FlowBus_SetReset(bool asserted)
{
  resetAsserted = asserted;
  if (asserted) { bank = 0U; }
}
uint32_t FlowBus_LastError() { return lastError; }
