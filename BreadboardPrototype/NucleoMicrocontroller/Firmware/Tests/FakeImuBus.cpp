#include "FakeImuBus.h"
#include "ImuBus.h"
#include <cstring>

namespace {
uint8_t registers[256];
unsigned reads[256];
bool present, ready, hold, busy, fail, mismatch, write;
uint8_t address;
uint8_t* buffer;
uint16_t size;
uint32_t lastSample;
unsigned resets;
ImuBusResult result;

ImuBusStart Start(bool isWrite, uint8_t reg, uint8_t* bytes, uint16_t length)
{
  if (busy || result == ImuBusResult::Pending) { return ImuBusStart::Busy; }
  if (fail) { fail = false; result = ImuBusResult::Error; return ImuBusStart::Error; }
  address = reg; buffer = bytes; size = length; write = isWrite;
  result = ImuBusResult::Pending;
  if (!isWrite) { ++reads[reg]; }
  return ImuBusStart::Started;
}
}

namespace FakeImuBus {
void Reset()
{
  std::memset(registers, 0, sizeof(registers));
  std::memset(reads, 0, sizeof(reads));
  registers[0x75] = 0x68U;
  registers[0x6B] = 0x40U;
  registers[0x3F] = 0x40U; // Stationary Z=+1g; gyro=0.
  present = ready = true;
  hold = busy = fail = mismatch = false;
  result = ImuBusResult::Idle;
  lastSample = 0U;
  resets = 0U;
}

void Advance(uint32_t nowMs)
{
  if (ready && (registers[0x6B] & 0x40U) == 0U &&
      static_cast<uint32_t>(nowMs - lastSample) >= 10U)
  {
    registers[0x3A] |= 1U;
    lastSample = nowMs;
  }
  if (hold || result != ImuBusResult::Pending) { return; }
  if (!present) { result = ImuBusResult::Error; return; }
  if (write)
  {
    if (address == 0x6BU && buffer[0] == 0x80U)
    {
      constexpr uint8_t resetRegisters[] = {0x19, 0x1A, 0x1B, 0x1C, 0x37, 0x38, 0x6C};
      for (uint8_t reg : resetRegisters) { registers[reg] = 0U; }
      registers[0x6B] = 0x40U;
    }
    else { std::memcpy(registers + address, buffer, size); }
  }
  else
  {
    std::memcpy(buffer, registers + address, size);
    if (address == 0x3AU) { registers[0x3A] = 0U; }
    if (mismatch && address == 0x19U) { buffer[0] ^= 1U; }
  }
  result = ImuBusResult::Complete;
}
void Present(bool value) { present = value; }
void Identity(uint8_t value) { registers[0x75] = value; }
void DataReady(bool value) { ready = value; if (!value) { registers[0x3A] = 0U; } }
void Hold(bool value) { hold = value; }
void Busy(bool value) { busy = value; }
void FailNext() { fail = true; }
void MismatchConfig(bool value) { mismatch = value; }
uint8_t Register(uint8_t reg) { return registers[reg]; }
unsigned Reads(uint8_t reg) { return reads[reg]; }
unsigned Resets() { return resets; }
}

bool ImuBus_Reset() { ++resets; result = ImuBusResult::Idle; return true; }
ImuBusStart ImuBus_Read(uint8_t reg, uint8_t* bytes, uint16_t length) { return Start(false, reg, bytes, length); }
ImuBusStart ImuBus_Write(uint8_t reg, uint8_t* bytes, uint16_t length) { return Start(true, reg, bytes, length); }
ImuBusResult ImuBus_Result() { return result; }
uint32_t ImuBus_LastError() { return present ? 0U : 4U; }
