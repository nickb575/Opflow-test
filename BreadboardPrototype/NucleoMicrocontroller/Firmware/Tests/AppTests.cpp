#include "App.h"
#include "main.h"
#include "FakeImuBus.h"
#include "FakeFlowBus.h"
#include "FakeAfbrS50.h"
#include <cstdio>
#include <cstring>

extern "C" {
UART_HandleTypeDef huart2{};
CoreDebug_Type debugRegisters{};
DWT_Type dwtRegisters{};
GPIO_TypeDef gpioRegisters{};
CoreDebug_Type* CoreDebug = &debugRegisters;
DWT_Type* DWT = &dwtRegisters;
GPIO_TypeDef* GPIOA = &gpioRegisters;
uint32_t SystemCoreClock = 64000000U;
extern volatile uint32_t UartTxCount, UartErrorCount;
extern volatile uint32_t UartBusyCount, UartTimeoutCount;
extern volatile uint32_t LastHalTick, HalTickStallCount;
extern volatile HAL_StatusTypeDef UartStatus;
}

namespace {
struct FakeState {
  uint32_t tick = 0U;
  uint32_t mask = 0U;
  uint32_t startMask = 0U;
  uint32_t abortMask = 0U;
  unsigned starts = 0U;
  unsigned aborts = 0U;
  unsigned toggles = 0U;
  unsigned ledWrites = 0U;
  GPIO_PinState ledState = GPIO_PIN_SET;
  const uint8_t* bytes = nullptr;
  uint16_t length = 0U;
  HAL_StatusTypeDef nextStatus = HAL_OK;
} fake;
unsigned failures = 0U;

void Expect(bool condition, const char* description)
{
  if (!condition)
  {
    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failures;
  }
}

void Reset()
{
  /* Complete any previous fixture's TX before resetting observable state. */
  HAL_UART_TxCpltCallback(&huart2);
  fake = FakeState{};
  UartTxCount = UartErrorCount = 0U;
  UartBusyCount = UartTimeoutCount = HalTickStallCount = 0U;
  LastHalTick = 0U;
  UartStatus = HAL_OK;
  FakeImuBus::Reset();
  FakeFlowBus::Reset();
  FakeAfbrS50::Reset();
  App_Init();
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

void AtMs(uint32_t ms, bool tickAdvances = true)
{
  DWT->CYCCNT = ms * (SystemCoreClock / 1000U);
  if (tickAdvances) { fake.tick = ms; }
  FakeImuBus::Advance(ms);
  FakeFlowBus::SetTime(ms);
  App_Task();
}

void TestCadenceAndCompletion()
{
  Reset();
  AtMs(99U);
  Expect(fake.starts == 0U, "no UART transmission before 100 ms");
  AtMs(100U);
  constexpr char expected[] = "IMU t_ms=100 ";
  Expect(fake.starts == 1U, "scheduled period starts one UART transfer");
  Expect(fake.ledWrites == 0U && fake.toggles == 0U, "application leaves SPI clock/LD2 pin under peripheral ownership");
  Expect(fake.bytes && fake.length == std::strlen(reinterpret_cast<const char*>(fake.bytes)),
         "UART length excludes the terminating NUL");
  Expect(fake.bytes && std::memcmp(fake.bytes, expected, sizeof(expected) - 1U) == 0,
         "persistent UART bytes remain valid after App_Task returns");
  Expect(UartTxCount == 0U, "starting a transfer does not count as completion");
  char saved[768];
  std::memcpy(saved, fake.bytes, fake.length);
  AtMs(150U);
  Expect(fake.starts == 1U && UartBusyCount == 0U && std::memcmp(fake.bytes, saved, fake.length) == 0,
         "service between periods leaves the in-flight buffer untouched");
  HAL_UART_TxCpltCallback(&huart2);
  HAL_UART_TxCpltCallback(&huart2);
  Expect(UartTxCount == 1U, "completion counted once");
  Expect(fake.mask == 0U && fake.startMask == 1U, "start protects state and restores interrupts");
}

void TestMissedDeadlines()
{
  Reset();
  AtMs(100U);
  HAL_UART_TxCpltCallback(&huart2);
  AtMs(450U);
  App_Task();
  Expect(fake.starts == 2U && fake.toggles == 0U, "late service skips missed UART periods without GPIO toggling");
  HAL_UART_TxCpltCallback(&huart2);
}

void TestTimeoutWithStoppedTick()
{
  Reset();
  AtMs(100U, false);
  Expect(HalTickStallCount == 1U && fake.starts == 1U, "stopped HAL tick does not block UART");
  AtMs(199U, false);
  Expect(fake.aborts == 0U, "no abort before 100 ms");
  AtMs(200U, false);
  /* The UART period equals the timeout, so the next heartbeat follows the
   * abort in the same service pass. */
  Expect(fake.aborts == 1U && UartTimeoutCount == 1U && UartErrorCount == 1U,
         "lost completion aborts at 100 ms without HAL tick progress");
  Expect(fake.abortMask == 1U && fake.mask == 0U, "abort runs with interrupts masked and restores them");
  Expect(fake.starts == 2U, "next heartbeat retries after a timeout");
  HAL_UART_TxCpltCallback(&huart2);
  Expect(UartTxCount == 1U, "retried transfer completes normally");
}

void TestCounterWrap()
{
  Reset();
  const uint32_t cyclesPerMs = SystemCoreClock / 1000U;
  DWT->CYCCNT = UINT32_MAX - 50U * cyclesPerMs;
  App_Task();
  const unsigned startsBefore = fake.starts;
  DWT->CYCCNT += 99U * cyclesPerMs;
  App_Task();
  Expect(fake.aborts == 0U && fake.starts == startsBefore, "wrapped counter does not abort or send early");
  DWT->CYCCNT += cyclesPerMs;
  App_Task();
  Expect(fake.aborts == 1U && fake.starts == startsBefore + 1U,
         "timeout and cadence cross the 32-bit counter boundary");
  HAL_UART_TxCpltCallback(&huart2);
  DWT->CYCCNT += 100U * cyclesPerMs;
  App_Task();
  Expect(fake.starts == startsBefore + 2U && fake.toggles == 0U, "UART cadence continues after counter wrap without GPIO toggling");
  HAL_UART_TxCpltCallback(&huart2);
}

void TestStartFailures()
{
  Reset();
  fake.nextStatus = HAL_BUSY;
  AtMs(100U);
  Expect(UartBusyCount == 1U && UartTxCount == 0U, "busy start is not a successful transfer");
  fake.nextStatus = HAL_ERROR;
  AtMs(200U);
  Expect(UartErrorCount == 1U && UartStatus == HAL_ERROR, "failed start is diagnosed");
  fake.nextStatus = HAL_OK;
  AtMs(300U);
  HAL_UART_TxCpltCallback(&huart2);
  Expect(UartTxCount == 1U, "failed starts do not leave application TX active");
}

void TestCallbacksAndMask()
{
  Reset();
  UART_HandleTypeDef unrelated{};
  AtMs(100U);
  HAL_UART_TxCpltCallback(&unrelated);
  HAL_UART_ErrorCallback(&unrelated);
  Expect(UartTxCount == 0U && UartErrorCount == 0U && fake.aborts == 0U,
         "other UART callbacks are ignored");
  HAL_UART_ErrorCallback(&huart2);
  Expect(UartErrorCount == 1U && fake.aborts == 1U, "USART2 errors abort and report the transfer");
  AtMs(200U);
  HAL_UART_TxCpltCallback(&huart2);
  Expect(UartTxCount == 1U, "next heartbeat recovers from a peripheral error");

  Reset();
  fake.mask = 1U;
  AtMs(1U);
  Expect(fake.mask == 1U, "early return preserves a disabled interrupt mask");
  AtMs(100U);
  Expect(fake.mask == 1U, "start preserves a disabled interrupt mask");
  AtMs(200U);
  Expect(fake.mask == 1U && fake.aborts == 1U, "abort preserves a disabled interrupt mask");
}

void TestLiveAndStaleOutput()
{
  Reset();
  for (uint32_t ms = 1U; ms <= 600U; ++ms)
  {
    AtMs(ms);
    HAL_UART_TxCpltCallback(&huart2);
  }
  const char* line = reinterpret_cast<const char*>(fake.bytes);
  Expect(std::strstr(line, "status=OK") && std::strstr(line, "az_mg=1000") &&
         std::strstr(line, "id=0x68 config=OK"), "verified sensor produces scaled live text");
  Expect(fake.starts == 6U && UartTxCount == 6U, "display runs at 10 lines per second");
  FakeImuBus::DataReady(false);
  for (uint32_t ms = 601U; ms <= 800U; ++ms)
  {
    AtMs(ms);
    HAL_UART_TxCpltCallback(&huart2);
  }
  Expect(std::strstr(line, "status=STALE") && !std::strstr(line, "ax_mg=") && !std::strstr(line, "status=OK"),
         "stale output explicitly reports error and omits old measurements");
}
void TestCombinedFlowOutput()
{
  Reset();
  for (uint32_t ms = 1U; ms <= 400U; ++ms)
  {
    AtMs(ms);
    HAL_UART_TxCpltCallback(&huart2);
  }
  const char* line = reinterpret_cast<const char*>(fake.bytes);
  const char* flowLine = std::strstr(line, "FLOW ");
  Expect(flowLine && std::strstr(flowLine, "status=TRACKING") &&
         std::strstr(flowLine, "id=0x49 rev=0x01 inv=0xB6"),
         "both sensors share one owned UART buffer with verified flow identity");
  Expect(fake.length < 768U && std::strstr(line, "status=OK"), "combined output fits buffer and retains working IMU");
  FakeFlowBus::FailNextRead();
  for (uint32_t ms = 401U; ms <= 500U; ++ms)
  {
    AtMs(ms);
    HAL_UART_TxCpltCallback(&huart2);
  }
  flowLine = std::strstr(line, "FLOW ");
  Expect(flowLine && std::strstr(flowLine, "status=SPI_ERROR") && !std::strstr(flowLine, " dx="),
         "flow read failure omits old counts without suppressing the IMU stream");
  Expect(std::strstr(line, "status=OK") != nullptr, "IMU remains valid after independent flow fault");
}

void TestTofOutput()
{
  Reset();
  FakeAfbrS50::Ranging(742, 1850U, 96U);
  for (uint32_t ms = 1U; ms <= 400U; ++ms)
  {
    AtMs(ms);
    HAL_UART_TxCpltCallback(&huart2);
  }
  const char* line = reinterpret_cast<const char*>(fake.bytes);
  const char* tofLine = std::strstr(line, "TOF ");
  Expect(tofLine && std::strstr(tofLine, "range_mm=742 amp=1850 quality=96 status=RANGING") &&
         std::strstr(tofLine, "chip=0x0012ABCD"), "ranging ToF line follows IMU and FLOW in one buffer");
  Expect(std::strstr(line, "IMU ") && std::strstr(line, "FLOW ") && fake.length < 768U &&
         line[fake.length - 1U] == '\n', "three-sensor frame fits the UART buffer");

  FakeAfbrS50::InitFailed(-101);
  for (uint32_t ms = 401U; ms <= 700U; ++ms)
  {
    AtMs(ms);
    HAL_UART_TxCpltCallback(&huart2);
  }
  tofLine = std::strstr(line, "TOF ");
  Expect(tofLine && std::strstr(tofLine, "status=INIT_FAILED") && std::strstr(tofLine, "argus=-101") &&
         !std::strstr(tofLine, "range_mm="), "ToF failure reports diagnostics and omits old range");
  Expect(std::strstr(line, "status=OK") && std::strstr(line, "status=TRACKING"),
         "IMU and FLOW stay valid during an independent ToF fault");
}
}  // namespace

extern "C" uint32_t __get_PRIMASK(void) { return fake.mask; }
extern "C" void __disable_irq(void) { fake.mask = 1U; }
extern "C" void __set_PRIMASK(uint32_t mask) { fake.mask = mask; }
extern "C" void __DSB(void) {}
extern "C" void __ISB(void) {}
extern "C" uint32_t HAL_GetTick(void) { return fake.tick; }
extern "C" void HAL_GPIO_WritePin(GPIO_TypeDef*, uint16_t, GPIO_PinState state) { fake.ledState = state; ++fake.ledWrites; }
extern "C" void HAL_GPIO_TogglePin(GPIO_TypeDef*, uint16_t) { ++fake.toggles; }
extern "C" HAL_StatusTypeDef HAL_UART_Transmit_IT(UART_HandleTypeDef*, const uint8_t* bytes, uint16_t length)
{
  ++fake.starts;
  fake.startMask = fake.mask;
  fake.bytes = bytes;
  fake.length = length;
  return fake.nextStatus;
}
extern "C" HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef*)
{
  ++fake.aborts;
  fake.abortMask = fake.mask;
  return HAL_OK;
}

int main()
{
  RunScenario("UART cadence, completion and buffer ownership", TestCadenceAndCompletion);
  RunScenario("UART missed deadlines", TestMissedDeadlines);
  RunScenario("UART timeout with stopped HAL tick", TestTimeoutWithStoppedTick);
  RunScenario("UART cycle-counter rollover", TestCounterWrap);
  RunScenario("UART start failures", TestStartFailures);
  RunScenario("UART callbacks and interrupt masks", TestCallbacksAndMask);
  RunScenario("Live measurements and stale serial output", TestLiveAndStaleOutput);
  RunScenario("Combined flow and IMU output with independent faults", TestCombinedFlowOutput);
  RunScenario("ToF output and independent ToF faults", TestTofOutput);
  std::printf("9 UART/application scenarios: %s (%u failed checks)\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
