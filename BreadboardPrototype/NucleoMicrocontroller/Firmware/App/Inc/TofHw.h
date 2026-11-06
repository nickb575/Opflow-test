/*****************************************************************************
 * @file    TofHw.h
 * @brief   Peripheral setup for the AFBR-S50 time-of-flight sensor.
 *
 *          Everything the ToF needs - SPI2, its two DMA channels, the chip
 *          select and interrupt GPIOs, the TIM2/TIM3 clocks and the NVIC entries - is
 *          configured here rather than in the CubeMX-generated Core layer.
 *
 *          That is deliberate.  Keeping it in App/ means the .ioc stays the
 *          source of truth for what it already owns, and regenerating from
 *          CubeMX cannot clobber any of this.  The interrupt handlers are
 *          defined here too, overriding the weak symbols in the startup
 *          file, so Core/Src/stm32f1xx_it.c needs no edits either.
 *
 *          ECE 480 / AG3 Labs
 *****************************************************************************/

#ifndef ECE480_TOF_HW_H
#define ECE480_TOF_HW_H

#include "main.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Handle owned by this module and referenced by the platform layer.
 * TIM2 and TIM3 are driven at register level by timer.c. */
extern SPI_HandleTypeDef hspi2;

/*!*************************************************************************
 * @brief   Brings up every peripheral the ToF sensor needs.
 *
 *          Call once, before Timer_Init() and S2PI_Init().
 *
 * @return  true on success, false if any HAL init failed.
 ***************************************************************************/
bool TofHw_Init(void);

/*! Non-zero if TofHw_Init() reported a failure; the value identifies which
 *  peripheral, for diagnostics. */
uint32_t TofHw_LastError(void);

#ifdef __cplusplus
}
#endif

#endif /* ECE480_TOF_HW_H */
