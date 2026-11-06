/*****************************************************************************
 * @file    s2pi.h
 * @brief   S2PI layer for the AFBR-S50 platform port, STM32F103.
 *****************************************************************************/

#ifndef S2PI_H
#define S2PI_H

#include "platform/argus_s2pi.h"

/*!*************************************************************************
 * @brief   Initializes the S2PI layer.
 *
 *          Call once after MX_SPI2_Init() and MX_GPIO_Init().  The SPI
 *          peripheral itself is configured by CubeMX; this only sets up the
 *          chip select line and the module's internal state.
 *
 * @param   defaultSlave The slave to use (S2PI_SLAVE1).
 * @param   baudRate     Requested SPI baud rate in Hz.  Informational on
 *                       this port - the prescaler is fixed in CubeMX.
 ***************************************************************************/
status_t S2PI_Init(s2pi_slave_t defaultSlave, uint32_t baudRate);

/*!*************************************************************************
 * @brief   Called from the EXTI handler when the sensor asserts IRQ_n.
 *
 *          Hook this into HAL_GPIO_EXTI_Callback, or call it directly from
 *          EXTI15_10_IRQHandler.
 ***************************************************************************/
void S2PI_IrqHandler(void);

#endif /* S2PI_H */
