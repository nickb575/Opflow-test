# AFBR-S50 ToF integration — ECE-480-Drone firmware

Adds the Broadcom AFBR-S50LV85D time-of-flight sensor alongside the MPU-6050
and PAW3902 already running on the `feature/uart-bringup` branch.

Written against the existing architecture: non-blocking `Service()` methods
driven from `App_Task`, drivers behind bus adapters, diagnostics counters
with C linkage.

---

## Verified wiring

Checked against the code, not just the diagram.

### IMU — unchanged

`__HAL_AFIO_REMAP_I2C1_ENABLE()` puts I2C1 on PB8/PB9.

| Signal | STM32 | Nucleo |
|---|---|---|
| SCL | PB8 | CN10 pin 3 |
| SDA | PB9 | CN10 pin 5 |

### PAW3902 — unchanged

| Pin | Signal | STM32 | Nucleo |
|---|---|---|---|
| 7 | NRESET | PC7 (`PAW_NRESET`) | D9 |
| 8 | MOTION | PA9 | D8 — **wired but unused** |
| 9 | MOSI | PA7 | D11 |
| 10 | SCLK | PA5 | D13 |
| 11 | MISO | PA6 | D12 |
| 12 | NCS | PB6 (`PAW_NCS`) | D10 |

`Paw3902.cpp` polls the Motion_Burst register rather than watching the
MOTION line, so PA9 is connected and idle. Harmless.

### AFBR-S50 — new

| Pin | Signal | STM32 | Nucleo |
|---|---|---|---|
| 1 | SPI_CLK | PB13 | CN10 pin 30 |
| 2 | SPI_MOSI | PB15 | CN10 pin 26 |
| 3 | SPI_MISO | PB14 | CN10 pin 28 |
| 4 | IRQ_n | PB4 | D5 (+ 10 kΩ pull-up to 3V3) |
| 15 | SPI_CS_n | PA8 | D7 |
| 6 / 8 / 13 | VDDL / VDD | — | 5 V bench supply |
| 5 / 7 / 9 / 14 | GNDL / GND | — | common ground |

Pins 10, 11, 12 unconnected.

No collisions with anything in use. PB4 is EXTI line 4, which has its own
vector — the B1 button already owns EXTI15_10.

---

## No CubeMX changes needed

Everything the ToF needs (SPI2, DMA1 Ch4/5, PA8, PB4, TIM2, TIM3, NVIC, and
the six interrupt vectors) is set up in `App/Src/TofHw.c`.

This is deliberate. The `.ioc` keeps owning what it already owns, and
regenerating from CubeMX can't clobber any of this. The IRQ handlers
override weak symbols from the startup file, so `Core/Src/stm32f1xx_it.c`
needs no edits either.

**The clock stays as it is.** I previously said you'd need to move to 72 MHz
via HSE — that was written before I'd read this codebase. You're on
HSI/2 × PLL16 = **64 MHz**, APB1 = 32 MHz, and SPI2 at prescaler 16 gives
2.0 MHz, already above the AFBR's 1 MHz floor. Leave `SystemClock_Config`
alone.

---

## Files

Copy into `BreadboardPrototype/NucleoMicrocontroller/Firmware/`:

```
App/Inc/   board_config.h  TofHw.h  s2pi.h  timer.h  AfbrS50.h
App/Src/   TofHw.c  s2pi.c  timer.c  irq.c  AfbrS50.cpp
```

Then from Broadcom's SDK (github.com/Broadcom/AFBR-S50-API, Code →
Download ZIP):

```
Drivers/AFBR-S50/Include/      the whole AFBR-S50/Include folder
Drivers/AFBR-S50/Lib/          AFBR-S50/Lib/libafbrs50_m3.a
```

`libafbrs50_m3.a` is the Cortex-M3 build. The driver is fixed-point
throughout, so no FPU is needed.

---

## CMakeLists.txt

```cmake
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    App/Src/App.cpp
    App/Src/Mpu6050.cpp
    App/Src/ImuBus.cpp
    App/Src/Paw3902.cpp
    App/Src/FlowBus.cpp
    App/Src/AfbrS50.cpp          # new
    App/Src/TofHw.c              # new
    App/Src/s2pi.c               # new
    App/Src/timer.c              # new
    App/Src/irq.c                # new
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    App/Inc
    Drivers/AFBR-S50/Include     # new
)

target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Drivers/AFBR-S50/Lib         # new
)

target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx
    afbrs50_m3                   # new
)
```

Check that the top-level `project()` call lists **C** as well as CXX and
ASM. Four of the new files are C.

---

## App.cpp — five edits

### 1. Include

```cpp
#include "Paw3902.h"
#include "FlowBus.h"
#include "AfbrS50.h"        // new
```

### 2. Diagnostic counters — inside the existing `extern "C"` block

```cpp
volatile uint32_t FlowStatusCode = 0U;
volatile uint32_t TofSampleCount = 0U;     // new
volatile uint32_t TofErrorCount = 0U;      // new
volatile uint32_t TofStatusCode = 0U;      // new
```

### 3. Instance and buffer — in the anonymous namespace

```cpp
constexpr uint32_t kUartPeriodMs = 100U;   // was 50U - see bandwidth note
constexpr uint32_t kTransmitTimeoutMs = 100U;
char uartMessage[768] = {};                // was 512
Mpu6050 imu;
Paw3902 flow;
AfbrS50 tof;                               // new
```

Both changes on that first line matter. See the bandwidth note below.

### 4. `StartTransmit` — append after the FLOW block

Insert immediately before `const InterruptLock lock;` near the end of the
function, after `length += flowLength;`:

```cpp
  const TofSample& tofSample = tof.Latest();
  const TofHealth& tofHealth = tof.Health();
  const size_t tofRemaining = sizeof(uartMessage) - static_cast<size_t>(length);
  int tofLength;
  if (tof.Fresh(nowMs))
  {
    tofLength = std::snprintf(uartMessage + length, tofRemaining,
        "TOF t_ms=%lu sample_ms=%lu seq=%lu range_mm=%ld amp=%lu quality=%u "
        "status=%s chip=0x%08lX errors=%lu\r\n",
        static_cast<unsigned long>(nowMs),
        static_cast<unsigned long>(tofSample.timestampMs),
        static_cast<unsigned long>(tofSample.sequence),
        static_cast<long>(tofSample.rangeMm),
        static_cast<unsigned long>(tofSample.amplitude),
        static_cast<unsigned>(tofSample.quality),
        tofSample.rangeValid ? "RANGING" : "NO_RANGE",
        static_cast<unsigned long>(tofHealth.chipId),
        static_cast<unsigned long>(tofHealth.errors));
  }
  else
  {
    const char* tofStatus = tofHealth.status == TofStatus::Ready
        ? (tofSample.valid ? "STALE" : "WAITING_SAMPLE")
        : AfbrS50::StatusName(tofHealth.status);
    tofLength = std::snprintf(uartMessage + length, tofRemaining,
        "TOF t_ms=%lu seq=%lu status=%s chip=0x%08lX errors=%lu attempts=%lu "
        "argus=%ld hw=0x%08lX\r\n",
        static_cast<unsigned long>(nowMs),
        static_cast<unsigned long>(tofHealth.samples), tofStatus,
        static_cast<unsigned long>(tofHealth.chipId),
        static_cast<unsigned long>(tofHealth.errors),
        static_cast<unsigned long>(tofHealth.attempts),
        static_cast<long>(tofHealth.lastArgusError),
        static_cast<unsigned long>(tofHealth.hwError));
  }
  if (tofLength <= 0 || static_cast<size_t>(tofLength) >= tofRemaining)
  {
    ++UartErrorCount;
    UartStatus = HAL_ERROR;
    return;
  }
  length += tofLength;
```

### 5. `App_Init` and `App_Task`

```cpp
extern "C" void App_Init(void)
{
  /* ... DWT setup unchanged ... */
  imu.Begin(0U);
  flow.Begin(0U);
  tof.Begin(0U);        // new - blocks ~300 ms, see note
}

extern "C" void App_Task(void)
{
  /* ... nowMs computation unchanged ... */
  imu.Service(nowMs);
  flow.Service(nowMs);
  tof.Service(nowMs);                                      // new
  /* ... existing IMU and FLOW counter copies ... */
  TofSampleCount = tof.Health().samples;                   // new
  TofErrorCount = tof.Health().errors;                     // new
  TofStatusCode = static_cast<uint32_t>(tof.Health().status);  // new
  /* ... rest unchanged ... */
}
```

---

## Two things that will bite you

### UART bandwidth — this is why `kUartPeriodMs` changes

At 20 Hz you are already using most of the link:

| | bytes/frame | at 20 Hz |
|---|---|---|
| IMU + FLOW (today) | ~420 | 84 kbit/s |
| **+ TOF** | **~590** | **118 kbit/s** |

115200 baud carries 115.2 kbit/s. Adding the TOF line at 20 Hz puts you
**over the link**, and the symptom is `UartBusyCount` climbing while
transmits start colliding with each other.

Moving `kUartPeriodMs` to **100** (10 Hz) brings it to 59 kbit/s, about
half the link. If you want 20 Hz back, raise the port to 230400 instead —
but 10 Hz is plenty for watching a bench bring-up.

### `Argus_Init` blocks for ~300 ms

That breaks this codebase's rule that nothing blocks. It's in `App_Init`
rather than `App_Task` for that reason — a one-time stall at boot, before
anything else is running. `imu.Begin()` and `flow.Begin()` only arm state
machines, so delaying them 300 ms costs nothing.

If a non-blocking boot matters later, `Argus_Init` would need splitting
into a step machine like `Paw3902` already has. Not worth it for the
prototype.

---

## Expected output

```
IMU  t_ms=... ax_mg=... status=OK id=0x68 ...
FLOW t_ms=... dx=... dy=... squal=... status=TRACKING id=0x49 ...
TOF  t_ms=... range_mm=742 amp=1850 quality=96 status=RANGING chip=0x... errors=0
```

---

## If it doesn't work

Read the TOF status field — the failure path reports which stage broke.

**`status=BUS_ERROR`, `hw=` non-zero.** Peripheral setup failed. The
bitfield says which: 1 = SPI, 2 = DMA RX, 4 = DMA TX. (TIM2/TIM3 are
programmed at register level and cannot fail init.)

**`status=INIT_FAILED`, `argus=` non-zero.** The device didn't answer.
Almost always SPI. Drop `SPI_BAUDRATEPRESCALER_16` to `_64` in `TofHw.c`
(500 kHz — below the datasheet minimum, but enough to tell whether the bus
works at all) and scope PB13 and PB14. Breadboard jumpers ring badly at
MHz rates.

**It hangs instead of returning an error.** TIM2 isn't firing. The driver
waits on the lifetime counter and will wait forever if that clock never
advances. Verify independently before suspecting SPI:

```cpp
uint32_t hct, lct;
Timer_GetCounterValue(&hct, &lct);
// hct must increment exactly once per wall-clock second
```

If `hct` drifts against a watch, the prescaler is wrong — remember TIM2/3/4
run at **APB1 × 2 = 64 MHz**, not 32 MHz, because the APB1 prescaler isn't 1.

**Ranges come out but are wrong or noisy.** Same timer suspicion. The ToF
maths is timestamp-driven, so clock error becomes distance error.

**Duplicate symbol at link time** on `HAL_GPIO_EXTI_Callback`,
`HAL_TIM_PeriodElapsedCallback`, or any `HAL_SPI_*Callback`. Something in
`Core/` now defines one too. Merge the bodies into a single definition
rather than deleting either side.

---

## Build fixes applied (2026-10-31)

The drop-in above did not build as delivered. What changed:

- **No HAL TIM.** This project's CubeMX config doesn't include the HAL TIM
  module or its sources, and `stm32f1xx_hal_conf.h` is regenerated by
  CubeMX. `timer.c` now programs TIM2/TIM3 directly and owns their IRQ
  handlers, so nothing in `Core/` or the `.ioc` needs touching.
- **Lifetime counter read is wrap-safe under `IRQ_LOCK`.** The driver reads
  the clock with interrupts masked; `Timer_GetCounterValue` now checks the
  pending update flag so time never runs backwards.
- **Every frame is evaluated.** The Argus API requires one
  `Argus_EvaluateData` per ready callback (two raw buffers). A single bool
  could drop one and stall ranging; `AfbrS50` now counts pending frames.
- **Retries don't reset the clock.** Peripherals, `Timer_Init` and
  `S2PI_Init` run once; retries only re-run `Argus_Init`. No frame for 1 s
  reports `STALE` and re-initialises after the 5 s backoff.
- **Retries use a fresh handle.** Re-running `Argus_Init` on a used handle
  returns `-5` (seen on hardware) and hides the real error; retries now
  deinit/destroy the handle and create a new one.
- **EEPROM bit-bang is paced.** `S2PI_WriteGpioPin` waits 10 us after each
  write, as `argus_s2pi.h` requires; MISO has a pull-up while in GPIO mode.
- `irq.c` includes `platform/argus_irq.h` (there was no `irq.h`);
  `TofHw.h` includes `<stdbool.h>`.

## SPI fixes from hardware bring-up (2026-11-05)

Found on hardware with a second module. Each one corrupted or blocked SPI
traffic. Together they made the sensor look dead and made it get stuck
after an MCU reset.

- **Every transfer is full duplex.** Transmit-only DMA left received bytes
  unread, and the resulting overrun (OVR) was reported against the next
  chained transfer. About half of all transfers failed. With no caller
  buffer, the transfer now receives into one discard byte with the RX
  channel's MINC bit cleared. The channel is disabled before CCR is
  changed, because the HAL leaves a finished channel enabled and the
  write is otherwise ignored.
- **TX DMA interrupt outranks RX** (`TOF_PRIO_DMA_TX` 0, `TOF_PRIO_DMA_RX` 1).
  The RX completion starts the next transfer. If it runs before the TX
  completion, the TX channel still reads busy and SPI2 is left stuck in
  BUSY_TX. A failed start now calls `HAL_SPI_Abort` so the HAL recovers.
- **3 ms power-on delay in `S2PI_Init`**, as in Broadcom's reference port.
  Without it, the first `Argus_Init` after power-up failed with -114.
- **EEPROM reads paced** as well as writes (10 us), as in the reference.
- **SPI at 2 MHz with medium slew** (`TOF_SPI_PRESCALER`,
  `TOF_SPI_GPIO_SPEED` in `board_config.h`). The 50 MHz edge setting rings
  on breadboard jumpers. 2, 4, 8 and 16 MHz all run without errors.

**Ranging works (2026-11-05)** with a third module on a breakout board:
status 0, range 0.33 m to 3.0 m, amplitude several hundred to 1600, quality
100, 25 Hz.

Two hardware faults turned up on the way, both diagnosed from the
`Auxiliary` fields of `argus_results_t` (the chip's own VDD, VDDL, TEMP and
IAPD readings):

- **Every frame -107 (APD bias dropout).** The second module reported
  TEMP = 70 C and IAPD pegged at about 4073 of 4095 with a hand over the
  lens. That detector is damaged; no software or wiring change cleared it.
- **Every frame -113 (laser malfunction)** on the third module.
  Pins 5 (GNDL) and 6 (VDDL) were not wired on the breakout. VDDL read
  about 15% below VDD. Wiring both fixed it at once.

When bringing up a module, check that **all** supply pins are connected:
VDDL (6) and VDD (8, 13) to 5 V, and GNDL (5) plus GND (7, 9, 14) to ground.
A healthy module reads TEMP near ambient and IAPD around mid-scale.

**Known limitation:** after an MCU-only reset (for example a flash) while the
sensor is measuring, the EEPROM read fails (-109, then -199) until the
sensor is power-cycled. After flashing, unplug and replug USB.

### Broadcom HAL self-test: passes

`Firmware/App/HalTest/` holds Broadcom's `argus_hal_test.c`
(`Argus_VerifyHALImplementation`, from `AFBR-S50/Test/` in the
AFBR-S50-API repo) and a stand-in `AfbrS50HalTest.cpp` that runs it at boot
instead of the driver. On 2026-11-05 all 9 tests passed on the F103RB: timer,
SPI, IRQ, EEPROM, LTC vs. device clock, PIT and SPI from interrupts. See
`hal_test_result_2026-11-05.txt`. So the platform port meets Broadcom's
requirements, and the open -107 is not a port problem.

To re-run it:
1. In `CMakeLists.txt`, replace `App/Src/AfbrS50.cpp` with
   `App/HalTest/AfbrS50HalTest.cpp` and `App/HalTest/argus_hal_test.c`,
   and add `App/HalTest` to the include directories.
2. Build, flash, then **power-cycle** (a flash-only reset fails test 6).
3. After about 15 s, read the `HalTestLog` buffer over SWD (address from
   `arm-none-eabi-nm`). `HalTestResult` is 0 on pass.
4. Revert `CMakeLists.txt`.

### Flash budget — read this before adding code

`libafbrs50_m3.a` links to ~120 KB on its own. The F103RB has 128 KB.

| Build | Flash |
|---|---|
| Original `-O0` Debug | 149.7 KB — overflows by 18 KB |
| `-Os`, no LTO | 133.4 KB — overflows by 2.3 KB |
| **`-Os -flto` (now both presets)** | **130.2 KB of 131.1 KB — ~870 B free** |

So `cmake/gcc-arm-none-eabi.cmake` now builds Debug at `-Os -g3 -flto`.
Breakpoints still work; expect some locals to show as optimized out.
App.cpp's uptime math also dropped a 64-bit divide (~750 B of libgcc).

There is effectively no room left on the Nucleo-F103RB. Anything substantial
added on the breadboard will need code removed first, or a larger-flash MCU.

---

## Sources

- Broadcom AFBR-S50 API: https://github.com/Broadcom/AFBR-S50-API
- AFBR-S50LV85D datasheet, AFBR-S50LV85D-DS103, December 2022
