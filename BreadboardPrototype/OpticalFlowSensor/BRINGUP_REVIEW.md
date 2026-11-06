# PAW3902 bring-up review — 2026-10-27

**Implementation update:** the user confirmed D10/PB6 NCS, D9/PC7 NRESET, 2 V VDD and 3.3 V VDDIO. A combined IMU/PAW3902 driver has been built, tested and flashed. The initial `0x00` identity failure was resolved when the user restored the sensor power feed and confirmed the breakout includes its supporting components. A fresh 30-second capture then showed 600 FLOW lines with `TRACKING`, 599 IMU lines with `OK`, correct identities and zero sensor errors. See [the current bring-up guide](../NucleoMicrocontroller/Firmware/OPTICAL_FLOW_BRINGUP.md) for actual evidence and remaining controlled physical tests. The review below records the original source comparison.

## Source and current status

Reviewed [nickb575/Opflow-test](https://github.com/nickb575/Opflow-test) at commit [`c6b08598c6fad972a9fe48364b16d0d08f743481`](https://github.com/nickb575/Opflow-test/tree/c6b08598c6fad972a9fe48364b16d0d08f743481). This is an STM32F103RBT6 / STM32CubeIDE project for a PixArt PAW3902JF-TXQT, with a bright-mode initialization table, SPI1 access and USART2 text output.

This source review does not establish successful optical tracking on the breadboard. The implementation update above supersedes the initial firmware/wiring status.

## Wiring discrepancy to resolve before configuring GPIO

| Signal | Opflow-test source | Saved OpticalFlowConnections.png |
| --- | --- | --- |
| SCLK | PA5 | PA5 |
| MISO | PA6 | PA6 |
| MOSI | PA7 | PA7 |
| NCS | **PB6** | **PB5** |
| NRESET | **PC7**, driven by firmware | No STM32 reset pin specified |
| Optional motion input | PA9 configured as input, unused in acquisition | PB1 suggested |

The source mappings are in [Core/Inc/main.h](https://github.com/nickb575/Opflow-test/blob/c6b08598c6fad972a9fe48364b16d0d08f743481/Core/Inc/main.h) and [840OF.ioc](https://github.com/nickb575/Opflow-test/blob/c6b08598c6fad972a9fe48364b16d0d08f743481/840OF.ioc). Confirm the actual NCS/NRESET wires and the actual sensor/module's VDD and VDDIO supplies. The saved diagram lists a 2 V core rail and 3.3 V I/O rail; that is an existing wiring note, not a measurement of the present breadboard or a supply recommendation for an unidentified breakout.

PA5 is shared with Nucleo LD2. When assigned to SPI1 SCLK it cannot also remain the independent LED-off GPIO used by the IMU-only firmware. The MCU-to-PC connection remains USART2 through ST-LINK USB.

## What can be reused

- Sensor register definitions and the documented 12-byte motion-burst ordering.
- Bright-mode optimization sequence, including register-bank changes and the two 10 ms waits.
- Chip-select and microsecond timing structure, after checking all transfer return values.
- Text fields for motion, signed X/Y delta counts, surface quality and shutter.

The initialization header attributes its values to PX4. I compared its 97 bright-mode register writes with PX4's `ConfigureModeBright()` and found the same order and values; its final three writes correspond to PX4's separate `EnableLed()` sequence. Preserve the upstream BSD-3-Clause notice when adapting those values. References: [PX4 PAW3902.cpp](https://github.com/PX4/PX4-Autopilot/blob/main/src/drivers/optical_flow/paw3902/PAW3902.cpp), [register definitions](https://github.com/PX4/PX4-Autopilot/blob/main/src/drivers/optical_flow/paw3902/PixArt_PAW3902_Registers.hpp). The PX4 links identify the reference inspected on this review date; the nickb575 source above is pinned to a commit.

## Changes needed for integration

1. **Preserve SWD debugging.** The source MSP initializer uses `__HAL_AFIO_REMAP_SWJ_DISABLE()`, disabling SWD and JTAG. Retain the current team firmware's SWD-enabled configuration. This source setting is consistent with the earlier need to connect to the board under reset.
2. **Require successful identification.** The existing test prints Product ID `BAD` but proceeds to configure and print motion anyway. Require product `0x49`, inverse product `0xB6`, and report revision (PX4 expects `0x01`). Failed identity is an explicit error, not valid zero motion.
3. **Check every transport result.** The existing write/read/burst helpers discard HAL return codes. The burst destination in the loop is also uninitialized before a potentially failed receive. Return an explicit transport result and invalidate failed samples before publishing anything as usable data.
4. **Use the current clock configuration.** The test uses 8 MHz HSI and SPI /64 (125 kHz). The team firmware uses a 64 MHz APB2 clock; copying /64 would produce 1 MHz. Choose and document the intended SPI speed rather than copying the divider alone. The source uses mode 3, 8-bit, MSB-first, software NCS. PX4's register header specifies a 2 MHz SPI interface.
5. **Keep both sensors serviced.** Add a PAW3902 driver/bus adapter to the current C++ application. The test's long delays, blocking UART printf and separate main loop are unsuitable for direct replacement of the existing IMU scheduler and interrupt-driven UART ownership.
6. **Distinguish tracking validity from bus success.** A successful burst with no useful optical scene must not automatically be labeled valid tracking. Report quality and shutter, discard startup readings appropriately, and apply the documented bright-mode invalid-data checks. All-zero motion/quality/shutter alone does not prove that the sensor is working.
7. **Extend terminal capture deliberately.** The IMU capture currently recognizes IMU lines. Add support for explicit FLOW lines so combined firmware output is displayed and logged without being mistaken for unexpected firmware.

## First implementation milestone

After confirming the physical wiring, configure SPI1 and the correct NCS/NRESET pins. Establish valid identity before configuration, then acquire motion bursts with bounded error handling while the MPU6050 continues to run. Publish separate timestamped IMU and FLOW text, with FLOW fields for sample sequence, signed dx/dy counts, motion flag, SQUAL, shutter and status. Measurements remain raw counts at this stage; conversion to velocity needs additional distance/geometry/timing work.

Software tests should exercise burst layout, negative counts, shutter masking, identity failures, transport failures, invalid tracking and stale/recovery behavior, while retaining the existing 15 IMU/UART scenarios. Hardware evidence must separately show correct identity, valid optical scene statistics, stationary behavior, direction reversal during X/Y movement, tracking loss and recovery. Use the sensor's documented optical assembly and working distance over a textured surface.

**Remaining physical validation:** controlled X/Y movement and direction reversal, documented stationary behavior, optical tracking loss/recovery and deliberate disconnect/reconnect. Communication and valid tracking now pass with restored sensor power; those outcomes do not establish physical scale or velocity accuracy.
