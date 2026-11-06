/*****************************************************************************
 * @file    s2pi.c
 * @brief   S2PI (SPI + GPIO) layer for the AFBR-S50 platform port,
 *          STM32F103RB.
 *
 *          The ToF driver core talks to the sensor only through this layer.
 *          It needs three things from it:
 *
 *            - asynchronous, DMA-backed SPI frames with a completion
 *              callback (the core is interrupt driven and never blocks)
 *            - an interrupt callback on the sensor's IRQ_n line
 *            - the ability to take the SPI pins over as plain GPIO, which
 *              the core uses during startup to bit-bang the sensor's
 *              internal EEPROM read
 *
 *          Ported from Broadcom's STM32F4xx platform layer.  The main
 *          F4-to-F1 differences handled here:
 *
 *            - the F103 has no alternate-function selection registers.  A
 *              pin is in AF mode by virtue of being configured AF_PP with
 *              the peripheral enabled, so switching to GPIO mode means
 *              reconfiguring the pin mode and disabling SPI2.
 *            - MISO is a floating input in SPI mode and gets a pull-up in
 *              GPIO mode.
 *            - DMA on the F1 uses fixed channels: SPI2_RX is DMA1 Channel 4
 *              and SPI2_TX is DMA1 Channel 5.  CubeMX wires these up.
 *
 *          ECE 480 / AG3 Labs
 *****************************************************************************/

#include "s2pi.h"
#include "board_config.h"
#include "platform/argus_irq.h"

extern SPI_HandleTypeDef S2PI_SPI_HANDLE;   /* hspi2, from CubeMX */

/*****************************************************************************
 * Module state
 *****************************************************************************/

static struct
{
    /*! The configured slave. */
    s2pi_slave_t slave;

    /*! True while a DMA transfer is in flight. */
    volatile bool busy;

    /*! True while the SPI pins are under manual GPIO control. */
    volatile bool gpio_mode;

    /*! Simple mutex the driver uses to serialise access. */
    volatile bool mutex_taken;

    /*! Completion callback for the current transfer. */
    s2pi_callback_t callback;
    void *          callback_param;

    /*! Callback invoked when IRQ_n goes low. */
    s2pi_irq_callback_t irq_callback;
    void *              irq_param;

} g_s2pi = { 0 };

/*****************************************************************************
 * Local helpers
 *****************************************************************************/

static inline void cs_low(void)
{
    HAL_GPIO_WritePin(S2PI_CS1_GPIO, S2PI_CS1_GPIO_PIN, GPIO_PIN_RESET);
}

static inline void cs_high(void)
{
    HAL_GPIO_WritePin(S2PI_CS1_GPIO, S2PI_CS1_GPIO_PIN, GPIO_PIN_SET);
}

/*! A few hundred nanoseconds. The datasheet asks for 20 ns of chip-select
 *  setup and hold; at 64 MHz a short spin covers it with margin. */
static inline void short_delay(void)
{
    for (volatile uint32_t i = 0; i < 20U; i++) { __NOP(); }
}

/*! Paces the bit-banged EEPROM protocol.  argus_s2pi.h requires a delay
 *  after every GPIO write; 10 us keeps the software clock under 100 kHz.
 *  Without it the pins toggle at several MHz, the EEPROM read returns
 *  garbage, and Argus_Init fails with ERROR_INVALID_ARGUMENT and chip ID 0.
 *  Uses the DWT cycle counter, which App_Init enables before the ToF starts. */
static void gpio_delay(void)
{
    const uint32_t start = DWT->CYCCNT;
    const uint32_t cycles = (SystemCoreClock / 1000000U) * 10U;
    while ((uint32_t)(DWT->CYCCNT - start) < cycles) { }
}

/*! Completes the in-flight transfer and hands the result to the core. */
static void finish_transfer(status_t status)
{
    cs_high();

    s2pi_callback_t cb    = g_s2pi.callback;
    void *          param = g_s2pi.callback_param;

    g_s2pi.callback       = 0;
    g_s2pi.callback_param = 0;
    g_s2pi.busy           = false;

    if (cb != 0)
    {
        cb(status, param);
    }
}

/*****************************************************************************
 * Initialisation
 *****************************************************************************/

status_t S2PI_Init(s2pi_slave_t defaultSlave, uint32_t baudRate)
{
    (void) baudRate;   /* prescaler is fixed in CubeMX on this port */

    g_s2pi.slave          = (defaultSlave == 0) ? SPI_DEFAULT_SLAVE : defaultSlave;
    g_s2pi.busy           = false;
    g_s2pi.gpio_mode      = false;
    g_s2pi.mutex_taken    = false;
    g_s2pi.callback       = 0;
    g_s2pi.callback_param = 0;
    g_s2pi.irq_callback   = 0;
    g_s2pi.irq_param      = 0;

    cs_high();   /* deselect before anything else happens */

    /* The sensor powers up with the Nucleo and needs up to 1 ms (datasheet
     * t_poweron) to finish its power-on reset.  Talking to it sooner makes
     * the POR land mid-init, which the driver reports as -114 (register
     * data integrity lost).  Broadcom's reference port waits 3 ms here. */
    HAL_Delay(3);

    return STATUS_OK;
}

/*****************************************************************************
 * Status and mutex
 *****************************************************************************/

status_t S2PI_GetStatus(s2pi_slave_t slave)
{
    if (slave != g_s2pi.slave) { return ERROR_S2PI_INVALID_SLAVE; }
    if (g_s2pi.gpio_mode)      { return STATUS_S2PI_GPIO_MODE; }
    if (g_s2pi.busy)           { return STATUS_BUSY; }

    return STATUS_IDLE;
}

status_t S2PI_TryGetMutex(s2pi_slave_t slave)
{
    if (slave != g_s2pi.slave) { return ERROR_S2PI_INVALID_SLAVE; }

    status_t result = STATUS_BUSY;

    IRQ_LOCK();
    if (!g_s2pi.mutex_taken)
    {
        g_s2pi.mutex_taken = true;
        result = STATUS_OK;
    }
    IRQ_UNLOCK();

    return result;
}

void S2PI_ReleaseMutex(s2pi_slave_t slave)
{
    (void) slave;
    g_s2pi.mutex_taken = false;
}

/*****************************************************************************
 * SPI transfer
 *****************************************************************************/

status_t S2PI_TransferFrame(s2pi_slave_t  slave,
                            uint8_t const *txData,
                            uint8_t       *rxData,
                            size_t         frameSize,
                            s2pi_callback_t callback,
                            void          *callbackData)
{
    if (slave != g_s2pi.slave)        { return ERROR_S2PI_INVALID_SLAVE; }
    if (frameSize == 0 || txData == 0){ return ERROR_INVALID_ARGUMENT; }
    if (frameSize > 0xFFFFU)          { return ERROR_INVALID_ARGUMENT; }
    if (g_s2pi.gpio_mode)             { return ERROR_S2PI_INVALID_STATE; }

    IRQ_LOCK();
    if (g_s2pi.busy)
    {
        IRQ_UNLOCK();
        return STATUS_BUSY;
    }
    g_s2pi.busy           = true;
    g_s2pi.callback       = callback;
    g_s2pi.callback_param = callbackData;
    IRQ_UNLOCK();

    cs_low();
    short_delay();                       /* chip select setup time */

    HAL_StatusTypeDef hal_status;

    /* Always run full duplex.  A transmit-only DMA transfer leaves every
     * received byte unread, raising OVR; when the driver chains the next
     * transfer from the completion callback, the F1 HAL reports that
     * stale overrun against the new transfer and it fails.  With no
     * caller buffer, receive into one throwaway byte with the RX channel's
     * memory increment switched off.  The HAL leaves a finished channel
     * enabled, and CCR configuration writes are ignored while EN is set,
     * so disable it first; HAL_DMA_Start_IT re-enables it. */
    static uint8_t rxDiscard;
    DMA_Channel_TypeDef *const rxChannel = S2PI_SPI_HANDLE.hdmarx->Instance;
    rxChannel->CCR &= ~DMA_CCR_EN;
    if (rxData != 0)
    {
        rxChannel->CCR |= DMA_CCR_MINC;
    }
    else
    {
        rxChannel->CCR &= ~DMA_CCR_MINC;
        rxData = &rxDiscard;
    }

    /* Keep completion interrupts out until the HAL has finished setup. */
    IRQ_LOCK();
    hal_status = HAL_SPI_TransmitReceive_DMA(&S2PI_SPI_HANDLE,
                                             (uint8_t *) txData,
                                             rxData,
                                             (uint16_t) frameSize);
    IRQ_UNLOCK();

    if (hal_status != HAL_OK)
    {
        /* Return the HAL to READY; otherwise one failed start leaves the
         * handle busy and every later transfer is refused. */
        HAL_SPI_Abort(&S2PI_SPI_HANDLE);
        cs_high();
        g_s2pi.busy           = false;
        g_s2pi.callback       = 0;
        g_s2pi.callback_param = 0;
        return ERROR_FAIL;
    }

    return STATUS_OK;
}

status_t S2PI_Abort(s2pi_slave_t slave)
{
    if (slave != g_s2pi.slave) { return ERROR_S2PI_INVALID_SLAVE; }

    if (!g_s2pi.busy) { return STATUS_OK; }

    HAL_SPI_Abort(&S2PI_SPI_HANDLE);
    finish_transfer(ERROR_ABORTED);

    return STATUS_OK;
}

/*****************************************************************************
 * HAL completion callbacks
 *
 * If another peripheral in this project ever uses SPI with DMA or
 * interrupts, these are shared - hence the instance check.  SPI1 (the
 * PAW3902) uses blocking transfers, so it never lands here.
 *****************************************************************************/

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == S2PI_SPI_INSTANCE)
    {
        short_delay();               /* chip select hold time */
        finish_transfer(STATUS_OK);
    }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == S2PI_SPI_INSTANCE)
    {
        short_delay();
        finish_transfer(STATUS_OK);
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == S2PI_SPI_INSTANCE)
    {
        finish_transfer(ERROR_FAIL);
    }
}

/*****************************************************************************
 * IRQ pin
 *****************************************************************************/

status_t S2PI_SetIrqCallback(s2pi_slave_t        slave,
                             s2pi_irq_callback_t callback,
                             void               *callbackData)
{
    if (slave != g_s2pi.slave) { return ERROR_S2PI_INVALID_SLAVE; }

    g_s2pi.irq_callback = callback;
    g_s2pi.irq_param    = callbackData;

    return STATUS_OK;
}

uint32_t S2PI_ReadIrqPin(s2pi_slave_t slave)
{
    if (slave != g_s2pi.slave) { return 1U; }

    /* IRQ_n is active low: 0 means a measurement is ready. */
    return (HAL_GPIO_ReadPin(S2PI_IRQ1_GPIO, S2PI_IRQ1_GPIO_PIN) == GPIO_PIN_SET)
               ? 1U : 0U;
}

void S2PI_IrqHandler(void)
{
    if (g_s2pi.irq_callback != 0)
    {
        g_s2pi.irq_callback(g_s2pi.irq_param);
    }
}

/*! CubeMX routes EXTI15_10_IRQHandler through HAL_GPIO_EXTI_IRQHandler,
 *  which lands here.  If main.c already defines this callback, merge the
 *  body below into that copy instead. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == S2PI_IRQ1_GPIO_PIN)
    {
        S2PI_IrqHandler();
    }
}

/*****************************************************************************
 * Chip select cycling
 *****************************************************************************/

status_t S2PI_CycleCsPin(s2pi_slave_t slave)
{
    if (slave != g_s2pi.slave) { return ERROR_S2PI_INVALID_SLAVE; }
    if (g_s2pi.busy)           { return STATUS_BUSY; }

    cs_low();
    short_delay();
    cs_high();
    short_delay();

    return STATUS_OK;
}

/*****************************************************************************
 * Manual GPIO control
 *
 * The core takes the bus over as plain GPIO during startup to read the
 * sensor's calibration EEPROM, which uses a protocol the SPI peripheral
 * cannot generate.  While in this mode, SPI2 is disabled and SCK/MOSI are
 * driven as ordinary outputs.
 *****************************************************************************/

status_t S2PI_CaptureGpioControl(s2pi_slave_t slave)
{
    if (slave != g_s2pi.slave) { return ERROR_S2PI_INVALID_SLAVE; }
    if (g_s2pi.gpio_mode)      { return STATUS_OK; }
    if (g_s2pi.busy)           { return STATUS_BUSY; }

    GPIO_InitTypeDef init = { 0 };

    /* Stop the peripheral driving the pins before changing their mode. */
    __HAL_SPI_DISABLE(&S2PI_SPI_HANDLE);

    /* SCK and MOSI become ordinary push-pull outputs.  Park SCK high to
     * match SPI mode 3's idle level so the sensor sees no spurious edge. */
    HAL_GPIO_WritePin(S2PI_CLK_GPIO,  S2PI_CLK_GPIO_PIN,  GPIO_PIN_SET);
    HAL_GPIO_WritePin(S2PI_MOSI_GPIO, S2PI_MOSI_GPIO_PIN, GPIO_PIN_RESET);

    init.Mode  = GPIO_MODE_OUTPUT_PP;
    init.Pull  = GPIO_NOPULL;
    init.Speed = TOF_SPI_GPIO_SPEED;

    init.Pin = S2PI_CLK_GPIO_PIN;
    HAL_GPIO_Init(S2PI_CLK_GPIO, &init);

    init.Pin = S2PI_MOSI_GPIO_PIN;
    HAL_GPIO_Init(S2PI_MOSI_GPIO, &init);

    /* MISO gets a pull-up while bit-banging so it never floats between
     * EEPROM bits; it is a floating input again in SPI mode. */
    init.Pin  = S2PI_MISO_GPIO_PIN;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(S2PI_MISO_GPIO, &init);

    g_s2pi.gpio_mode = true;

    return STATUS_OK;
}

status_t S2PI_ReleaseGpioControl(s2pi_slave_t slave)
{
    if (slave != g_s2pi.slave) { return ERROR_S2PI_INVALID_SLAVE; }
    if (!g_s2pi.gpio_mode)     { return STATUS_OK; }

    GPIO_InitTypeDef init = { 0 };

    /* Hand SCK and MOSI back to the SPI peripheral. */
    init.Mode  = GPIO_MODE_AF_PP;
    init.Pull  = GPIO_NOPULL;
    init.Speed = TOF_SPI_GPIO_SPEED;

    init.Pin = S2PI_CLK_GPIO_PIN;
    HAL_GPIO_Init(S2PI_CLK_GPIO, &init);

    init.Pin = S2PI_MOSI_GPIO_PIN;
    HAL_GPIO_Init(S2PI_MOSI_GPIO, &init);

    init.Pin  = S2PI_MISO_GPIO_PIN;
    init.Mode = GPIO_MODE_INPUT;
    init.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(S2PI_MISO_GPIO, &init);

    __HAL_SPI_ENABLE(&S2PI_SPI_HANDLE);

    g_s2pi.gpio_mode = false;

    return STATUS_OK;
}

status_t S2PI_WriteGpioPin(s2pi_slave_t slave, s2pi_pin_t pin, uint32_t value)
{
    if (slave != g_s2pi.slave) { return ERROR_S2PI_INVALID_SLAVE; }
    if (!g_s2pi.gpio_mode)     { return ERROR_S2PI_INVALID_STATE; }
    if (value > 1U)            { return ERROR_INVALID_ARGUMENT; }

    GPIO_PinState state = (value != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET;

    switch (pin)
    {
        case S2PI_CLK:
            HAL_GPIO_WritePin(S2PI_CLK_GPIO, S2PI_CLK_GPIO_PIN, state);
            break;

        case S2PI_CS:
            HAL_GPIO_WritePin(S2PI_CS1_GPIO, S2PI_CS1_GPIO_PIN, state);
            break;

        case S2PI_MOSI:
            HAL_GPIO_WritePin(S2PI_MOSI_GPIO, S2PI_MOSI_GPIO_PIN, state);
            break;

        default:
            /* MISO and IRQ are inputs; they cannot be driven. */
            return ERROR_INVALID_ARGUMENT;
    }

    gpio_delay();
    return STATUS_OK;
}

status_t S2PI_ReadGpioPin(s2pi_slave_t slave, s2pi_pin_t pin, uint32_t *value)
{
    if (slave != g_s2pi.slave) { return ERROR_S2PI_INVALID_SLAVE; }
    if (!g_s2pi.gpio_mode)     { return ERROR_S2PI_INVALID_STATE; }
    if (value == 0)            { return ERROR_INVALID_ARGUMENT; }

    switch (pin)
    {
        case S2PI_CLK:
            *value = (HAL_GPIO_ReadPin(S2PI_CLK_GPIO, S2PI_CLK_GPIO_PIN) == GPIO_PIN_SET);
            break;

        case S2PI_CS:
            *value = (HAL_GPIO_ReadPin(S2PI_CS1_GPIO, S2PI_CS1_GPIO_PIN) == GPIO_PIN_SET);
            break;

        case S2PI_MOSI:
            *value = (HAL_GPIO_ReadPin(S2PI_MOSI_GPIO, S2PI_MOSI_GPIO_PIN) == GPIO_PIN_SET);
            break;

        case S2PI_MISO:
            *value = (HAL_GPIO_ReadPin(S2PI_MISO_GPIO, S2PI_MISO_GPIO_PIN) == GPIO_PIN_SET);
            break;

        case S2PI_IRQ:
            *value = (HAL_GPIO_ReadPin(S2PI_IRQ1_GPIO, S2PI_IRQ1_GPIO_PIN) == GPIO_PIN_SET);
            break;

        default:
            return ERROR_INVALID_ARGUMENT;
    }

    gpio_delay();   /* paced like writes, as in Broadcom's reference port */
    return STATUS_OK;
}
