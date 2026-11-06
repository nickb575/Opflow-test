#ifndef ECE480_AFBR_S50_H
#define ECE480_AFBR_S50_H

#include <stdint.h>

/* Mirrors the shape of Paw3902/Mpu6050: Begin, Service, Latest, Health. */

enum class TofStatus { Starting, Ready, InitFailed, BusError, Stale };

struct TofSample {
  uint32_t timestampMs = 0U;
  uint32_t sequence = 0U;
  /* Range in millimetres. Can be slightly negative very close to the
   * reference plane, hence signed. */
  int32_t rangeMm = 0;
  /* Signal amplitude in LSB. Zero means the binning algorithm produced no
   * valid result for this frame. */
  uint32_t amplitude = 0U;
  /* Signal quality as a percentage: 0 = n/a, 1 = bad, 100 = good. */
  uint8_t quality = 0U;
  /* valid = a frame was evaluated without error.
   * rangeValid = that frame carried a usable distance. */
  bool valid = false;
  bool rangeValid = false;
};

struct TofHealth {
  TofStatus status = TofStatus::Starting;
  uint32_t chipId = 0U;
  uint32_t apiVersion = 0U;
  uint32_t moduleVersion = 0U;
  uint32_t samples = 0U;
  uint32_t errors = 0U;
  uint32_t attempts = 0U;
  /* Last non-OK status_t from the Argus API, for diagnostics. */
  int32_t lastArgusError = 0;
  /* Bitfield from TofHw_Init if peripheral setup failed. */
  uint32_t hwError = 0U;
};

class AfbrS50 {
 public:
  /* Blocks for roughly 300 ms (datasheet T_init) performing the device's
   * boot and EEPROM calibration read. Call from App_Init, never from
   * App_Task. */
  void Begin(uint32_t nowMs);

  /* Non-blocking. Evaluates a frame when the driver reports one ready, and
   * retries initialisation on a backoff if Begin failed. */
  void Service(uint32_t nowMs);

  bool Fresh(uint32_t nowMs) const;
  const TofSample& Latest() const { return sample_; }
  const TofHealth& Health() const { return health_; }
  static const char* StatusName(TofStatus status);

 private:
  bool Start(uint32_t nowMs);
  void Fail(TofStatus status, uint32_t nowMs);

  TofSample sample_{};
  TofHealth health_{};
  uint32_t lastSampleMs_ = 0U;
  uint32_t retryAtMs_ = 0U;
  bool started_ = false;
};

#endif /* ECE480_AFBR_S50_H */
