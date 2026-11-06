# NUCLEO-F103RB MPU6050 and PAW3902 serial verification

Open this **Firmware** folder in VS Code to edit and build the team-repository version on `feature/uart-bringup`. The older CubeMX project outside the repository is a separate copy.

## Combined IMU and optical-flow firmware

The current firmware services both the MPU6050 and PAW3902. See [OPTICAL_FLOW_BRINGUP.md](OPTICAL_FLOW_BRINGUP.md) for confirmed optical wiring, code/source references, terminal commands and the current hardware result. Both streams are published at 20 lines/s per sensor. Run `./Tests/capture_sensors.ps1 -Port COM11 -Seconds 300`; add `-Sensor FLOW` or `-Sensor IMU` to filter terminal output while preserving both streams in the log. The existing `capture_imu.ps1` remains compatible. The IMU analyzer ignores legitimate FLOW lines.

The test runner now executes 25 named scenarios: 8 UART/application, 8 MPU6050 and 9 PAW3902. The new application case checks that independent flow faults do not suppress valid IMU readings.

PA5 now belongs to SPI1 SCLK. LD2 shares that pin and may light from the idle-high mode-3 clock; the application no longer drives PA5 as an independent LED. SPI uses short transactions with a 1 ms DWT deadline and interrupts enabled. SWD, I2C1 and the existing interrupt-driven UART remain enabled. No DMA or heap allocation is added.

**Hardware status, 2026-10-27: both sensors are working in the combined firmware.** The user restored a missing power feed to the PAW3902 breakout. Optical identity now reads `49/01/B6` and valid scene readings report `TRACKING`, while the IMU remains `OK`. A fresh 30-second run after MCU reset saved 600 FLOW and 599 IMU lines, with zero reported sensor errors in both streams. Both acquired approximately 100 samples/s and displayed 20 lines/s each; timestamps and sample sequences advanced throughout. See the [raw capture](Tests/Evidence/2026-10-27-combined-powered-30s.log) and [detailed hardware result](OPTICAL_FLOW_BRINGUP.md). Controlled axis direction/reversal, scale calibration, and disconnect/reconnect tests still need to be recorded. The earlier `0x00` identity results are retained in that guide as troubleshooting history from before the power feed was restored.

## Wiring and viewing readings

| MPU6050 / GY-521 | NUCLEO-F103RB |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| SCL | PB8, remapped I2C1 SCL |
| SDA | PB9, remapped I2C1 SDA |
| AD0 | GND (7-bit address `0x68`) |

The module's I2C pull-ups must reference 3.3 V. INT is not needed: data-ready is polled over I2C. I2C transfers use interrupts, without DMA. PA5/LD2 is now shared with SPI1 as described above.

Connect the ST-LINK USB port. In VS Code: **Ctrl+Shift+P → Open Serial → COM11 → 115200 baud**, 8 data bits, no parity, 1 stop bit, no flow control. Close other readers of COM11 first.

UART output is about 20 lines/s per sensor; the terminal defaults to 1 update/s per sensor. Actual example:

```text
IMU t_ms=76307 sample_ms=76303 seq=7612 ax_mg=121 ay_mg=-10 az_mg=982 gx_mdps=-1412 gy_mdps=473 gz_mdps=274 status=OK id=0x68 config=OK errors=0
```

- `t_ms`: MCU uptime when the display update was scheduled.
- `sample_ms`: MCU uptime when the latest sample read completed, not a hardware timestamp of the sensor's sampling edge.
- `seq`: count of acquired samples; normally advances about 5 per logged UART line (100 Hz acquisition, 20 Hz UART output), or about 100 per default terminal update (1 Hz).
- `a*_mg`: acceleration; **1000 mg = 1 g**. Resting acceleration magnitude should be near 1000 mg because it includes gravity.
- `g*_mdps`: angular rate; **1000 mdps = 1 degree/second**. Resting gyro offset is reported without subtracting calibration.
- `id=0x68 config=OK`: identity and every configured register passed readback. `errors` counts sensor faults.

## Configuration and errors

I2C1 runs at 100 kHz. WHO_AM_I register `0x75` must return `0x68`. After sensor reset and a 100 ms wait, the following registers are written and read back, followed by a 50 ms settling interval:

| Register | Value | Meaning |
| --- | --- | --- |
| `0x6B` PWR_MGMT_1 | `0x01` | Awake, X gyro PLL |
| `0x6C` PWR_MGMT_2 | `0x00` | All axes enabled |
| `0x1A` CONFIG | `0x03` | DLPF: 44 Hz accel / 42 Hz gyro |
| `0x19` SMPLRT_DIV | `0x09` | 1000 / (1 + 9) = 100 Hz |
| `0x1B` GYRO_CONFIG | `0x00` | ±250 degrees/second, 131 LSB per degree/second |
| `0x1C` ACCEL_CONFIG | `0x00` | ±2 g, 16384 LSB/g |
| `0x37` INT_PIN_CFG | `0x00` | Reading INT_STATUS clears data-ready |
| `0x38` INT_ENABLE | `0x01` | Data-ready enabled |

Reference: InvenSense *MPU-6000/MPU-6050 Register Map and Descriptions*, revision 4.2, sections 4.2–4.5, 4.14–4.19 and 4.28–4.32 ([manufacturer link](https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf), [readable copy of the manufacturer document](https://hw101.tbs1.de/mpu6050/doc/MPU-6000-6050-Register-Map.pdf)). Values are decoded as signed big-endian words; temperature bytes are skipped. Integer unit conversion truncates toward zero.

Data-ready (`0x3A`, bit 0) is checked every 2 ms. A fresh notification triggers a contiguous 14-byte read from `0x3B`. Transfer completion or persistent hardware BUSY has a 10 ms deadline. After 100 ms without a fresh sample, the sample is invalid. Initialization retries once per second after a fault, preserving sample count and error history.

Error lines omit old measurement fields:

```text
IMU t_ms=1500 seq=100 status=I2C_ERROR id=0x68 errors=1 attempts=1 i2c=0x00000004
```

| Status | Meaning |
| --- | --- |
| `INITIALIZING`, `WAITING_SAMPLE` | Startup/recovery or first sample pending |
| `NO_DEVICE` | Identity transaction failed; check power, ground, PB8/PB9 and AD0 |
| `WRONG_ID` | WHO_AM_I answered with a value other than `0x68` |
| `CONFIG_MISMATCH` | Configuration readback did not match |
| `I2C_ERROR` | Later I2C transaction failed; `i2c` retains last HAL error bits (`0x04` = acknowledge failure) |
| `READ_TIMEOUT` | Transfer completion or bus availability exceeded 10 ms |
| `STALE` | No new sample for 100 ms |

Allow about one second plus initialization time after restoring wiring. An electrically stuck bus may require fixing the connection or cycling sensor power; peripheral reset cannot repair wiring. Debugger pauses are outside timing tests.

## Build and software tests

From this folder in the STM32 managed toolchain environment:

```powershell
cube-cmake --preset Debug
cube-cmake --build --preset Debug
cube-cmake --preset Release
cube-cmake --build --preset Release
./Tests/run_app_tests.ps1
```

Native tests require Visual Studio C++ build tools. They compile the actual application and drivers with fake HAL/UART and sensor transports using `/W4 /WX`. The original UART and IMU scenarios remain, with new combined-output and PAW3902 coverage described above. Outputs go under ignored `build/`.

Software tests do not establish physical sensor behavior. See [Tests/HARDWARE_RESULTS.md](Tests/HARDWARE_RESULTS.md) for actual evidence and remaining physical tests.

## Capture and physical tests

Close Serial Monitor before capturing:

```powershell
./Tests/capture_imu.ps1 -Port COM11 -Seconds 30 -OutputPath build/hardware-tests/imu.log
py -3 Tests/analyze_imu_log.py build/hardware-tests/imu.log
```

The capture command immediately saves every reading and displays a selected subset in the terminal. Use `-Seconds 300` for five minutes, or Ctrl+C to stop early; readings already written remain in the log. Add `-Quiet` to save without printing each reading. A second terminal can run `./Tests/run_app_tests.ps1` concurrently: these are host software tests with named RUN/PASS/FAIL output, and they do not use the COM port or move the physical sensor.

If the terminal shows the startup message but no measurements, the logger reports a warning every five seconds without complete serial lines. IMU and FLOW prefixes are recognized. Unexpected serial text is displayed with a `RAW:` prefix and preserved in the log, with a warning; it is never treated as recognized sensor data. For example, `MOT=`, `dX`, `dY` and `SQUAL` identify optical-flow debug output from different firmware. Build and flash the combined image from this Firmware folder, then run/continue the MCU. Selecting a source file in the editor or pulling the repository does not change the firmware already loaded on the board. The log analyzer counts unexpected lines separately as malformed, and the capture command fails if no recognized sensor lines arrived.

The terminal defaults to 1 update/s per sensor, with one labeled measurement per line in separate IMU and OPTICAL FLOW blocks. Values retain their original units and precision. Add `-Compact` for the original single-line format. Use `-DisplayHz 5` for five updates/s or `-DisplayHz 20` for up to the full UART rate. Display selection and formatting happen after each raw line is saved; acquisition and the full 20 Hz per-sensor log are unchanged. Stop a running capture with Ctrl+C and restart it to apply display changes.

The analyzer uses the Python standard library. It reports statuses, rates, timestamp/sequence progress, sample age, and first-10-second statistics. The statistics estimate stationary bias/noise only if the sensor remained still; they use the 20 Hz logged UART subset, not all 100 Hz samples. Capture 12 seconds for a dedicated stationary test so the first 10-second window is complete.

1. **Stationary:** rest for at least 10 seconds; record acceleration magnitude near 1000 mg and gyro mean/standard deviation. Record the pose and confirm it remained still.
2. **Six orientations:** hold the labeled +X/−X, +Y/−Y and +Z/−Z axes upward in turn. The upward axis should read about +1000 mg, downward about −1000 mg, and the other two near zero. Record each pose using the module's axis markings.
3. **Rotate each axis:** rotate about one axis, then reverse. Its gyro should change sign and settle near its resting baseline. Positive rotation follows the right-hand rule.
4. **Disconnect/reconnect:** log while disconnecting SDA, then restore it. Expect an explicit error with no old measurement fields, followed by recovery. Record wire and times; repeat with sensor power if needed.
5. **Continuous 30 seconds:** expect about 100 samples/s, 20 lines/s, advancing timestamps/sequence and no unexplained errors. Displayed sequence skips are expected.

Record these physical results separately from host-test passes.

## Code structure and timing

`Core/Src/main.c` initializes GPIO, I2C1, SPI1 and USART2, then calls `App_Init()` and repeatedly `App_Task()`. The C++17 layer in `App/Src/` contains:

- `Mpu6050.cpp`: identity, configuration, data-ready acquisition, conversion, freshness and recovery.
- `ImuBus.cpp`: exclusive I2C1 ownership, interrupt callbacks and peripheral reset; HAL uses address `0x68 << 1`.
- `Paw3902.cpp`: optical identity, initialization, burst decoding, optical validity, freshness and retries.
- `FlowBus.cpp`: DWT-bounded SPI transactions and chip select/reset control.
- `App.cpp`: timestamps, display pacing, text formatting, persistent UART buffer and watchdog. Busy UART updates are skipped; completion releases the buffer; lost completion aborts after 100 ms.

USART2 uses PA2 TX / PA3 RX through ST-LINK virtual COM, IRQ priority 1. I2C1 event/error IRQ priority is 2. No DMA or heap allocation is used. IMU transfers remain interrupt-driven; SPI uses short bounded CPU polling independent of SysTick. The included I2C/SPI HAL source/headers match STM32Cube F1 V1.8.7. The `.ioc` records peripheral pins, speeds and interrupts.

Debugger variables: `UartTxCount`, `UartErrorCount`, `UartBusyCount`, `UartTimeoutCount`, `UartStatus`, `LastHalTick`, `HalTickStallCount`, `ImuSampleCount`, `ImuErrorCount`, `ImuStatusCode` (the `ImuStatus` enum), and `ImuI2cErrorCode`.

Flow debugger variables: `FlowSampleCount`, `FlowErrorCount`, `FlowStatusCode` (the `FlowStatus` enum), and `FlowSpiErrorCode`.

The existing 64 MHz HSI/PLL clock stays constant. A 64-bit accumulation of DWT cycles supplies millisecond uptime independently of the HAL tick. The main loop must run at least once per cycle-counter rollover (about 67 seconds); millisecond deadlines also tolerate rollover. Missed display periods are skipped. This stage adds no orientation filter, graphs, MAVLink or flight-controller integration.
