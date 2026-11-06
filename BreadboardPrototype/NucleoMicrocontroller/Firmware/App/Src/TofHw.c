/*****************************************************************************
 * @file    TofHw.c
 * @brief   Peripheral setup and interrupt vectors for the AFBR-S50.
 *
 *          See TofHw.h for why this lives in App/ rather than in the
 *          CubeMX-generated Core layer.
 *
 *          ECE 480 / AG3 Labs
 *****************************************************************************/

#include "TofHw.h"
#include "board_config.h"

SPI_HandleTypeDef hspi2;

static DMA_HandleTypeDef hdma_spi2_rx;
static DMA_HandleTypeDef hdma_spi2_tx;

/* Bitfield of whichever init step failed, for diagnostics. */
static uint32_t s_lastError = 0U;

#define TOFHW_ERR_SPI      (1U << 0)
#define TOFHW_ERR_DMA_RX   (1U << 1)
#define TOFHW_ERR_DMA_TX   (1U << 2)

/*****************************************************************************
 * GPIO
 *****************************************************************************/

static void GpioInit(void)
{
    GPIO_InitTypeDef gpio = { 0 };

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();

    /* Release PB4 from its NJTRST role.  No-op if the project already runs
     * Serial Wire (it does - PB3 is SWO), but harmless and makes this file
     * correct on its own. SWD and SWO are unaffected. */
    __HAL_AFIO_REMAP_SWJ_NOJTAG();

    /* Chip select - PA8.  Idle high: CS is active low, and starting it low
     * would leave the sensor permanently selected. */
    HAL_GPIO_WritePin(S2PI_CS1_GPIO, S2PI_CS1_GPIO_PIN, GPIO_PIN_SET);
    gpio.Pin   = S2PI_CS1_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = TOF_SPI_GPIO_SPEED;
    HAL_GPIO_Init(S2PI_CS1_GPIO, &gpio);

    /* SPI2 SCK and MOSI - alternate function push-pull.  On the F103 there
     * is no AF selection register; the pin follows the peripheral once it
     * is enabled. */
    gpio.Pin   = S2PI_CLK_GPIO_PIN | S2PI_MOSI_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = TOF_SPI_GPIO_SPEED;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* SPI2 MISO - floating input. */
    gpio.Pin  = S2PI_MISO_GPIO_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(S2PI_MISO_GPIO, &gpio);

    /* IRQ_n - PB4, falling edge.  No internal pull: the external 10k to
     * 3V3 does that job, as the datasheet recommends for this open-drain
     * output. */
    gpio.Pin  = S2PI_IRQ1_GPIO_PIN;
    gpio.Mode = GPIO_MODE_IT_FALLING;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(S2PI_IRQ1_GPIO, &gpio);
}

/*****************************************************************************
 * SPI2 + DMA
 *****************************************************************************/

static bool Spi2Init(void)
{
    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    /* SPI2_RX is fixed to DMA1 Channel 4 on the F103, SPI2_TX to Channel 5.
     * Unlike the F4 there is no channel-select field - the mapping is wired
     * into the part. Neither channel is used by anything else in this
     * project (I2C1 and USART2 both run interrupt-driven, not DMA). */
    hdma_spi2_rx.Instance                 = DMA1_Channel4;
    hdma_spi2_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_spi2_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_spi2_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_spi2_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_spi2_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_spi2_rx.Init.Mode                = DMA_NORMAL;
    hdma_spi2_rx.Init.Priority            = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&hdma_spi2_rx) != HAL_OK)
    {
        s_lastError |= TOFHW_ERR_DMA_RX;
        return false;
    }
    __HAL_LINKDMA(&hspi2, hdmarx, hdma_spi2_rx);

    hdma_spi2_tx.Instance                 = DMA1_Channel5;
    hdma_spi2_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_spi2_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_spi2_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_spi2_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_spi2_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_spi2_tx.Init.Mode                = DMA_NORMAL;
    hdma_spi2_tx.Init.Priority            = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&hdma_spi2_tx) != HAL_OK)
    {
        s_lastError |= TOFHW_ERR_DMA_TX;
        return false;
    }
    __HAL_LINKDMA(&hspi2, hdmatx, hdma_spi2_tx);

    /* SPI mode 3 (CPOL = 1, CPHA = 1), which the datasheet specifies on
     * page 13.  Clock rate: see TOF_SPI_PRESCALER in board_config.h. */
    hspi2.Instance               = SPI2;
    hspi2.Init.Mode              = SPI_MODE_MASTER;
    hspi2.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity       = SPI_POLARITY_HIGH;
    hspi2.Init.CLKPhase          = SPI_PHASE_2EDGE;
    hspi2.Init.NSS               = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = TOF_SPI_PRESCALER;
    hspi2.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial     = 10;
    if (HAL_SPI_Init(&hspi2) != HAL_OK)
    {
        s_lastError |= TOFHW_ERR_SPI;
        return false;
    }

    return true;
}

/*****************************************************************************
 * Timers
 *
 * Only the clocks are enabled here.  timer.c programs TIM2 and TIM3 at
 * register level in Timer_Init(), so the HAL TIM module is not needed.
 *****************************************************************************/

static void TimersInit(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_TIM3_CLK_ENABLE();
}

/*****************************************************************************
 * NVIC
 *****************************************************************************/

static void NvicInit(void)
{
    HAL_NVIC_SetPriority(TIM2_IRQn, TOF_PRIO_TIM_LCT, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);

    HAL_NVIC_SetPriority(TIM3_IRQn, TOF_PRIO_TIM_PIT, 0);
    HAL_NVIC_EnableIRQ(TIM3_IRQn);

    HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, TOF_PRIO_DMA_RX, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);

    HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, TOF_PRIO_DMA_TX, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);

    HAL_NVIC_SetPriority(SPI2_IRQn, TOF_PRIO_SPI, 0);
    HAL_NVIC_EnableIRQ(SPI2_IRQn);

    HAL_NVIC_SetPriority(S2PI_IRQ1_EXTI, TOF_PRIO_EXTI, 0);
    HAL_NVIC_EnableIRQ(S2PI_IRQ1_EXTI);
}

/*****************************************************************************
 * Entry point
 *****************************************************************************/

bool TofHw_Init(void)
{
    s_lastError = 0U;

    GpioInit();

    if (!Spi2Init())   { return false; }
    TimersInit();

    NvicInit();

    return true;
}

uint32_t TofHw_LastError(void)
{
    return s_lastError;
}

/*****************************************************************************
 * Interrupt vectors
 *
 * These override weak symbols in the startup file.  Core/Src/stm32f1xx_it.c
 * does not define any of them, so there is no duplicate-symbol conflict -
 * but if CubeMX is ever asked to generate SPI2, DMA1 Ch4/5 or EXTI4
 * handlers, it will emit its own copies there and the link will fail.
 * In that case delete the generated ones, not these.  The TIM2 and TIM3
 * vectors live in timer.c.
 *****************************************************************************/

void DMA1_Channel4_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_spi2_rx);
}

void DMA1_Channel5_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_spi2_tx);
}

void SPI2_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&hspi2);
}

void EXTI4_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(S2PI_IRQ1_GPIO_PIN);
}
