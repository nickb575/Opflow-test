#include "ImuBus.h"
#include "main.h"

extern "C" {
extern I2C_HandleTypeDef hi2c1;
volatile uint32_t ImuI2cErrorCode = 0U;
}

namespace {
constexpr uint16_t kAddress = 0x68U << 1U;  // HAL expects the shifted 7-bit address.
volatile ImuBusResult result = ImuBusResult::Idle;

class InterruptLock {
 public:
  InterruptLock() : mask_(__get_PRIMASK()) { __disable_irq(); }
  ~InterruptLock() { __set_PRIMASK(mask_); }
 private:
  uint32_t mask_;
};

ImuBusStart Start(bool write, uint8_t reg, uint8_t* bytes, uint16_t length)
{
  const InterruptLock lock;
  /* Avoid the HAL's BUSY wait. This adapter is the only I2C1 master owner. */
  if (result == ImuBusResult::Pending || HAL_I2C_GetState(&hi2c1) != HAL_I2C_STATE_READY ||
      __HAL_I2C_GET_FLAG(&hi2c1, I2C_FLAG_BUSY) != RESET)
  {
    return ImuBusStart::Busy;
  }
  result = ImuBusResult::Pending;
  const HAL_StatusTypeDef status = write
      ? HAL_I2C_Mem_Write_IT(&hi2c1, kAddress, reg, I2C_MEMADD_SIZE_8BIT, bytes, length)
      : HAL_I2C_Mem_Read_IT(&hi2c1, kAddress, reg, I2C_MEMADD_SIZE_8BIT, bytes, length);
  if (status != HAL_OK)
  {
    ImuI2cErrorCode = HAL_I2C_GetError(&hi2c1);
    result = ImuBusResult::Error;
    return status == HAL_BUSY ? ImuBusStart::Busy : ImuBusStart::Error;
  }
  return ImuBusStart::Started;
}
}  // namespace

bool ImuBus_Reset()
{
  const InterruptLock lock;
  HAL_I2C_DeInit(&hi2c1);
  __HAL_RCC_I2C1_FORCE_RESET();
  __HAL_RCC_I2C1_RELEASE_RESET();
  HAL_NVIC_ClearPendingIRQ(I2C1_EV_IRQn);
  HAL_NVIC_ClearPendingIRQ(I2C1_ER_IRQn);
  result = ImuBusResult::Idle;
  return HAL_I2C_Init(&hi2c1) == HAL_OK;
}

ImuBusStart ImuBus_Read(uint8_t reg, uint8_t* bytes, uint16_t length) { return Start(false, reg, bytes, length); }
ImuBusStart ImuBus_Write(uint8_t reg, uint8_t* bytes, uint16_t length) { return Start(true, reg, bytes, length); }
ImuBusResult ImuBus_Result() { return result; }
uint32_t ImuBus_LastError() { return ImuI2cErrorCode; }

extern "C" void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef* i2c)
{
  if (i2c == &hi2c1 && result == ImuBusResult::Pending) { result = ImuBusResult::Complete; }
}
extern "C" void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef* i2c)
{
  if (i2c == &hi2c1 && result == ImuBusResult::Pending) { result = ImuBusResult::Complete; }
}
extern "C" void HAL_I2C_ErrorCallback(I2C_HandleTypeDef* i2c)
{
  if (i2c == &hi2c1)
  {
    ImuI2cErrorCode = HAL_I2C_GetError(i2c);
    result = ImuBusResult::Error;
  }
}
