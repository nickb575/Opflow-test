#include "Mpu6050.h"
#include "ImuBus.h"

namespace {
struct Setting { uint8_t reg; uint8_t value; };
/* PLL X gyro, all axes enabled, DLPF 44/42 Hz, 100 Hz, +/-2g, +/-250 dps.
 * INT_RD_CLEAR=0: only reading INT_STATUS clears data-ready. No FIFO/DMP. */
constexpr Setting kSettings[] = {
    {0x6B, 0x01}, {0x6C, 0x00}, {0x1A, 0x03}, {0x19, 0x09},
    {0x1B, 0x00}, {0x1C, 0x00}, {0x37, 0x00}, {0x38, 0x01}};
constexpr uint8_t kSettingCount = sizeof(kSettings) / sizeof(kSettings[0]);

int32_t SignedWord(const uint8_t* bytes)
{
  const uint32_t word = (static_cast<uint32_t>(bytes[0]) << 8U) | bytes[1];
  return word < 0x8000U ? static_cast<int32_t>(word) : static_cast<int32_t>(word) - 65536;
}
}  // namespace

void Mpu6050::Begin(uint32_t nowMs)
{
  *this = Mpu6050{};
  health_.attempts = 1U;
  lastPollMs_ = nowMs;
  if (!ImuBus_Reset()) { Fail(ImuStatus::BusError, nowMs); }
}

void Mpu6050::Fail(ImuStatus reason, uint32_t nowMs)
{
  sample_.valid = false;
  health_.status = reason;
  ++health_.errors;
  waiting_ = false;
  starting_ = false;
  step_ = Step::Fault;
  stageStartMs_ = nowMs;
  /* Cancel interrupts before the persistent receive buffer is reused. */
  ImuBus_Reset();
}

bool Mpu6050::Fresh(uint32_t nowMs) const
{
  return sample_.valid && health_.status == ImuStatus::Ready &&
         static_cast<uint32_t>(nowMs - sample_.timestampMs) < 100U;
}

ImuSample Mpu6050::Decode(const uint8_t* bytes, uint32_t timestampMs, uint32_t sequence)
{
  ImuSample sample{};
  sample.timestampMs = timestampMs;
  sample.sequence = sequence;
  for (unsigned axis = 0U; axis < 3U; ++axis)
  {
    sample.accelMg[axis] = SignedWord(bytes + 2U * axis) * 1000 / 16384;
    /* Bytes 6/7 contain temperature, not gyro X. */
    sample.gyroMdps[axis] = SignedWord(bytes + 8U + 2U * axis) * 1000 / 131;
  }
  sample.valid = true;
  return sample;
}

void Mpu6050::Start(bool write, uint8_t reg, uint16_t length, uint32_t nowMs)
{
  if (!starting_) { transferStartMs_ = nowMs; }
  const ImuBusStart result = write ? ImuBus_Write(reg, bytes_, length) : ImuBus_Read(reg, bytes_, length);
  starting_ = result == ImuBusStart::Busy;
  waiting_ = result == ImuBusStart::Started;
  if (result == ImuBusStart::Error)
  {
    Fail(step_ == Step::Who ? ImuStatus::NoDevice : ImuStatus::BusError, nowMs);
  }
  else if (starting_ && static_cast<uint32_t>(nowMs - transferStartMs_) >= 10U)
  {
    Fail(ImuStatus::ReadTimeout, nowMs);
  }
}

void Mpu6050::Complete(uint32_t nowMs)
{
  switch (step_)
  {
    case Step::Who:
      health_.identity = bytes_[0];
      if (bytes_[0] != 0x68U) { Fail(ImuStatus::WrongIdentity, nowMs); }
      else { step_ = Step::Reset; }
      break;
    case Step::Reset:
      stageStartMs_ = nowMs;
      step_ = Step::ResetWait;
      break;
    case Step::WriteConfig:
      if (++configIndex_ == kSettingCount)
      {
        configIndex_ = 0U;
        step_ = Step::VerifyConfig;
      }
      break;
    case Step::VerifyConfig:
      if (bytes_[0] != kSettings[configIndex_].value)
      {
        Fail(ImuStatus::ConfigMismatch, nowMs);
      }
      else if (++configIndex_ == kSettingCount)
      {
        stageStartMs_ = nowMs;
        step_ = Step::Settle;
      }
      break;
    case Step::Status:
      if ((bytes_[0] & 0x01U) != 0U) { step_ = Step::Data; }
      break;
    case Step::Data:
      sample_ = Decode(bytes_, nowMs, ++health_.samples);
      lastSampleMs_ = nowMs;
      step_ = Step::Status;
      break;
    default:
      break;
  }
}

void Mpu6050::Service(uint32_t nowMs)
{
  if (step_ == Step::Fault)
  {
    if (static_cast<uint32_t>(nowMs - stageStartMs_) < 1000U) { return; }
    ++health_.attempts;
    health_.status = ImuStatus::Starting;
    configIndex_ = 0U;
    step_ = Step::Who;
    if (!ImuBus_Reset()) { Fail(ImuStatus::BusError, nowMs); return; }
  }

  if (waiting_)
  {
    const ImuBusResult result = ImuBus_Result();
    if (result == ImuBusResult::Pending)
    {
      if (static_cast<uint32_t>(nowMs - transferStartMs_) >= 10U)
      {
        Fail(ImuStatus::ReadTimeout, nowMs);
      }
      return;
    }
    waiting_ = false;
    if (result != ImuBusResult::Complete)
    {
      Fail(step_ == Step::Who ? ImuStatus::NoDevice : ImuStatus::BusError, nowMs);
      return;
    }
    Complete(nowMs);
  }

  if (health_.status == ImuStatus::Ready &&
      static_cast<uint32_t>(nowMs - lastSampleMs_) >= 100U)
  {
    Fail(ImuStatus::Stale, nowMs);
    return;
  }

  switch (step_)
  {
    case Step::Who: Start(false, 0x75U, 1U, nowMs); break;
    case Step::Reset:
      bytes_[0] = 0x80U;
      Start(true, 0x6BU, 1U, nowMs);
      break;
    case Step::ResetWait:
      if (static_cast<uint32_t>(nowMs - stageStartMs_) >= 100U) { step_ = Step::WriteConfig; }
      break;
    case Step::WriteConfig:
      bytes_[0] = kSettings[configIndex_].value;
      Start(true, kSettings[configIndex_].reg, 1U, nowMs);
      break;
    case Step::VerifyConfig: Start(false, kSettings[configIndex_].reg, 1U, nowMs); break;
    case Step::Settle:
      if (static_cast<uint32_t>(nowMs - stageStartMs_) >= 50U)
      {
        health_.status = ImuStatus::Ready;
        lastSampleMs_ = nowMs;
        lastPollMs_ = nowMs - 2U;
        step_ = Step::Status;
      }
      break;
    case Step::Status:
      if (static_cast<uint32_t>(nowMs - lastPollMs_) >= 2U)
      {
        lastPollMs_ = nowMs;
        Start(false, 0x3AU, 1U, nowMs);
      }
      break;
    case Step::Data: Start(false, 0x3BU, 14U, nowMs); break;
    case Step::Fault: break;
  }
}

const char* Mpu6050::StatusName(ImuStatus status)
{
  switch (status)
  {
    case ImuStatus::Starting: return "INITIALIZING";
    case ImuStatus::Ready: return "READY";
    case ImuStatus::NoDevice: return "NO_DEVICE";
    case ImuStatus::WrongIdentity: return "WRONG_ID";
    case ImuStatus::ConfigMismatch: return "CONFIG_MISMATCH";
    case ImuStatus::BusError: return "I2C_ERROR";
    case ImuStatus::ReadTimeout: return "READ_TIMEOUT";
    case ImuStatus::Stale: return "STALE";
  }
  return "UNKNOWN";
}
