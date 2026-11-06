#ifndef ECE480_MPU6050_H
#define ECE480_MPU6050_H

#include <stdint.h>

enum class ImuStatus {
  Starting, Ready, NoDevice, WrongIdentity, ConfigMismatch,
  BusError, ReadTimeout, Stale
};

struct ImuSample {
  uint32_t timestampMs = 0U;
  uint32_t sequence = 0U;
  int32_t accelMg[3] = {};
  int32_t gyroMdps[3] = {};
  bool valid = false;
};

struct ImuHealth {
  ImuStatus status = ImuStatus::Starting;
  uint8_t identity = 0U;
  uint32_t samples = 0U;
  uint32_t errors = 0U;
  uint32_t attempts = 0U;
};

class Mpu6050 {
 public:
  void Begin(uint32_t nowMs);
  void Service(uint32_t nowMs);
  bool Fresh(uint32_t nowMs) const;
  const ImuSample& Latest() const { return sample_; }
  const ImuHealth& Health() const { return health_; }
  static const char* StatusName(ImuStatus status);
  static ImuSample Decode(const uint8_t* bytes, uint32_t timestampMs, uint32_t sequence);

 private:
  enum class Step { Who, Reset, ResetWait, WriteConfig, VerifyConfig, Settle, Status, Data, Fault };
  void Fail(ImuStatus reason, uint32_t nowMs);
  void Start(bool write, uint8_t reg, uint16_t length, uint32_t nowMs);
  void Complete(uint32_t nowMs);
  Step step_ = Step::Who;
  ImuSample sample_{};
  ImuHealth health_{};
  uint8_t bytes_[14] = {};
  uint8_t configIndex_ = 0U;
  bool waiting_ = false;
  bool starting_ = false;
  uint32_t transferStartMs_ = 0U;
  uint32_t stageStartMs_ = 0U;
  uint32_t lastPollMs_ = 0U;
  uint32_t lastSampleMs_ = 0U;
};

#endif
