#include "App.h"
#include "Mpu6050.h"
#include "ImuBus.h"
#include "Paw3902.h"
#include "FlowBus.h"
#include "AfbrS50.h"
#include "main.h"
#include <cstdio>

extern "C" {
extern UART_HandleTypeDef huart2;

/* C linkage keeps the diagnostic names convenient in the debugger. */
volatile uint32_t UartTxCount = 0U;
volatile uint32_t UartErrorCount = 0U;
volatile uint32_t UartBusyCount = 0U;
volatile uint32_t UartTimeoutCount = 0U;
volatile uint32_t LastHalTick = 0U;
volatile uint32_t HalTickStallCount = 0U;
volatile HAL_StatusTypeDef UartStatus = HAL_OK;
volatile uint32_t ImuSampleCount = 0U;
volatile uint32_t ImuErrorCount = 0U;
volatile uint32_t ImuStatusCode = 0U;
volatile uint32_t FlowSampleCount = 0U;
volatile uint32_t FlowErrorCount = 0U;
volatile uint32_t FlowStatusCode = 0U;
volatile uint32_t TofSampleCount = 0U;
volatile uint32_t TofErrorCount = 0U;
volatile uint32_t TofStatusCode = 0U;
}

namespace {
/* 10 Hz. At 20 Hz the IMU+FLOW+TOF lines exceed 115200 baud. */
constexpr uint32_t kUartPeriodMs = 100U;
constexpr uint32_t kTransmitTimeoutMs = 100U;
char uartMessage[768] = {};
Mpu6050 imu;
Paw3902 flow;
AfbrS50 tof;
uint32_t previousCycles = 0U;
/* Uptime as whole milliseconds plus leftover cycles. Kept in 32 bits: a
 * 64-bit divide pulls in ~750 bytes of libgcc, and flash is nearly full. */
uint32_t elapsedMs = 0U;
uint32_t leftoverCycles = 0U;
uint32_t cyclesPerMs = 1U;

uint32_t uartPeriodCycles = 0U;
uint32_t transmitTimeoutCycles = 0U;
uint32_t lastUartCycles = 0U;
uint32_t transmitStartCycles = 0U;
volatile bool transmitActive = false;

/* Protect state shared with UART callbacks, preserving the caller's mask. */
class InterruptLock {
 public:
  InterruptLock() : savedMask_(__get_PRIMASK()) { __disable_irq(); }
  ~InterruptLock() { __set_PRIMASK(savedMask_); }
  InterruptLock(const InterruptLock&) = delete;
  InterruptLock& operator=(const InterruptLock&) = delete;

 private:
  uint32_t savedMask_;
};

void CheckTransmitTimeout(uint32_t nowCycles)
{
  const InterruptLock lock;
  if (!transmitActive ||
      static_cast<uint32_t>(nowCycles - transmitStartCycles) < transmitTimeoutCycles)
  {
    return;
  }

  /* With interrupt-only TX and no DMA, this HAL abort has no wait loop. */
  HAL_UART_AbortTransmit(&huart2);
  transmitActive = false;
  UartStatus = HAL_TIMEOUT;
  ++UartTimeoutCount;
  ++UartErrorCount;
}

void StartTransmit(uint32_t nowCycles, uint32_t nowMs)
{
  {
    const InterruptLock lock;
    if (transmitActive)
    {
      ++UartBusyCount;
      return;
    }
  }

  /* Format only while idle. IRQ transmission owns this buffer until completion. */
  const ImuSample& sample = imu.Latest();
  const ImuHealth& health = imu.Health();
  int length;
  if (imu.Fresh(nowMs))
  {
    length = std::snprintf(uartMessage, sizeof(uartMessage),
        "IMU t_ms=%lu sample_ms=%lu seq=%lu ax_mg=%ld ay_mg=%ld az_mg=%ld "
        "gx_mdps=%ld gy_mdps=%ld gz_mdps=%ld status=OK id=0x%02X config=OK errors=%lu\r\n",
        static_cast<unsigned long>(nowMs), static_cast<unsigned long>(sample.timestampMs),
        static_cast<unsigned long>(sample.sequence),
        static_cast<long>(sample.accelMg[0]), static_cast<long>(sample.accelMg[1]),
        static_cast<long>(sample.accelMg[2]), static_cast<long>(sample.gyroMdps[0]),
        static_cast<long>(sample.gyroMdps[1]), static_cast<long>(sample.gyroMdps[2]),
        static_cast<unsigned>(health.identity), static_cast<unsigned long>(health.errors));
  }
  else
  {
    const char* status = health.status == ImuStatus::Ready
        ? (sample.valid ? "STALE" : "WAITING_SAMPLE") : Mpu6050::StatusName(health.status);
    length = std::snprintf(uartMessage, sizeof(uartMessage),
        "IMU t_ms=%lu seq=%lu status=%s id=0x%02X errors=%lu attempts=%lu i2c=0x%08lX\r\n",
        static_cast<unsigned long>(nowMs), static_cast<unsigned long>(health.samples), status,
        static_cast<unsigned>(health.identity), static_cast<unsigned long>(health.errors),
        static_cast<unsigned long>(health.attempts), static_cast<unsigned long>(ImuBus_LastError()));
  }
  if (length <= 0 || static_cast<unsigned>(length) >= sizeof(uartMessage))
  {
    ++UartErrorCount;
    UartStatus = HAL_ERROR;
    return;
  }
  const FlowSample& flowSample = flow.Latest();
  const FlowHealth& flowHealth = flow.Health();
  const size_t remaining = sizeof(uartMessage) - static_cast<size_t>(length);
  int flowLength;
  if (flow.Fresh(nowMs))
  {
    flowLength = std::snprintf(uartMessage + length, remaining,
        "FLOW t_ms=%lu sample_ms=%lu seq=%lu dx=%ld dy=%ld mot=%u squal=%u shutter=%u "
        "rawsum=%u rawmin=%u rawmax=%u obs=0x%02X status=%s id=0x%02X rev=0x%02X inv=0x%02X errors=%lu\r\n",
        static_cast<unsigned long>(nowMs), static_cast<unsigned long>(flowSample.timestampMs),
        static_cast<unsigned long>(flowSample.sequence), static_cast<long>(flowSample.dx),
        static_cast<long>(flowSample.dy), static_cast<unsigned>((flowSample.motion >> 7U) & 1U),
        static_cast<unsigned>(flowSample.squal), static_cast<unsigned>(flowSample.shutter),
        static_cast<unsigned>(flowSample.rawSum), static_cast<unsigned>(flowSample.rawMin),
        static_cast<unsigned>(flowSample.rawMax), static_cast<unsigned>(flowSample.observation),
        flowSample.trackingValid ? "TRACKING" : "NO_TRACKING",
        static_cast<unsigned>(flowHealth.productId), static_cast<unsigned>(flowHealth.revisionId),
        static_cast<unsigned>(flowHealth.inverseId), static_cast<unsigned long>(flowHealth.errors));
  }
  else
  {
    const char* status = flowHealth.status == FlowStatus::Ready
        ? (flowSample.valid ? "STALE" : "WAITING_SAMPLE") : Paw3902::StatusName(flowHealth.status);
    flowLength = std::snprintf(uartMessage + length, remaining,
        "FLOW t_ms=%lu seq=%lu status=%s id=0x%02X rev=0x%02X inv=0x%02X errors=%lu attempts=%lu spi=0x%08lX\r\n",
        static_cast<unsigned long>(nowMs), static_cast<unsigned long>(flowHealth.samples), status,
        static_cast<unsigned>(flowHealth.productId), static_cast<unsigned>(flowHealth.revisionId),
        static_cast<unsigned>(flowHealth.inverseId), static_cast<unsigned long>(flowHealth.errors),
        static_cast<unsigned long>(flowHealth.attempts), static_cast<unsigned long>(FlowBus_LastError()));
  }
  if (flowLength <= 0 || static_cast<size_t>(flowLength) >= remaining)
  {
    ++UartErrorCount;
    UartStatus = HAL_ERROR;
    return;
  }
  length += flowLength;

  const TofSample& tofSample = tof.Latest();
  const TofHealth& tofHealth = tof.Health();
  const size_t tofRemaining = sizeof(uartMessage) - static_cast<size_t>(length);
  int tofLength;
  if (tof.Fresh(nowMs))
  {
    tofLength = std::snprintf(uartMessage + length, tofRemaining,
        "TOF t_ms=%lu sample_ms=%lu seq=%lu range_mm=%ld amp=%lu quality=%u "
        "status=%s chip=0x%08lX errors=%lu\r\n",
        static_cast<unsigned long>(nowMs),
        static_cast<unsigned long>(tofSample.timestampMs),
        static_cast<unsigned long>(tofSample.sequence),
        static_cast<long>(tofSample.rangeMm),
        static_cast<unsigned long>(tofSample.amplitude),
        static_cast<unsigned>(tofSample.quality),
        tofSample.rangeValid ? "RANGING" : "NO_RANGE",
        static_cast<unsigned long>(tofHealth.chipId),
        static_cast<unsigned long>(tofHealth.errors));
  }
  else
  {
    const char* tofStatus = tofHealth.status == TofStatus::Ready
        ? (tofSample.valid ? "STALE" : "WAITING_SAMPLE")
        : AfbrS50::StatusName(tofHealth.status);
    tofLength = std::snprintf(uartMessage + length, tofRemaining,
        "TOF t_ms=%lu seq=%lu status=%s chip=0x%08lX errors=%lu attempts=%lu "
        "argus=%ld hw=0x%08lX\r\n",
        static_cast<unsigned long>(nowMs),
        static_cast<unsigned long>(tofHealth.samples), tofStatus,
        static_cast<unsigned long>(tofHealth.chipId),
        static_cast<unsigned long>(tofHealth.errors),
        static_cast<unsigned long>(tofHealth.attempts),
        static_cast<long>(tofHealth.lastArgusError),
        static_cast<unsigned long>(tofHealth.hwError));
  }
  if (tofLength <= 0 || static_cast<size_t>(tofLength) >= tofRemaining)
  {
    ++UartErrorCount;
    UartStatus = HAL_ERROR;
    return;
  }
  length += tofLength;
  const InterruptLock lock;
  transmitStartCycles = nowCycles;
  transmitActive = true;
  UartStatus = HAL_UART_Transmit_IT(
      &huart2, reinterpret_cast<const uint8_t*>(uartMessage), static_cast<uint16_t>(length));
  if (UartStatus != HAL_OK)
  {
    transmitActive = false;
    if (UartStatus == HAL_BUSY)
    {
      ++UartBusyCount;
    }
    else
    {
      ++UartErrorCount;
    }
  }
}
}  // namespace

extern "C" void App_Init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  __DSB();
  __ISB();

  uartPeriodCycles = (SystemCoreClock / 1000U) * kUartPeriodMs;
  transmitTimeoutCycles = (SystemCoreClock / 1000U) * kTransmitTimeoutMs;
  lastUartCycles = DWT->CYCCNT;
  previousCycles = lastUartCycles;
  elapsedMs = 0U;
  leftoverCycles = 0U;
  cyclesPerMs = SystemCoreClock / 1000U;
  LastHalTick = HAL_GetTick();
  imu.Begin(0U);
  flow.Begin(0U);
  /* Blocks ~300 ms performing the device boot and EEPROM read. */
  tof.Begin(0U);
}

extern "C" void App_Task(void)
{
  const uint32_t nowCycles = DWT->CYCCNT;
  const uint32_t deltaCycles = static_cast<uint32_t>(nowCycles - previousCycles);
  previousCycles = nowCycles;
  elapsedMs += deltaCycles / cyclesPerMs;
  leftoverCycles += deltaCycles % cyclesPerMs;
  if (leftoverCycles >= cyclesPerMs)
  {
    leftoverCycles -= cyclesPerMs;
    ++elapsedMs;
  }
  const uint32_t nowMs = elapsedMs;
  imu.Service(nowMs);
  flow.Service(nowMs);
  tof.Service(nowMs);
  ImuSampleCount = imu.Health().samples;
  ImuErrorCount = imu.Health().errors;
  ImuStatusCode = static_cast<uint32_t>(imu.Health().status);
  FlowSampleCount = flow.Health().samples;
  FlowErrorCount = flow.Health().errors;
  FlowStatusCode = static_cast<uint32_t>(flow.Health().status);
  TofSampleCount = tof.Health().samples;
  TofErrorCount = tof.Health().errors;
  TofStatusCode = static_cast<uint32_t>(tof.Health().status);
  CheckTransmitTimeout(nowCycles);
  if (static_cast<uint32_t>(nowCycles - lastUartCycles) < uartPeriodCycles)
  {
    return;
  }

  /* Skip missed deadlines instead of replaying a burst after a long pause. */
  lastUartCycles = nowCycles;

  const uint32_t tick = HAL_GetTick();
  if (tick == LastHalTick)
  {
    ++HalTickStallCount;
  }
  LastHalTick = tick;

  /* Interrupt TX and the DWT watchdog remain independent of the HAL tick. */
  StartTransmit(nowCycles, nowMs);
}

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* uart)
{
  if (uart == &huart2 && transmitActive)
  {
    transmitActive = false;
    UartStatus = HAL_OK;
    ++UartTxCount;
  }
}

extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* uart)
{
  if (uart == &huart2)
  {
    HAL_UART_AbortTransmit(uart);
    transmitActive = false;
    UartStatus = HAL_ERROR;
    ++UartErrorCount;
  }
}
