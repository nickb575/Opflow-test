/*****************************************************************************
 * @file    timer.c
 * @brief   Timer layer for the AFBR-S50 platform port, STM32F103.
 *
 *          Two jobs:
 *
 *          1. Lifetime counter - a monotonic microsecond clock the ranging
 *             algorithm timestamps measurements with.  Broadcom's F4 port
 *             builds this by chaining two 32-bit timers.  Every timer on the
 *             F103 is 16-bit, so this uses the software approach their own
 *             argus_timer.h documents: a 16-bit timer that wraps on a known
 *             period, plus a counter incremented in the update ISR.
 *
 *             TIM2 runs at 1 MHz and wraps every 1 ms.  CNT therefore holds
 *             microseconds within the current millisecond (0..999), and the
 *             software counter holds elapsed milliseconds.  Resolution is
 *             1 us, which is better than the ~10 us the API asks for.
 *
 *          2. Periodic interrupt timer (PIT) - TIM3, reprogrammed at runtime
 *             to fire the driver's measurement trigger at a requested
 *             interval.
 *
 *          Both timers are driven at register level rather than through the
 *          HAL TIM module.  This project's CubeMX configuration does not
 *          include HAL TIM, and keeping it that way means regenerating from
 *          the .ioc cannot break the ToF build.
 *
 *          ECE 480 / AG3 Labs
 *****************************************************************************/

#include "timer.h"
#include "board_config.h"

/*****************************************************************************
 * Lifetime counter state
 *****************************************************************************/

/*! Elapsed milliseconds since Timer_Init().  Incremented by the TIM2 update
 *  ISR.  Wraps after ~49.7 days, which is far beyond any session length. */
static volatile uint32_t g_millis = 0;

/*****************************************************************************
 * PIT state
 *****************************************************************************/

static volatile timer_cb_t g_pit_callback = 0;
static void * volatile     g_pit_param    = 0;
static uint32_t            g_pit_interval_us = 0;

static void PitStop(void)
{
    TIMER_PIT_INSTANCE->CR1  &= ~TIM_CR1_CEN;
    TIMER_PIT_INSTANCE->DIER &= ~TIM_DIER_UIE;
    TIMER_PIT_INSTANCE->SR    = ~TIM_SR_UIF;
    g_pit_interval_us = 0;
}

/*!*************************************************************************
 * @brief   Starts the lifetime counter and leaves the PIT stopped.  Call
 *          once, after TofHw_Init() has enabled the timer clocks and NVIC.
 ***************************************************************************/
void Timer_Init(void)
{
    g_millis = 0;
    g_pit_callback = 0;
    g_pit_param = 0;

    /* Keep the timers frozen while halted at a breakpoint, otherwise the
     * lifetime counter races ahead every time you stop in the debugger. */
    DBGMCU->CR |= DBGMCU_CR_DBG_TIM2_STOP | DBGMCU_CR_DBG_TIM3_STOP;

    /* Lifetime counter: 1 MHz tick, wraps every TIMER_LCT_PERIOD_US. */
    TIMER_LCT_INSTANCE->CR1  = TIM_CR1_ARPE;
    TIMER_LCT_INSTANCE->PSC  = TIMER_PRESCALER;
    TIMER_LCT_INSTANCE->ARR  = TIMER_LCT_PERIOD_US - 1U;
    TIMER_LCT_INSTANCE->CNT  = 0U;
    TIMER_LCT_INSTANCE->EGR  = TIM_EGR_UG;   /* latch PSC into its shadow */
    TIMER_LCT_INSTANCE->SR   = ~TIM_SR_UIF;  /* UG sets UIF; discard it */
    TIMER_LCT_INSTANCE->DIER = TIM_DIER_UIE;
    TIMER_LCT_INSTANCE->CR1 |= TIM_CR1_CEN;

    /* PIT stays stopped until the driver asks for an interval. */
    TIMER_PIT_INSTANCE->CR1 = TIM_CR1_ARPE;
    PitStop();
}

/*!*************************************************************************
 * @brief   Reads the lifetime counter.
 *
 * @param   hct Receives elapsed whole seconds.
 * @param   lct Receives microseconds within the current second, 0..999999.
 *
 * @details The millisecond count and the hardware counter must be sampled
 *          consistently.  The driver calls this from inside IRQ_LOCK and
 *          from interrupt context, where the TIM2 update ISR cannot run, so
 *          CNT may already have wrapped while g_millis has not advanced.
 *          A pending update flag reveals that case; the wrap is accounted
 *          for here instead, so the result never jumps backwards.
 ***************************************************************************/
void Timer_GetCounterValue(uint32_t *hct, uint32_t *lct)
{
    uint32_t ms;
    uint32_t cnt;

    do
    {
        ms = g_millis;
        const uint32_t uifBefore = TIMER_LCT_INSTANCE->SR & TIM_SR_UIF;
        cnt = TIMER_LCT_INSTANCE->CNT;
        const uint32_t uifAfter = TIMER_LCT_INSTANCE->SR & TIM_SR_UIF;

        if (uifAfter != 0U)
        {
            /* A wrap is pending.  If it happened between the two flag reads,
             * the CNT sample is ambiguous; read it again, now post-wrap. */
            if (uifBefore == 0U)
            {
                cnt = TIMER_LCT_INSTANCE->CNT;
            }
            ms += 1U;
        }
    } while (ms - g_millis > 1U);   /* the ISR ran mid-sample; try again */

    *hct = ms / 1000U;
    *lct = ((ms % 1000U) * 1000U) + cnt;
}

/*!*************************************************************************
 * @brief   Installs the PIT callback.
 ***************************************************************************/
status_t Timer_SetCallback(timer_cb_t f)
{
    g_pit_callback = f;

    if (f == 0)
    {
        /* No handler, so stop the timer rather than fire into nothing. */
        PitStop();
    }

    return STATUS_OK;
}

/*!*************************************************************************
 * @brief   Sets the periodic interrupt interval.
 *
 * @param   dt_microseconds Interval in microseconds; 0 stops the timer.
 * @param   param           Opaque pointer handed back to the callback.
 *
 * @details The timer clock is 64 MHz (TIMER_CLOCK_HZ).  A prescaler of
 *          (64*k - 1) gives a tick of k microseconds, so the period in ticks
 *          is dt/k, which must fit in the 16-bit auto-reload register.  k is
 *          chosen as the smallest value that makes it fit, keeping
 *          resolution as fine as the requested interval allows.
 *
 *          Range: 1 us up to roughly 65 seconds.
 ***************************************************************************/
status_t Timer_SetInterval(uint32_t dt_microseconds, void *param)
{
    g_pit_param = param;

    if (dt_microseconds == 0)
    {
        PitStop();
        return STATUS_OK;
    }

    if (g_pit_callback == 0)
    {
        return ERROR_NOT_INITIALIZED;
    }

    /* Already running at this interval - leave it alone rather than
     * restarting and shifting the phase. */
    if (dt_microseconds == g_pit_interval_us)
    {
        return STATUS_OK;
    }

    /* Smallest tick, in microseconds, that fits dt into 16 bits. */
    uint32_t k = (dt_microseconds + 65535U) / 65536U;
    if (k == 0) { k = 1U; }

    uint32_t prescaler = (TIMER_CLOCK_HZ / 1000000U) * k;   /* (clock_MHz) * k */
    uint32_t period    = dt_microseconds / k;

    if (prescaler > 65536U || period == 0U || period > 65536U)
    {
        return ERROR_INVALID_ARGUMENT;
    }

    PitStop();

    TIMER_PIT_INSTANCE->PSC = prescaler - 1U;
    TIMER_PIT_INSTANCE->ARR = period - 1U;
    TIMER_PIT_INSTANCE->CNT = 0U;

    /* Force the shadow registers to load the new prescaler immediately
     * instead of at the next natural update event. */
    TIMER_PIT_INSTANCE->EGR  = TIM_EGR_UG;
    TIMER_PIT_INSTANCE->SR   = ~TIM_SR_UIF;

    g_pit_interval_us = dt_microseconds;

    TIMER_PIT_INSTANCE->DIER |= TIM_DIER_UIE;
    TIMER_PIT_INSTANCE->CR1  |= TIM_CR1_CEN;

    return STATUS_OK;
}

/*****************************************************************************
 * Interrupt vectors
 *
 * These override weak symbols in the startup file.  If CubeMX is ever asked
 * to generate TIM2 or TIM3 handlers it will emit its own copies in
 * stm32f1xx_it.c and the link will fail; delete the generated ones.
 *****************************************************************************/

void TIM2_IRQHandler(void)
{
    if ((TIMER_LCT_INSTANCE->SR & TIM_SR_UIF) != 0U)
    {
        /* One millisecond elapsed.  Keep this as short as possible - the
         * lifetime counter loses time if this ISR is ever delayed past the
         * next wrap, which is why TIM2 gets the highest NVIC priority. */
        TIMER_LCT_INSTANCE->SR = ~TIM_SR_UIF;
        g_millis++;
    }
}

void TIM3_IRQHandler(void)
{
    if ((TIMER_PIT_INSTANCE->SR & TIM_SR_UIF) != 0U)
    {
        TIMER_PIT_INSTANCE->SR = ~TIM_SR_UIF;

        const timer_cb_t cb = g_pit_callback;
        if (cb != 0)
        {
            cb(g_pit_param);
        }
    }
}
