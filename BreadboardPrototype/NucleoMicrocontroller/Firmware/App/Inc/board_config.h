/*****************************************************************************
 * @file    board_config.h
 * @brief   Board configuration for the AFBR-S50 platform layer on the
 *          NUCLEO-F103RB, as wired for the ECE 480 breadboard prototype.
 *
 *          Ported from Broadcom's STM32F4xx platform layer.
 *          Link against AFBR-S50/Lib/libafbrs50_m3.a (Cortex-M3, no FPU).
 *
 *          ECE 480 / AG3 Labs - NDAA-compliant optical flow module
 *****************************************************************************/

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "main.h"

#define BOARD_NAME "STM32F103RB-Nucleo-64"

/*****************************************************************************
 * Clock
 *
 * This project runs HSI/2 x PLL16 = 64 MHz (see SystemClock_Config in main.c).
 * There is no HSE.  Derived:
 *
 *   HCLK   = 64 MHz   (AHB  /1)
 *   PCLK1  = 32 MHz   (APB1 /2)   -> TIM2/3/4 clock = 64 MHz (APB1 x2)
 *   PCLK2  = 64 MHz   (APB2 /1)
 *
 * The APB1-x2 rule is the easy thing to get wrong here: because the APB1
 * prescaler is not 1, the timers hanging off APB1 are clocked at twice
 * PCLK1, so 64 MHz and not 32 MHz.
 *****************************************************************************/

/*! Timer input clock in Hz. */
#define TIMER_CLOCK_HZ 64000000U

/*****************************************************************************
 * SPI configuration
 *
 * The AFBR-S50 sits on SPI2.  SPI1 belongs to the PAW3902, which tops out at
 * 2 MHz, while this part requires a minimum of 1 MHz.  They cannot share.
 *
 * SPI2 on the F103 is hardwired to PB13/PB14/PB15 - there is no alternate
 * pin mapping for SPI2 on this part, which is why these three land on the
 * CN10 morpho header rather than the Arduino connectors.
 *
 * SPI2 is on APB1 (32 MHz).  Prescaler 16 gives 2.0 MHz: above the 1 MHz
 * datasheet minimum, and deliberately slow for breadboard jumper wiring.
 * On the real PCB, move to prescaler 4 (8 MHz) and update the figure below.
 *****************************************************************************/

#define S2PI_SLAVE_COUNT 1
#define S2PI_SLAVE_NONE  0
#define S2PI_SLAVE1      1

#ifndef SPI_DEFAULT_SLAVE
#define SPI_DEFAULT_SLAVE (S2PI_SLAVE1)
#endif

/* SPI2 prescaler on the 32 MHz APB1 clock.  _16 gives 2 MHz; _2 (16 MHz) is
 * the fastest SPI2 can run.  The datasheet allows 1..25 MHz and its
 * frame-rate figures assume 12 MHz or more.  An early 2 MHz build logged
 * "Integration Timeout" on every frame, but it also had the SPI overrun
 * bug.  With that fixed, 2, 4, 8 and 16 MHz all ran without SPI or frame
 * errors on the breadboard (and all gave the same -107 frame status).
 * Raise it on a PCB.  Keep SPI_MAX_BAUDRATE in step with it. */
#ifndef TOF_SPI_PRESCALER
#define TOF_SPI_PRESCALER SPI_BAUDRATEPRESCALER_16
#endif

/* Output slew for CS, SCK and MOSI.  The 50 MHz setting rings on breadboard
 * jumpers and a ringing SCK can be counted twice by the sensor, shifting
 * bits in long configuration writes.  The 10 MHz setting is ample for a
 * 4 MHz SPI clock. */
#ifndef TOF_SPI_GPIO_SPEED
#define TOF_SPI_GPIO_SPEED GPIO_SPEED_FREQ_MEDIUM
#endif

#ifndef SPI_MAX_BAUDRATE
#define SPI_MAX_BAUDRATE 2000000
#endif

#define S2PI_SPI_HANDLE   hspi2
#define S2PI_SPI_INSTANCE SPI2

/*****************************************************************************
 * Pin assignments  (ToF module pin -> STM32 -> Nucleo connector)
 *
 *    1  SPI_CLK   -> PB13  CN10 pin 30
 *    2  SPI_MOSI  -> PB15  CN10 pin 26
 *    3  SPI_MISO  -> PB14  CN10 pin 28
 *    4  IRQ_n     -> PB4   D5     (+ external 10k pull-up to 3V3)
 *   15  SPI_CS_n  -> PA8   D7
 *
 * Pins 10, 11 (CLK+/-) and 12 (Test) are left unconnected.
 *****************************************************************************/

/* SPI clock - PB13 */
#define S2PI_CLK_GPIO           GPIOB
#define S2PI_CLK_GPIO_PIN       GPIO_PIN_13
#define S2PI_CLK_PORTCLK_ENB()  __HAL_RCC_GPIOB_CLK_ENABLE()

/* SPI MISO - PB14 */
#define S2PI_MISO_GPIO          GPIOB
#define S2PI_MISO_GPIO_PIN      GPIO_PIN_14
#define S2PI_MISO_PORTCLK_ENB() __HAL_RCC_GPIOB_CLK_ENABLE()

/* SPI MOSI - PB15 */
#define S2PI_MOSI_GPIO          GPIOB
#define S2PI_MOSI_GPIO_PIN      GPIO_PIN_15
#define S2PI_MOSI_PORTCLK_ENB() __HAL_RCC_GPIOB_CLK_ENABLE()

/* Chip select (active low) - PA8.  Software controlled; SPI2 NSS unused. */
#define S2PI_CS1_GPIO           GPIOA
#define S2PI_CS1_GPIO_PIN       GPIO_PIN_8
#define S2PI_CS1_PORTCLK_ENB()  __HAL_RCC_GPIOA_CLK_ENABLE()

/* Measurement-ready interrupt (active low, open drain) - PB4.
 *
 * PB4 is EXTI line 4, which owns its own interrupt vector.  That matters
 * here: the B1 user button is on PC13 and already occupies EXTI15_10, and
 * the PAW3902's MOTION line would land on EXTI9_5.  EXTI4 collides with
 * neither.
 *
 * PB4 is also NJTRST.  It is only usable as GPIO once the debug port is
 * remapped away from full JTAG.  This project already runs Serial Wire plus
 * SWO (PB3 is labelled SWO in main.h), so that remap is in place; TofHw_Init
 * reapplies it defensively, which is a no-op if already done.
 *
 * Requires an external 10 kOhm pull-up to 3.3 V. */
#define S2PI_IRQ1_GPIO          GPIOB
#define S2PI_IRQ1_GPIO_PIN      GPIO_PIN_4
#define S2PI_IRQ1_EXTI          EXTI4_IRQn
#define S2PI_IRQ1_PORTCLK_ENB() __HAL_RCC_GPIOB_CLK_ENABLE()

/*****************************************************************************
 * Timers
 *
 * TIM2 - lifetime counter.  64 MHz / 64 = 1 MHz tick, ARR 999 -> wraps every
 *        1 ms, and the update ISR increments a software millisecond counter.
 *        This is the approach Broadcom's own argus_timer.h prescribes for
 *        parts without a 32-bit timer; every timer on the F103 is 16-bit.
 *
 * TIM3 - periodic interrupt timer.  Prescaler and period are rewritten at
 *        runtime by Timer_SetInterval().
 *****************************************************************************/

#define TIMER_LCT_HANDLE   htim2
#define TIMER_LCT_INSTANCE TIM2
#define TIMER_PIT_HANDLE   htim3
#define TIMER_PIT_INSTANCE TIM3

/*! Prescaler register value for a 1 MHz tick: 64 MHz / 64 = 1 MHz. */
#define TIMER_PRESCALER (((TIMER_CLOCK_HZ) / 1000000U) - 1U)

/*! Lifetime counter wrap period in microseconds (ARR + 1). */
#define TIMER_LCT_PERIOD_US 1000U

/*****************************************************************************
 * Interrupt priorities
 *
 * Existing in this project: EXTI15_10 (B1 button) = 0, USART2 = 1,
 * I2C1_EV/ER = 2.
 *
 * TIM2 takes 0 because the lifetime counter loses time permanently if its
 * 1 ms update is ever missed, and its ISR is a single increment - it cannot
 * meaningfully delay anything else.
 *
 * The SPI2 TX DMA channel must outrank RX.  In a full-duplex transfer the
 * RX completion ends the transfer and the driver immediately starts the
 * next one from that callback.  At 16 MHz the TX and RX completion
 * interrupts are under 1 us apart; if RX is serviced first, the TX channel
 * still reads as busy, HAL_SPI_TransmitReceive_DMA fails and SPI2 is left
 * stuck in BUSY_TX.
 *****************************************************************************/

#define TOF_PRIO_TIM_LCT 0U
#define TOF_PRIO_DMA_TX  0U
#define TOF_PRIO_DMA_RX  1U
#define TOF_PRIO_SPI     1U
#define TOF_PRIO_EXTI    2U
#define TOF_PRIO_TIM_PIT 3U

#endif /* BOARD_CONFIG_H */
