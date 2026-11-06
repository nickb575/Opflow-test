/*****************************************************************************
 * @file    irq.c
 * @brief   Interrupt lock/unlock for the AFBR-S50 platform layer, STM32F103.
 *
 *          The ToF driver core is interrupt driven.  It calls IRQ_LOCK around
 *          short critical sections where it manipulates state that an ISR
 *          also touches.  These calls nest, so a plain
 *          __disable_irq/__enable_irq pair is not sufficient: an inner
 *          UNLOCK would re-enable interrupts while an outer LOCK still
 *          expects them disabled.  The counter below fixes that.
 *****************************************************************************/

#include "platform/argus_irq.h"
#include "board_config.h"

/*! Nesting depth of IRQ_LOCK calls. */
static volatile uint32_t g_irq_lock_count = 0;

/*! Saved PRIMASK from the outermost lock. */
static volatile uint32_t g_irq_primask = 0;

/*!*************************************************************************
 * @brief   Disables interrupts, supporting nested calls.
 ***************************************************************************/
void IRQ_LOCK(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();

    if (g_irq_lock_count == 0)
    {
        /* Outermost lock: remember whether interrupts were already off,
         * so we do not enable them on unlock if the caller had them
         * disabled for its own reasons. */
        g_irq_primask = primask;
    }

    g_irq_lock_count++;
}

/*!*************************************************************************
 * @brief   Re-enables interrupts once the outermost lock is released.
 ***************************************************************************/
void IRQ_UNLOCK(void)
{
    if (g_irq_lock_count == 0)
    {
        /* Unbalanced unlock. Should not happen; ignore rather than
         * underflow the counter and permanently wedge interrupts off. */
        return;
    }

    g_irq_lock_count--;

    if (g_irq_lock_count == 0 && g_irq_primask == 0)
    {
        __enable_irq();
    }
}
