#include "FakeAfbrS50.h"

namespace {
TofSample nextSample;
TofHealth nextHealth;
bool producing;
}

namespace FakeAfbrS50 {
void Reset()
{
  nextSample = TofSample{};
  nextHealth = TofHealth{};
  nextHealth.status = TofStatus::Ready;
  nextHealth.chipId = 0x0012ABCDU;
  nextHealth.attempts = 1U;
  producing = false;
}
void Ranging(int32_t rangeMm, uint32_t amplitude, uint8_t quality)
{
  nextSample.rangeMm = rangeMm;
  nextSample.amplitude = amplitude;
  nextSample.quality = quality;
  nextSample.valid = true;
  nextSample.rangeValid = amplitude != 0U;
  nextHealth.status = TofStatus::Ready;
  producing = true;
}
void InitFailed(int32_t argusError)
{
  nextHealth.status = TofStatus::InitFailed;
  nextHealth.lastArgusError = argusError;
  ++nextHealth.errors;
  producing = false;
}
}  // namespace FakeAfbrS50

void AfbrS50::Begin(uint32_t nowMs)
{
  sample_ = TofSample{};
  health_ = nextHealth;
  lastSampleMs_ = nowMs;
  started_ = true;
}

void AfbrS50::Service(uint32_t nowMs)
{
  health_.status = nextHealth.status;
  health_.errors = nextHealth.errors;
  health_.lastArgusError = nextHealth.lastArgusError;
  if (!producing) { return; }
  sample_ = nextSample;
  sample_.timestampMs = nowMs;
  sample_.sequence = ++health_.samples;
  lastSampleMs_ = nowMs;
}

bool AfbrS50::Fresh(uint32_t nowMs) const
{
  return sample_.valid && static_cast<uint32_t>(nowMs - lastSampleMs_) <= 250U;
}

const char* AfbrS50::StatusName(TofStatus status)
{
  switch (status)
  {
    case TofStatus::Starting:   return "STARTING";
    case TofStatus::Ready:      return "READY";
    case TofStatus::InitFailed: return "INIT_FAILED";
    case TofStatus::BusError:   return "BUS_ERROR";
    case TofStatus::Stale:      return "STALE";
    default:                    return "UNKNOWN";
  }
}
