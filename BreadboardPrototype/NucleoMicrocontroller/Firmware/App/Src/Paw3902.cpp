/****************************************************************************
 *
 *   Copyright (c) 2019-2022 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "Paw3902.h"
#include "FlowBus.h"

namespace {
struct Setting { uint8_t reg; uint8_t value; };
constexpr uint8_t kDelay = 0xFFU;
/* Bright-mode table and LED_N enable sequence adapted from PX4's PAW3902
 * driver via nickb575/Opflow-test c6b08598c6fad972a9fe48364b16d0d08f743481,
 * Core/Inc/paw3902_init.h. Register order and delay markers are preserved.
 * Bus/scheduler/error handling below is the standalone ECE480 integration. */
constexpr Setting kBrightSettings[] = {
    { 0x7F, 0x00 }, { 0x55, 0x01 }, { 0x50, 0x07 }, { 0x7F, 0x0E },
    { 0x43, 0x10 }, { 0x48, 0x02 }, { 0x7F, 0x00 }, { 0x51, 0x7B },
    { 0x50, 0x00 }, { 0x55, 0x00 }, { 0x7F, 0x00 }, { 0x61, 0xAD },
    { 0x7F, 0x03 }, { 0x40, 0x00 }, { 0x7F, 0x05 }, { 0x41, 0xB3 },
    { 0x43, 0xF1 }, { 0x45, 0x14 }, { 0x5F, 0x34 }, { 0x7B, 0x08 },
    { 0x5E, 0x34 }, { 0x5B, 0x32 }, { 0x6D, 0x32 }, { 0x45, 0x17 },
    { 0x70, 0xE5 }, { 0x71, 0xE5 }, { 0x7F, 0x06 }, { 0x44, 0x1B },
    { 0x40, 0xBF }, { 0x4E, 0x3F }, { 0x7F, 0x08 }, { 0x66, 0x44 },
    { 0x65, 0x20 }, { 0x6A, 0x3A }, { 0x61, 0x05 }, { 0x62, 0x05 },
    { 0x7F, 0x09 }, { 0x4F, 0xAF }, { 0x48, 0x80 }, { 0x49, 0x80 },
    { 0x57, 0x77 }, { 0x5F, 0x40 }, { 0x60, 0x78 }, { 0x61, 0x78 },
    { 0x62, 0x08 }, { 0x63, 0x50 }, { 0x7F, 0x0A }, { 0x45, 0x60 },
    { 0x7F, 0x00 }, { 0x4D, 0x11 }, { 0x55, 0x80 }, { 0x74, 0x21 },
    { 0x75, 0x1F }, { 0x4A, 0x78 }, { 0x4B, 0x78 }, { 0x44, 0x08 },
    { 0x45, 0x50 }, { 0x64, 0xFE }, { 0x65, 0x1F }, { 0x72, 0x0A },
    { 0x73, 0x00 }, { 0x7F, 0x14 }, { 0x44, 0x84 }, { 0x65, 0x47 },
    { 0x66, 0x18 }, { 0x63, 0x70 }, { 0x6F, 0x2C }, { 0x7F, 0x15 },
    { 0x48, 0x48 }, { 0x7F, 0x07 }, { 0x41, 0x0D }, { 0x43, 0x14 },
    { 0x4B, 0x0E }, { 0x45, 0x0F }, { 0x44, 0x42 }, { 0x4C, 0x80 },
    { 0x7F, 0x10 }, { 0x5B, 0x03 }, { 0x7F, 0x07 }, { 0x40, 0x41 },
    { kDelay, 10 }, { 0x7F, 0x00 }, { 0x32, 0x00 }, { 0x7F, 0x07 },
    { 0x40, 0x40 }, { 0x7F, 0x06 }, { 0x68, 0x70 }, { 0x69, 0x01 },
    { 0x7F, 0x0D }, { 0x48, 0xC0 }, { 0x6F, 0xD5 }, { 0x7F, 0x00 },
    { 0x5B, 0xA0 }, { 0x4E, 0xA8 }, { 0x5A, 0x50 }, { 0x40, 0x80 },
    { 0x73, 0x1F }, { kDelay, 10 }, { 0x73, 0x00 }, { 0x7F, 0x14 },
    { 0x6F, 0x1C }, { 0x7F, 0x00 },
};
constexpr uint16_t kSettingCount = static_cast<uint16_t>(sizeof(kBrightSettings) / sizeof(kBrightSettings[0]));
constexpr uint8_t kIdentityRegisters[] = {0x00U, 0x01U, 0x5FU};
constexpr uint8_t kExpectedIdentity[] = {0x49U, 0x01U, 0xB6U};

int32_t SignedLittleEndian(const uint8_t* bytes)
{
  const uint32_t word = bytes[0] | (static_cast<uint32_t>(bytes[1]) << 8U);
  return word < 0x8000U ? static_cast<int32_t>(word) : static_cast<int32_t>(word) - 65536;
}
}  // namespace

void Paw3902::Begin(uint32_t nowMs)
{
  *this = Paw3902{};
  health_.attempts = 1U;
  stageStartMs_ = nowMs;
  FlowBus_SetReset(true);
}

void Paw3902::Fail(FlowStatus status, uint32_t nowMs)
{
  sample_.valid = false;
  sample_.trackingValid = false;
  health_.status = status;
  ++health_.errors;
  step_ = Step::Fault;
  stageStartMs_ = nowMs;
  /* Leave the sensor released between retries so NRESET can be measured.
   * The next retry still provides the explicit reset-low pulse. */
  FlowBus_SetReset(false);
}

bool Paw3902::Fresh(uint32_t nowMs) const
{
  return sample_.valid && health_.status == FlowStatus::Ready &&
         static_cast<uint32_t>(nowMs - sample_.timestampMs) < 100U;
}

FlowSample Paw3902::Decode(const uint8_t* bytes, uint32_t timestampMs, uint32_t sequence)
{
  FlowSample sample{};
  sample.timestampMs = timestampMs;
  sample.sequence = sequence;
  sample.motion = bytes[0];
  sample.observation = bytes[1];
  sample.dx = SignedLittleEndian(bytes + 2U);
  sample.dy = SignedLittleEndian(bytes + 4U);
  sample.squal = bytes[6];
  sample.rawSum = bytes[7];
  sample.rawMax = bytes[8];
  sample.rawMin = bytes[9];
  /* Motion_Burst transmits shutter upper then lower, with 13 significant bits. */
  sample.shutter = static_cast<uint16_t>((static_cast<uint16_t>(bytes[10] & 0x1FU) << 8U) | bytes[11]);
  sample.valid = true;
  const bool motionConsistent = (sample.motion & 0x80U) != 0U || (sample.dx == 0 && sample.dy == 0);
  sample.trackingValid = sample.squal > 0U && sample.rawSum > 0U &&
      sample.rawSum <= 0x98U && sample.shutter > 0U &&
      !(sample.squal < 0x19U && sample.shutter >= 0x1FF0U) && motionConsistent;
  return sample;
}

void Paw3902::Service(uint32_t nowMs)
{
  if (step_ == Step::Fault)
  {
    if (static_cast<uint32_t>(nowMs - stageStartMs_) < 1000U) { return; }
    ++health_.attempts;
    health_.status = FlowStatus::Starting;
    identityIndex_ = clearIndex_ = 0U;
    configIndex_ = 0U;
    discard_ = 3U;
    recheckingIdentity_ = false;
    stageStartMs_ = nowMs;
    step_ = Step::ResetLow;
    FlowBus_SetReset(true);
  }

  /* A main-loop stall must not refresh old data by reading first. */
  if (health_.status == FlowStatus::Ready && sample_.valid &&
      static_cast<uint32_t>(nowMs - sample_.timestampMs) >= 100U)
  {
    Fail(FlowStatus::Stale, nowMs);
    return;
  }

  uint8_t byte = 0U;
  switch (step_)
  {
    case Step::ResetLow:
      if (static_cast<uint32_t>(nowMs - stageStartMs_) >= 1U)
      {
        FlowBus_SetReset(false);
        stageStartMs_ = nowMs;
        step_ = Step::ResetHighWait;
      }
      break;
    case Step::ResetHighWait:
      if (static_cast<uint32_t>(nowMs - stageStartMs_) >= 50U) { step_ = Step::SoftReset; }
      break;
    case Step::SoftReset:
      if (!FlowBus_Write(0x3AU, 0x5AU)) { Fail(FlowStatus::BusError, nowMs); break; }
      stageStartMs_ = nowMs;
      step_ = Step::SoftResetWait;
      break;
    case Step::SoftResetWait:
      /* PAW3902 general datasheet p7: reset-to-valid-motion >=50 ms. */
      if (static_cast<uint32_t>(nowMs - stageStartMs_) >= 50U) { step_ = Step::Identity; }
      break;
    case Step::Identity:
      if (!FlowBus_Read(kIdentityRegisters[identityIndex_], &byte, 1U))
      {
        Fail(FlowStatus::BusError, nowMs);
        break;
      }
      if (identityIndex_ == 0U) { health_.productId = byte; }
      else if (identityIndex_ == 1U) { health_.revisionId = byte; }
      else { health_.inverseId = byte; }
      if (++identityIndex_ == 3U)
      {
        /* Collect the whole triple even if product ID is wrong: diagnostics
         * must distinguish read values from untouched/default fields. */
        if (health_.productId != kExpectedIdentity[0] || health_.revisionId != kExpectedIdentity[1] ||
            health_.inverseId != kExpectedIdentity[2])
        {
          Fail(FlowStatus::WrongIdentity, nowMs);
          break;
        }
        lastIdentityMs_ = nowMs;
        step_ = recheckingIdentity_ ? Step::Read : Step::ClearMotion;
        recheckingIdentity_ = false;
      }
      break;
    case Step::ClearMotion:
      if (!FlowBus_Read(static_cast<uint8_t>(0x02U + clearIndex_), &byte, 1U))
      {
        Fail(FlowStatus::BusError, nowMs);
      }
      else if (++clearIndex_ == 5U) { step_ = Step::Configure; }
      break;
    case Step::Configure:
      if (configIndex_ == kSettingCount)
      {
        health_.status = FlowStatus::Ready;
        lastReadMs_ = nowMs - 10U;
        step_ = Step::Read;
      }
      else if (kBrightSettings[configIndex_].reg == kDelay)
      {
        delayMs_ = kBrightSettings[configIndex_++].value;
        stageStartMs_ = nowMs;
        step_ = Step::ConfigDelay;
      }
      else
      {
        const Setting& setting = kBrightSettings[configIndex_];
        if (!FlowBus_Write(setting.reg, setting.value)) { Fail(FlowStatus::BusError, nowMs); }
        else { ++configIndex_; }
      }
      break;
    case Step::ConfigDelay:
      if (static_cast<uint32_t>(nowMs - stageStartMs_) >= delayMs_) { step_ = Step::Configure; }
      break;
    case Step::Read:
      /* SPI cannot NACK. Periodic identity checks detect all-zero/all-one reads
       * from a disconnected/unpowered sensor without confusing them with rest. */
      if (static_cast<uint32_t>(nowMs - lastIdentityMs_) >= 1000U)
      {
        identityIndex_ = 0U;
        recheckingIdentity_ = true;
        step_ = Step::Identity;
      }
      else if (static_cast<uint32_t>(nowMs - lastReadMs_) >= 10U)
      {
        uint8_t bytes[12] = {};
        if (!FlowBus_Read(0x16U, bytes, sizeof(bytes))) { Fail(FlowStatus::BusError, nowMs); break; }
        lastReadMs_ = nowMs;
        sample_ = Decode(bytes, nowMs, ++health_.samples);
        if (discard_ > 0U) { --discard_; sample_.trackingValid = false; }
      }
      break;
    case Step::Fault:
      break;
  }
}

const char* Paw3902::StatusName(FlowStatus status)
{
  switch (status)
  {
    case FlowStatus::Starting: return "INITIALIZING";
    case FlowStatus::Ready: return "READY";
    case FlowStatus::WrongIdentity: return "WRONG_ID";
    case FlowStatus::BusError: return "SPI_ERROR";
    case FlowStatus::Stale: return "STALE";
  }
  return "UNKNOWN";
}
