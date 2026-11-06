#include "FlowBus.h"
#include "main.h"

extern "C" {
extern SPI_HandleTypeDef hspi1;
/* Last adapter fault, retained across successful transfers for diagnostics. */
volatile uint32_t FlowSpiErrorCode = 0U;
}

namespace {
constexpr uint32_t kTimeoutUs = 1000U;
constexpr uint32_t kTimeout = 1U;
constexpr uint32_t kModeFault = 2U;
constexpr uint32_t kOverrun = 4U;
constexpr uint32_t kInvalidArgument = 8U;
constexpr uint32_t kClockUnavailable = 16U;
constexpr uint32_t kReinitFailed = 32U;

uint32_t CyclesForUs(uint32_t us) { return (SystemCoreClock / 1000000U) * us; }

void DelayUs(uint32_t us)
{
  const uint32_t start = DWT->CYCCNT;
  while (static_cast<uint32_t>(DWT->CYCCNT - start) < CyclesForUs(us)) { }
}

bool WaitFlag(uint32_t flag, bool set, uint32_t transactionStart)
{
  for (;;)
  {
    const uint32_t status = SPI1->SR;
    if ((status & SPI_SR_MODF) != 0U) { FlowSpiErrorCode = kModeFault; return false; }
    if ((status & SPI_SR_OVR) != 0U) { FlowSpiErrorCode = kOverrun; return false; }
    if (static_cast<uint32_t>(DWT->CYCCNT - transactionStart) >= CyclesForUs(kTimeoutUs))
    {
      FlowSpiErrorCode = kTimeout;
      return false;
    }
    if (((status & flag) != 0U) == set) { return true; }
  }
}

bool ExchangeByte(uint8_t tx, uint8_t& rx, uint32_t start)
{
  if (!WaitFlag(SPI_SR_TXE, true, start)) { return false; }
  *reinterpret_cast<volatile uint8_t*>(&SPI1->DR) = tx;
  if (!WaitFlag(SPI_SR_RXNE, true, start)) { return false; }
  rx = *reinterpret_cast<volatile uint8_t*>(&SPI1->DR);
  return true;
}

void Recover()
{
  /* End the slave transaction first; reset clears RXNE/OVR/BSY and pending data.
   * HAL_SPI_Init performs register setup only, with no tick-based wait loop. */
  HAL_GPIO_WritePin(PAW_NCS_GPIO_Port, PAW_NCS_Pin, GPIO_PIN_SET);
  __HAL_SPI_DISABLE(&hspi1);
  __HAL_RCC_SPI1_FORCE_RESET();
  __HAL_RCC_SPI1_RELEASE_RESET();
  if (HAL_SPI_Init(&hspi1) != HAL_OK) { FlowSpiErrorCode |= kReinitFailed; }
}

bool Transfer(bool write, uint8_t reg, uint8_t* bytes, uint16_t length)
{
  if (bytes == nullptr || length == 0U || length > 12U || reg > 0x7FU)
  {
    FlowSpiErrorCode = kInvalidArgument;
    HAL_GPIO_WritePin(PAW_NCS_GPIO_Port, PAW_NCS_Pin, GPIO_PIN_SET);
    return false;
  }
  if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U ||
      (CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U || SystemCoreClock < 1000000U)
  {
    FlowSpiErrorCode = kClockUnavailable;
    HAL_GPIO_WritePin(PAW_NCS_GPIO_Port, PAW_NCS_Pin, GPIO_PIN_SET);
    return false;
  }

  const uint32_t start = DWT->CYCCNT;
  if (!WaitFlag(SPI_SR_BSY, false, start)) { Recover(); return false; }
  __HAL_SPI_ENABLE(&hspi1);
  HAL_GPIO_WritePin(PAW_NCS_GPIO_Port, PAW_NCS_Pin, GPIO_PIN_RESET);
  DelayUs(1U);

  uint8_t ignored = 0U;
  if (!ExchangeByte(static_cast<uint8_t>(reg | (write ? 0x80U : 0U)), ignored, start))
  {
    Recover();
    return false;
  }
  if (!write) { DelayUs(2U); } // tSRAD, address-to-read-data delay.
  for (uint16_t i = 0U; i < length; ++i)
  {
    uint8_t received = 0U;
    if (!ExchangeByte(write ? bytes[i] : 0U, received, start))
    {
      Recover();
      return false;
    }
    if (!write) { bytes[i] = received; }
  }
  if (!WaitFlag(SPI_SR_BSY, false, start)) { Recover(); return false; }
  DelayUs(write ? 2U : 1U);
  HAL_GPIO_WritePin(PAW_NCS_GPIO_Port, PAW_NCS_Pin, GPIO_PIN_SET);
  DelayUs(write ? 11U : 2U); // Covers tSWW/tSWR or tSRW/tSRR.
  return true;
}
}  // namespace

bool FlowBus_Read(uint8_t reg, uint8_t* bytes, uint16_t length)
{
  return Transfer(false, reg, bytes, length);
}

bool FlowBus_Write(uint8_t reg, uint8_t value)
{
  return Transfer(true, reg, &value, 1U);
}

void FlowBus_SetReset(bool asserted)
{
  HAL_GPIO_WritePin(PAW_NCS_GPIO_Port, PAW_NCS_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(PAW_NRESET_GPIO_Port, PAW_NRESET_Pin, asserted ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

uint32_t FlowBus_LastError() { return FlowSpiErrorCode; }
