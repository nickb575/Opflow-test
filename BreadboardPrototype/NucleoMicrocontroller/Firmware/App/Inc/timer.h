/*****************************************************************************
 * @file    timer.h
 * @brief   Timer layer for the AFBR-S50 platform port, STM32F103.
 *****************************************************************************/

#ifndef TIMER_H
#define TIMER_H

#include "platform/argus_timer.h"

/*! Set to 0 to build without the periodic interrupt timer, in which case
 *  the application must trigger measurements by calling the asynchronous
 *  API function itself. */
#ifndef TIMER_PIT_ENABLED
#define TIMER_PIT_ENABLED 1
#endif

/*!*************************************************************************
 * @brief   Starts the lifetime counter and prepares the PIT.
 *
 *          Call once, after TofHw_Init() and before Argus_Init().  Calling it
 *          again resets the lifetime counter, which the driver forbids once
 *          it is running.
 ***************************************************************************/
void Timer_Init(void);

#endif /* TIMER_H */
