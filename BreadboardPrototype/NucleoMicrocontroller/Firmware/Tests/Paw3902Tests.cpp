#include "Paw3902.h"
#include "FakeFlowBus.h"
#include <cstdio>

namespace {
unsigned failures = 0U;
void Expect(bool condition, const char* message)
{
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void RunScenario(const char* name, void (*test)())
{
  const unsigned before = failures;
  std::printf("[RUN ] %s\n", name); std::fflush(stdout);
  test();
  std::printf("[%s] %s\n", failures == before ? "PASS" : "FAIL", name); std::fflush(stdout);
}
void Reset(Paw3902& flow, uint32_t nowMs = 0U) { FakeFlowBus::Reset(); flow.Begin(nowMs); }
void At(Paw3902& flow, uint32_t nowMs) { FakeFlowBus::SetTime(nowMs); flow.Service(nowMs); }
void Run(Paw3902& flow, uint32_t first, uint32_t last)
{
  for (uint32_t ms = first; ms <= last; ++ms) { At(flow, ms); }
}

void TestDecode()
{
  const uint8_t bytes[12] = {0x80U,0x3FU, 0x34U,0x12U, 0xFFU,0xFFU, 40U,80U,200U,10U, 0xE4U,0x56U};
  const FlowSample sample = Paw3902::Decode(bytes, 123U, 7U);
  Expect(sample.valid && sample.trackingValid && sample.timestampMs == 123U && sample.sequence == 7U,
         "burst metadata and scene validity");
  Expect(sample.dx == 0x1234 && sample.dy == -1, "little-endian signed motion words");
  Expect(sample.shutter == 0x0456U && sample.squal == 40U && sample.rawSum == 80U &&
         sample.rawMax == 200U && sample.rawMin == 10U, "burst field order and 13-bit shutter mask");
  const uint8_t extremes[12] = {0x80U,0U, 0U,0x80U, 0xFFU,0x7FU, 40U,80U,200U,10U, 4U,0U};
  const FlowSample edge = Paw3902::Decode(extremes, 0U, 1U);
  Expect(edge.dx == -32768 && edge.dy == 32767, "signed motion limits do not depend on char signedness");
}

void TestInitialization()
{
  Paw3902 flow; Reset(flow);
  Expect(FakeFlowBus::ResetAsserted() && !flow.Fresh(0U), "reset starts asserted with no valid sample");
  At(flow, 0U); Expect(FakeFlowBus::WriteCount() == 0U, "no SPI writes during reset-low wait");
  At(flow, 1U); Expect(!FakeFlowBus::ResetAsserted(), "hardware reset released after at least one millisecond");
  Run(flow, 2U, 400U);
  Expect(flow.Fresh(400U) && flow.Latest().trackingValid && flow.Health().errors == 0U,
         "initialized sensor produces fresh scene-valid bursts");
  Expect(flow.Health().productId == 0x49U && flow.Health().revisionId == 0x01U && flow.Health().inverseId == 0xB6U,
         "all identity registers checked");
  Expect(FakeFlowBus::FirstWriteMs() >= 51U && FakeFlowBus::FirstBurstMs() - FakeFlowBus::FirstWriteMs() >= 50U,
         "hardware and soft-reset-to-motion waits respected");
  Expect(FakeFlowBus::WriteCount() == 101U && FakeFlowBus::Register(0U,0x73U) == 0U &&
         FakeFlowBus::Register(0x14U,0x6FU) == 0x1CU, "complete bright optimization and LED_N enable written");
  for (uint8_t reg = 2U; reg <= 6U; ++reg) { Expect(FakeFlowBus::ReadCount(reg) == 1U, "post-reset motion register clear"); }
  const uint32_t sequence = flow.Latest().sequence;
  Run(flow, 401U, 900U);
  Expect(flow.Latest().sequence - sequence == 50U, "burst acquisition runs at 100 Hz");
}

void TestDiscardAndStationary()
{
  Paw3902 flow; Reset(flow);
  for (uint32_t ms = 0U; ms < 400U; ++ms)
  {
    At(flow, ms);
    const uint32_t sequence = flow.Latest().sequence;
    if (sequence > 0U && sequence <= 3U)
    {
      Expect(flow.Fresh(ms) && !flow.Latest().trackingValid, "first three startup bursts are discarded for tracking");
    }
    if (sequence == 4U)
    {
      Expect(flow.Latest().trackingValid && flow.Latest().dx == 0 && flow.Latest().dy == 0 &&
             (flow.Latest().motion & 0x80U) == 0U, "stationary no-motion burst with good scene data is valid");
      return;
    }
  }
  Expect(false, "fourth startup burst arrived");
}

void TestIdentityFailures()
{
  const uint8_t identities[3][3] = {{0x00U,0x01U,0xB6U},{0x49U,0x02U,0xB6U},{0x49U,0x01U,0x00U}};
  for (const auto& identity : identities)
  {
    Paw3902 flow; Reset(flow); FakeFlowBus::Identity(identity[0],identity[1],identity[2]); Run(flow,0U,300U);
    Expect(flow.Health().status == FlowStatus::WrongIdentity && !flow.Latest().valid &&
           FakeFlowBus::ReadCount(0x16U) == 0U && FakeFlowBus::WriteCount() == 1U,
           "wrong product/revision/inverse prevents configuration and motion publishing");
    Expect(FakeFlowBus::ReadCount(0x00U) == 1U && FakeFlowBus::ReadCount(0x01U) == 1U &&
           FakeFlowBus::ReadCount(0x5FU) == 1U && flow.Health().productId == identity[0] &&
           flow.Health().revisionId == identity[1] && flow.Health().inverseId == identity[2],
           "identity diagnostic triple contains all actual reads even when the first ID is wrong");
    Expect(!FakeFlowBus::ResetAsserted(), "identity fault leaves NRESET released between retries");
  }
}

void TestReadWriteFailures()
{
  Paw3902 flow; Reset(flow); Run(flow,0U,51U); FakeFlowBus::FailNextWrite(); At(flow,52U);
  Expect(flow.Health().status == FlowStatus::BusError && !flow.Latest().valid && !FakeFlowBus::ResetAsserted(),
         "soft-reset write failure reported while NRESET remains released");
  Reset(flow); Run(flow,0U,400U); FakeFlowBus::FailNextRead(); Run(flow,401U,411U);
  Expect(flow.Health().status == FlowStatus::BusError && !flow.Latest().valid && !flow.Fresh(411U),
         "failed burst invalidates previous measurements");
  Reset(flow); Run(flow,0U,102U); FakeFlowBus::FailNextRead(); Run(flow,103U,107U);
  Expect(flow.Health().status == FlowStatus::BusError && FakeFlowBus::ReadCount(0x16U) == 0U,
         "identity read failure cannot become valid all-zero flow");
}

void TestTrackingQuality()
{
  uint8_t bytes[12] = {0U,0U,0U,0U,0U,0U,40U,80U,200U,10U,4U,0U};
  Expect(Paw3902::Decode(bytes,0U,1U).trackingValid, "normal bright scene accepted");
  bytes[2] = 1U;
  const FlowSample inconsistent = Paw3902::Decode(bytes,0U,1U);
  Expect(inconsistent.valid && inconsistent.dx == 1 && !inconsistent.trackingValid,
         "nonzero delta without MOT stays visible but cannot be valid tracking");
  bytes[0] = 0x80U;
  Expect(Paw3902::Decode(bytes,0U,1U).trackingValid, "nonzero delta with MOT is consistent tracking");
  bytes[0] = bytes[2] = 0U;
  bytes[6] = 0U; Expect(!Paw3902::Decode(bytes,0U,1U).trackingValid, "zero SQUAL rejected");
  bytes[6] = 40U; bytes[7] = 0U; Expect(!Paw3902::Decode(bytes,0U,1U).trackingValid, "zero raw sum rejected");
  bytes[7] = 0x99U; Expect(!Paw3902::Decode(bytes,0U,1U).trackingValid, "raw sum outside documented range rejected");
  bytes[7] = 80U; bytes[10] = 0U; Expect(!Paw3902::Decode(bytes,0U,1U).trackingValid, "zero shutter rejected");
  bytes[6] = 24U; bytes[10] = 0x1FU; bytes[11] = 0xF0U;
  Expect(!Paw3902::Decode(bytes,0U,1U).trackingValid, "bright low-quality and saturated shutter rejected");
  bytes[6] = 25U; Expect(Paw3902::Decode(bytes,0U,1U).trackingValid, "bright quality threshold boundary accepted");
  bytes[6] = 24U; bytes[11] = 0xEFU;
  Expect(Paw3902::Decode(bytes,0U,1U).trackingValid, "bright shutter threshold boundary accepted");
  Paw3902 flow; Reset(flow); Run(flow,0U,400U); bytes[6] = 0U; FakeFlowBus::Burst(bytes); Run(flow,401U,411U);
  Expect(flow.Fresh(411U) && !flow.Latest().trackingValid && flow.Health().errors == 0U,
         "tracking loss stays distinct from successful transport");
}

void TestStaleAndRecovery()
{
  Paw3902 flow; Reset(flow); Run(flow,0U,400U);
  const uint32_t last = flow.Latest().timestampMs;
  const uint32_t sequence = flow.Latest().sequence;
  Expect(flow.Fresh(last + 99U) && !flow.Fresh(last + 100U), "freshness expires at exactly100ms");
  At(flow,last + 100U);
  Expect(flow.Health().status == FlowStatus::Stale && !flow.Latest().valid &&
         flow.Health().samples == sequence, "100ms service gap faults before accepting a new burst");
  Run(flow,last + 101U,last + 1099U);
  Expect(flow.Health().attempts == 1U, "stale initialization retry waits one second");
  Run(flow,last + 1100U,last + 1500U);
  Expect(flow.Fresh(last + 1500U) && flow.Health().attempts == 2U && flow.Health().errors == 1U &&
         flow.Latest().sequence > sequence, "stale recovery retains sequence and error history");
}

void TestDisconnectAndReconnect()
{
  Paw3902 flow; Reset(flow); Run(flow,0U,400U); FakeFlowBus::Present(false); Run(flow,401U,1200U);
  Expect(flow.Health().status == FlowStatus::WrongIdentity && !flow.Latest().valid &&
         !FakeFlowBus::ResetAsserted(), "periodic identity detects no-ACK all-zero SPI disconnect with reset released");
  FakeFlowBus::Present(true); Run(flow,1201U,2500U);
  Expect(flow.Fresh(2500U) && flow.Latest().trackingValid && flow.Health().attempts == 2U,
         "reconnected device initializes and recovers scene data");
}

void TestMillisecondWrap()
{
  Paw3902 flow; constexpr uint32_t start = UINT32_MAX - 250U; Reset(flow,start);
  for (uint32_t delta = 0U; delta <= 600U; ++delta) { At(flow,start + delta); }
  Expect(flow.Fresh(start + 600U) && flow.Health().errors == 0U, "reset/config/read deadlines tolerate millisecond rollover");
}
}

int main()
{
  RunScenario("Motion-burst layout, signed counts and shutter mask", TestDecode);
  RunScenario("Identity, initialization, reset timing and acquisition", TestInitialization);
  RunScenario("Startup discard and valid stationary readings", TestDiscardAndStationary);
  RunScenario("Wrong product, revision and inverse IDs", TestIdentityFailures);
  RunScenario("Read/write failures invalidate measurements", TestReadWriteFailures);
  RunScenario("Optical tracking quality and bright-mode boundaries", TestTrackingQuality);
  RunScenario("Stale-data deadline and initialization recovery", TestStaleAndRecovery);
  RunScenario("SPI all-zero disconnect and reconnect", TestDisconnectAndReconnect);
  RunScenario("Flow millisecond rollover", TestMillisecondWrap);
  std::printf("9 PAW3902 scenarios: %s (%u failed checks)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
