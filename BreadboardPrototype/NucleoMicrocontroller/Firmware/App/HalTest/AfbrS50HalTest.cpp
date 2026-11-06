/* TEMPORARY: replaces AfbrS50.cpp to run Broadcom's HAL self-test
 * (Argus_VerifyHALImplementation, AFBR-S50/Test/argus_hal_test.c) against
 * this platform port.  The driver core is not linked in this build.
 * Output is captured in RAM (HalTestLog) for readout over SWD. */
#include <stdarg.h>
#include <stdio.h>
#include "AfbrS50.h"
#include "TofHw.h"
#include "board_config.h"

extern "C" {
#include "argus_hal_test.h"
#include "platform/argus_print.h"
#include "s2pi.h"
#include "timer.h"

char HalTestLog[8192];
volatile unsigned HalTestLen;
volatile int32_t HalTestResult = 1;   /* 1 = not run yet */

status_t print(const char *fmt_s, ...)
{
  if (HalTestLen >= sizeof(HalTestLog) - 1) { return STATUS_OK; }
  va_list ap;
  va_start(ap, fmt_s);
  int n = vsnprintf(HalTestLog + HalTestLen, sizeof(HalTestLog) - HalTestLen, fmt_s, ap);
  va_end(ap);
  if (n > 0)
  {
    HalTestLen += (unsigned)n;
    if (HalTestLen > sizeof(HalTestLog) - 1) { HalTestLen = sizeof(HalTestLog) - 1; }
  }
  return STATUS_OK;
}
}

void AfbrS50::Begin(uint32_t nowMs)
{
  sample_ = TofSample{};
  health_ = TofHealth{};
  started_ = false;
  Start(nowMs);
}

bool AfbrS50::Start(uint32_t nowMs)
{
  ++health_.attempts;
  if (!TofHw_Init())
  {
    health_.hwError = TofHw_LastError();
    Fail(TofStatus::BusError, nowMs);
    return false;
  }
  Timer_Init();
  if (S2PI_Init(S2PI_SLAVE1, SPI_MAX_BAUDRATE) != STATUS_OK)
  {
    Fail(TofStatus::BusError, nowMs);
    return false;
  }

  HalTestResult = Argus_VerifyHALImplementation(S2PI_SLAVE1);
  health_.lastArgusError = HalTestResult;
  health_.status = (HalTestResult == STATUS_OK) ? TofStatus::Ready : TofStatus::InitFailed;
  return HalTestResult == STATUS_OK;
}

void AfbrS50::Service(uint32_t nowMs) { (void)nowMs; }

bool AfbrS50::Fresh(uint32_t nowMs) const { (void)nowMs; return false; }

void AfbrS50::Fail(TofStatus status, uint32_t nowMs)
{
  ++health_.errors;
  health_.status = status;
  started_ = false;
  retryAtMs_ = nowMs;
}

const char* AfbrS50::StatusName(TofStatus status)
{
  switch (status)
  {
    case TofStatus::Ready:      return "HALTEST_PASS";
    case TofStatus::InitFailed: return "HALTEST_FAIL";
    case TofStatus::BusError:   return "BUS_ERROR";
    default:                    return "UNKNOWN";
  }
}
