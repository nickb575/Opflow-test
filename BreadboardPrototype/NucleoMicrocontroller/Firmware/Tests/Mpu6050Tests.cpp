#include "Mpu6050.h"
#include "FakeImuBus.h"
#include <cstdio>

namespace {
unsigned failures;
void Expect(bool condition, const char* description)
{
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", description); ++failures; }
}
void RunScenario(const char* name, void (*test)())
{
  const unsigned before = failures;
  std::printf("[RUN ] %s\n", name);
  std::fflush(stdout);
  test();
  std::printf("[%s] %s\n", failures == before ? "PASS" : "FAIL", name);
  std::fflush(stdout);
}
void Run(Mpu6050& imu, uint32_t first, uint32_t last)
{
  for (uint32_t ms = first; ms <= last; ++ms) { FakeImuBus::Advance(ms); imu.Service(ms); }
}
void Reset(Mpu6050& imu, uint32_t ms = 0U) { FakeImuBus::Reset(); imu.Begin(ms); }

void TestDecode()
{
  const uint8_t bytes[] = {0x40,0x00, 0xC0,0x00, 0x80,0x00, 0x12,0x34, 0x00,0x83, 0xFF,0x7D, 0x7F,0xFF};
  const ImuSample sample = Mpu6050::Decode(bytes, 123U, 42U);
  Expect(sample.valid && sample.timestampMs == 123U && sample.sequence == 42U, "decode metadata");
  Expect(sample.accelMg[0] == 1000 && sample.accelMg[1] == -1000 && sample.accelMg[2] == -2000,
         "big-endian signed acceleration, including INT16_MIN, converts to mg");
  Expect(sample.gyroMdps[0] == 1000 && sample.gyroMdps[1] == -1000 && sample.gyroMdps[2] == 250129,
         "signed gyro converts to mdps and skips the temperature bytes");
  const uint8_t extremes[] = {0x7F,0xFF, 0xFF,0xFF, 0x00,0x01, 0,0, 0x80,0x00, 0xFF,0xFF, 0x00,0x01};
  const ImuSample edge = Mpu6050::Decode(extremes, 0U, 1U);
  Expect(edge.accelMg[0] == 1999 && edge.accelMg[1] == 0 && edge.accelMg[2] == 0 &&
         edge.gyroMdps[0] == -250137 && edge.gyroMdps[1] == -7 && edge.gyroMdps[2] == 7,
         "conversion extremes and integer rounding toward zero");
}

void TestInitializationAndCadence()
{
  Mpu6050 imu; Reset(imu); Run(imu, 0U, 300U);
  Expect(imu.Fresh(300U) && imu.Health().identity == 0x68U && imu.Health().errors == 0U,
         "correct identity and configuration produce fresh samples");
  Expect(FakeImuBus::Register(0x19U) == 9U && FakeImuBus::Register(0x1AU) == 3U &&
         FakeImuBus::Register(0x1BU) == 0U && FakeImuBus::Register(0x1CU) == 0U &&
         FakeImuBus::Register(0x6BU) == 1U && FakeImuBus::Register(0x38U) == 1U,
         "100 Hz, DLPF, ranges, power and data-ready settings written");
  Expect(FakeImuBus::Reads(0x19U) == 1U && FakeImuBus::Reads(0x1BU) == 1U &&
         FakeImuBus::Reads(0x1CU) == 1U && FakeImuBus::Reads(0x6CU) == 1U,
         "configuration registers are read back");
  const uint32_t sequence = imu.Latest().sequence;
  const unsigned statusReads = FakeImuBus::Reads(0x3AU);
  Run(imu, 301U, 1300U);
  Expect(imu.Latest().sequence - sequence == 100U, "acquisition follows 100 Hz data-ready");
  Expect(FakeImuBus::Reads(0x3AU) - statusReads == 500U, "data-ready polled every 2 ms");
}

void TestWrongIdentityAndRetry()
{
  Mpu6050 imu; Reset(imu); FakeImuBus::Identity(0x70U); Run(imu, 0U, 1U);
  Expect(imu.Health().status == ImuStatus::WrongIdentity && !imu.Fresh(1U), "wrong identity rejected");
  FakeImuBus::Identity(0x68U); Run(imu, 2U, 1000U);
  Expect(imu.Health().attempts == 1U, "no initialization retry before one second");
  Run(imu, 1001U, 1300U);
  Expect(imu.Fresh(1300U) && imu.Health().attempts == 2U && imu.Health().errors == 1U,
         "corrected device recovers on the next retry");
}

void TestMissingAndFailedRead()
{
  Mpu6050 imu; Reset(imu); FakeImuBus::Present(false); Run(imu, 0U, 1U);
  Expect(imu.Health().status == ImuStatus::NoDevice && !imu.Latest().valid, "identity NACK reports missing device");
  Reset(imu); Run(imu, 0U, 300U); FakeImuBus::Present(false); Run(imu, 301U, 305U);
  Expect(imu.Health().status == ImuStatus::BusError && !imu.Latest().valid && !imu.Fresh(305U),
         "failed interrupt read invalidates previously valid data");
  FakeImuBus::Present(true); Run(imu, 306U, 1500U);
  Expect(imu.Fresh(1500U) && imu.Health().attempts == 2U, "disconnect/reconnect recovers automatically");
  Reset(imu); FakeImuBus::FailNext(); imu.Service(0U);
  Expect(imu.Health().status == ImuStatus::NoDevice, "failed transfer start is reported");
}

void TestConfigMismatch()
{
  Mpu6050 imu; Reset(imu); FakeImuBus::MismatchConfig(true); Run(imu, 0U, 300U);
  Expect(imu.Health().status == ImuStatus::ConfigMismatch && !imu.Latest().valid,
         "configuration readback mismatch prevents valid output");
}

void TestStaleAndRecovery()
{
  Mpu6050 imu; Reset(imu); Run(imu, 0U, 300U);
  FakeImuBus::DataReady(false); Run(imu, 301U, 310U);
  const uint32_t last = imu.Latest().timestampMs;
  const uint32_t sequence = imu.Latest().sequence;
  Run(imu, 311U, last + 99U);
  Expect(imu.Fresh(last + 99U) && !imu.Fresh(last + 100U), "freshness changes exactly at 100 ms");
  Run(imu, last + 100U, last + 103U);
  Expect(imu.Health().status == ImuStatus::Stale && !imu.Latest().valid && imu.Health().samples == sequence,
         "no data-ready creates no duplicate samples and reports stale");
  FakeImuBus::DataReady(true); Run(imu, last + 104U, last + 1400U);
  Expect(imu.Fresh(last + 1400U) && imu.Latest().sequence > sequence && imu.Health().errors == 1U,
         "stale recovery preserves advancing sequence and error history");
}

void TestTimeoutAndBusy()
{
  Mpu6050 imu; Reset(imu); FakeImuBus::Hold(true); Run(imu, 0U, 9U);
  Expect(imu.Health().status == ImuStatus::Starting, "pending transfer allowed up to timeout");
  Run(imu, 10U, 10U);
  Expect(imu.Health().status == ImuStatus::ReadTimeout && FakeImuBus::Resets() == 2U,
         "lost interrupt times out and cancels the transfer");
  Reset(imu); FakeImuBus::Busy(true); Run(imu, 0U, 2U); FakeImuBus::Busy(false); Run(imu, 3U, 300U);
  Expect(imu.Fresh(300U) && imu.Health().errors == 0U, "short hardware BUSY after STOP is retried");
  Reset(imu); FakeImuBus::Busy(true); Run(imu, 0U, 10U);
  Expect(imu.Health().status == ImuStatus::ReadTimeout, "stuck hardware BUSY has a bounded timeout");
}

void TestMillisecondWrap()
{
  Mpu6050 imu; constexpr uint32_t base = UINT32_MAX - 250U; Reset(imu, base);
  for (uint32_t delta = 0U; delta <= 500U; ++delta)
  {
    const uint32_t now = base + delta;
    FakeImuBus::Advance(now); imu.Service(now);
  }
  Expect(imu.Fresh(base + 500U) && imu.Health().errors == 0U, "sensor deadlines survive millisecond rollover");
}
}

int main()
{
  RunScenario("Signed decoding and unit conversions", TestDecode);
  RunScenario("Identity, configuration and acquisition cadence", TestInitializationAndCadence);
  RunScenario("Wrong identity and initialization retry", TestWrongIdentityAndRetry);
  RunScenario("Missing device, failed reads and reconnect", TestMissingAndFailedRead);
  RunScenario("Configuration readback mismatch", TestConfigMismatch);
  RunScenario("Stale-data boundary and recovery", TestStaleAndRecovery);
  RunScenario("Transfer timeout and hardware BUSY", TestTimeoutAndBusy);
  RunScenario("Sensor millisecond rollover", TestMillisecondWrap);
  std::printf("8 MPU6050 scenarios: %s (%u failed checks)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
