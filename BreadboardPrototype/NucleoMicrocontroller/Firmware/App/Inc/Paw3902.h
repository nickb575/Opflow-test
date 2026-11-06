#ifndef ECE480_PAW3902_H
#define ECE480_PAW3902_H

#include <stdint.h>

enum class FlowStatus { Starting, Ready, WrongIdentity, BusError, Stale };

struct FlowSample {
  uint32_t timestampMs = 0U;
  uint32_t sequence = 0U;
  int32_t dx = 0;
  int32_t dy = 0;
  uint8_t motion = 0U;
  uint8_t observation = 0U;
  uint8_t squal = 0U;
  uint8_t rawSum = 0U;
  uint8_t rawMax = 0U;
  uint8_t rawMin = 0U;
  uint16_t shutter = 0U;
  /* valid = a successfully received burst; trackingValid = usable scene data.
   * No motion with good scene data is a valid stationary measurement. */
  bool valid = false;
  bool trackingValid = false;
};

struct FlowHealth {
  FlowStatus status = FlowStatus::Starting;
  uint8_t productId = 0U;
  uint8_t revisionId = 0U;
  uint8_t inverseId = 0U;
  uint32_t samples = 0U;
  uint32_t errors = 0U;
  uint32_t attempts = 0U;
};

class Paw3902 {
 public:
  void Begin(uint32_t nowMs);
  void Service(uint32_t nowMs);
  bool Fresh(uint32_t nowMs) const;
  const FlowSample& Latest() const { return sample_; }
  const FlowHealth& Health() const { return health_; }
  static const char* StatusName(FlowStatus status);
  static FlowSample Decode(const uint8_t* bytes, uint32_t timestampMs, uint32_t sequence);

 private:
  enum class Step { ResetLow, ResetHighWait, SoftReset, SoftResetWait, Identity,
                    ClearMotion, Configure, ConfigDelay, Read, Fault };
  void Fail(FlowStatus status, uint32_t nowMs);
  Step step_ = Step::ResetLow;
  FlowSample sample_{};
  FlowHealth health_{};
  uint32_t stageStartMs_ = 0U;
  uint32_t lastReadMs_ = 0U;
  uint32_t lastIdentityMs_ = 0U;
  uint16_t configIndex_ = 0U;
  uint8_t identityIndex_ = 0U;
  uint8_t clearIndex_ = 0U;
  uint8_t delayMs_ = 0U;
  uint8_t discard_ = 3U;
  bool recheckingIdentity_ = false;
};

#endif
