/*****************************************************************************
 * @file    AfbrS50.cpp
 * @brief   Application-layer driver for the Broadcom AFBR-S50LV85D
 *          time-of-flight sensor.
 *
 *          Wraps Broadcom's precompiled driver core (libafbrs50_m3.a) in the
 *          same Begin/Service/Latest/Health shape as Paw3902 and Mpu6050, so
 *          App.cpp treats all three sensors alike.
 *
 *          Division of labour, per Broadcom's own guidance: the driver core
 *          runs the device in the background out of interrupt context, the
 *          measurement-ready callback does nothing but set a flag, and the
 *          expensive evaluation happens in Service() on the main loop.
 *
 *          ECE 480 / AG3 Labs
 *****************************************************************************/

#include "AfbrS50.h"
#include "TofHw.h"
#include "board_config.h"

extern "C" {
#include "argus.h"
#include "platform/argus_irq.h"
#include "s2pi.h"
#include "timer.h"
}

namespace {

/*! Milliseconds after which the latest sample is considered stale. The
 *  driver's default frame rate is well above 10 Hz, so 250 ms is generous. */
constexpr uint32_t kStaleAfterMs = 250U;

/*! With no frame at all for this long, the measurement loop is assumed
 *  wedged and the device is re-initialised. */
constexpr uint32_t kRestartAfterMs = 1000U;

/*! Backoff between re-initialisation attempts after a failure.  Each attempt
 *  runs Argus_Init on the main loop, which stalls the other sensors briefly,
 *  so this stays long. */
constexpr uint32_t kRetryPeriodMs = 5000U;

argus_hnd_t* g_handle = nullptr;

/*! Peripherals and the lifetime counter are brought up exactly once.  The
 *  driver requires the lifetime counter to be monotonic, so a retry must not
 *  reset it. */
bool g_hwReady = false;

/*! Frames reported by the measurement-ready callback and not yet evaluated.
 *  The driver has two raw buffers, so this can reach 2 before Service()
 *  runs, and each one must be passed to Argus_EvaluateData. */
volatile uint32_t g_pendingFrames = 0U;

/*! Most recent non-OK status reported by the callback. */
volatile status_t g_callbackError = STATUS_OK;

/*! Kept off the stack: the results struct carries per-pixel data. */
argus_results_t g_results;

/*! Q9.22 fixed point to millimetres.
 *
 *  Range is metres in Q9.22, so metres = Range / 2^22. Multiplying by 1000
 *  first would overflow int32 at long range (30 m is 1.25e11), so the
 *  intermediate is 64-bit. */
inline int32_t RangeToMillimetres(q9_22_t range)
{
  return static_cast<int32_t>((static_cast<int64_t>(range) * 1000) >> 22);
}

}  // namespace

/* C linkage: the driver core is C and invokes this through a function
 * pointer. Keep it minimal - it runs in interrupt context. */
extern "C" status_t AfbrS50_MeasurementReady(status_t status, argus_hnd_t* hnd)
{
  (void)hnd;
  if (status < STATUS_OK)
  {
    g_callbackError = status;
  }
  ++g_pendingFrames;
  return status;
}

bool AfbrS50::Start(uint32_t nowMs)
{
  ++health_.attempts;

  if (!g_hwReady)
  {
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
    g_hwReady = true;
  }
  else if (g_handle != nullptr)
  {
    /* Restarting after a failure or stall. Argus_Init rejects a handle that
     * has already been through it (ERROR_INVALID_ARGUMENT, seen on hardware),
     * so tear the old one down and start from a fresh handle. */
    (void)Argus_StopMeasurementTimer(g_handle);
    (void)Argus_Deinit(g_handle);
    Argus_DestroyHandle(g_handle);
    g_handle = nullptr;
  }

  IRQ_LOCK();
  g_pendingFrames = 0U;
  g_callbackError = STATUS_OK;
  IRQ_UNLOCK();

  if (g_handle == nullptr)
  {
    g_handle = Argus_CreateHandle();
    if (g_handle == nullptr)
    {
      Fail(TofStatus::InitFailed, nowMs);
      return false;
    }
  }

  /* Blocking, roughly 300 ms. Boots the device and reads its calibration
   * EEPROM, which the core does over bit-banged GPIO via
   * S2PI_CaptureGpioControl - so this is also what exercises the trickiest
   * part of the platform port. */
  const status_t initStatus = Argus_Init(g_handle, S2PI_SLAVE1);
  if (initStatus != STATUS_OK)
  {
    health_.lastArgusError = static_cast<int32_t>(initStatus);
    Fail(TofStatus::InitFailed, nowMs);
    return false;
  }

  health_.chipId = Argus_GetChipID(g_handle);
  health_.apiVersion = Argus_GetAPIVersion();
  health_.moduleVersion = static_cast<uint32_t>(Argus_GetModuleVersion(g_handle));

  const status_t timerStatus =
      Argus_StartMeasurementTimer(g_handle, AfbrS50_MeasurementReady);
  if (timerStatus != STATUS_OK)
  {
    health_.lastArgusError = static_cast<int32_t>(timerStatus);
    Fail(TofStatus::InitFailed, nowMs);
    return false;
  }

  health_.status = TofStatus::Ready;
  health_.hwError = 0U;
  lastSampleMs_ = nowMs;
  started_ = true;
  return true;
}

void AfbrS50::Begin(uint32_t nowMs)
{
  sample_ = TofSample{};
  health_ = TofHealth{};
  lastSampleMs_ = 0U;
  started_ = false;

  Start(nowMs);
}

void AfbrS50::Service(uint32_t nowMs)
{
  if (!started_)
  {
    /* Retry on a backoff rather than hammering a sensor that may simply
     * not be plugged in. */
    if (static_cast<uint32_t>(nowMs - retryAtMs_) < kRetryPeriodMs)
    {
      return;
    }
    Start(nowMs);
    return;
  }

  uint32_t pending;
  status_t callbackError;
  IRQ_LOCK();
  pending = g_pendingFrames;
  g_pendingFrames = 0U;
  callbackError = g_callbackError;
  g_callbackError = STATUS_OK;
  IRQ_UNLOCK();

  if (callbackError < STATUS_OK)
  {
    ++health_.errors;
    health_.lastArgusError = static_cast<int32_t>(callbackError);
  }

  if (pending == 0U)
  {
    const uint32_t sinceMs = static_cast<uint32_t>(nowMs - lastSampleMs_);
    if (sinceMs > kRestartAfterMs)
    {
      Fail(TofStatus::Stale, nowMs);
    }
    else if (sample_.valid && sinceMs > kStaleAfterMs)
    {
      health_.status = TofStatus::Stale;
    }
    return;
  }

  /* Every reported frame must be evaluated, even after an error, or its raw
   * buffer is never released and measurements stop. Heavy fixed-point work,
   * deliberately on the main loop rather than in the callback. */
  for (; pending > 0U; --pending)
  {
    const status_t evalStatus = Argus_EvaluateData(g_handle, &g_results);
    if (evalStatus == ERROR_ARGUS_BUFFER_EMPTY)
    {
      break;
    }
    if (evalStatus < STATUS_OK)
    {
      ++health_.errors;
      health_.lastArgusError = static_cast<int32_t>(evalStatus);
      continue;
    }

    sample_.timestampMs = nowMs;
    sample_.sequence = ++health_.samples;
    sample_.rangeMm = RangeToMillimetres(g_results.Bin.Range);
    /* Amplitude is UQ12.4, so sixteen LSB per unit. */
    sample_.amplitude = static_cast<uint32_t>(g_results.Bin.Amplitude) / 16U;
    sample_.quality = g_results.Bin.SignalQuality;
    sample_.valid = true;
    /* Amplitude of zero is the documented "no valid result" marker. */
    sample_.rangeValid = (g_results.Bin.Amplitude != 0U);

    lastSampleMs_ = nowMs;
    health_.status = TofStatus::Ready;
  }
}

bool AfbrS50::Fresh(uint32_t nowMs) const
{
  return sample_.valid &&
         (static_cast<uint32_t>(nowMs - lastSampleMs_) <= kStaleAfterMs);
}

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
    case TofStatus::Starting:   return "STARTING";
    case TofStatus::Ready:      return "READY";
    case TofStatus::InitFailed: return "INIT_FAILED";
    case TofStatus::BusError:   return "BUS_ERROR";
    case TofStatus::Stale:      return "STALE";
    default:                    return "UNKNOWN";
  }
}
