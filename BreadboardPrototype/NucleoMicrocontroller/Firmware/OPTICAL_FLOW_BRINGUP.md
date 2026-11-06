# PAW3902 bring-up

## Confirmed connections

| Signal | Nucleo label | STM32 pin |
| --- | --- | --- |
| SCLK | D13 | PA5 |
| MISO | D12 | PA6 |
| MOSI | D11 | PA7 |
| NCS | D10 | PB6 |
| NRESET | D9 | PC7 |
| VDD | 2 V rail | External core supply |
| VDDIO | 3.3 V rail | I/O supply |
| Ground | GND | Common GND |

The user confirmed NCS, NRESET and supply assignments, and later confirmed a breakout with capacitors and resistors. A missing power feed was restored; successful identity reads and tracking now establish a functioning SPI path with this setup. ST [UM1724 rev17](https://www.st.com/resource/en/user_manual/dm00105823.pdf), p40 Table12 confirms the Arduino mappings. PA5 shares LD2; SPI idle-high may illuminate it. The optional MOTION pin is unused. MPU6050 I2C1 remains on PB8/PB9.

## Documentation and implementation

Reference: [nickb575/Opflow-test at c6b08598c6fad972a9fe48364b16d0d08f743481](https://github.com/nickb575/Opflow-test/tree/c6b08598c6fad972a9fe48364b16d0d08f743481). Its initialization matches PX4's 97 bright-mode register writes and three LED_N enable writes. The full PX4 BSD-3-Clause notice is retained in `App/Src/Paw3902.cpp`.

The supplied `product260105153723_4824.pdf` is the PAW3902JF General Product Datasheet v1.1 (21 Nov 2022). Relevant references: p5 pin functions, p6 operating conditions, p7-8 serial timing, p12 reference circuit. VDD 1.8-2.1 V and VDDIO 1.8-3.6 V with VDDIO>=VDD cover the stated 2 V/3.3 V supplies. Maximum SPI is 2 MHz. The specified L214-ZSZ lens assembly requires a working height of at least 80 mm. This general PDF does not supply the complete register manual; IDs, burst fields, optimization sequence and scene validity follow the reviewed repository/[PX4 driver](https://github.com/PX4/PX4-Autopilot/blob/main/src/drivers/optical_flow/paw3902/PAW3902.cpp).

Firmware configuration:

- SPI1 mode 3, 8-bit MSB-first, software NCS, 500 kHz (64 MHz APB2 /128).
- Active-low reset, 50 ms release wait, soft reset, another 50 ms wait, then IDs `49/01/B6`. Initialization is a state machine; long waits do not block IMU/UART service.
- Clear motion registers and perform the ordered bright-mode register sequence with its two 10 ms waits.
- Read 12-byte bursts every 10 ms; the sensor internally runs bright mode at 126 fps. Discard the first three bursts from tracking.
- Decode signed little-endian deltas and 13-bit shutter. Distinguish recent bus reads from valid optical scene data. Good stationary zero counts are valid; nonzero deltas without MOT are not valid tracking.
- Expire data after 100 ms and retry faults after 1 second. Recheck identity about once per second because SPI has no acknowledge.
- SPI transactions have a 1 ms DWT deadline, leave interrupts enabled, obey command delays, and release NCS/recover on failure. `FlowSpiErrorCode` bits: 1 timeout, 2 mode fault, 4 overrun, 8 bad argument, 16 DWT unavailable, 32 peripheral reinit failed.
- IMU and FLOW lines share one persistent 512-byte UART buffer. One transmission every 50 ms produces 20 lines/s of each prefix. Typical bandwidth is about 7 kB/s; worst legal field widths fit the buffer and 115200-baud period. UART ownership/watchdog behavior is retained.

## Real terminal output

From this Firmware folder, close other COM11 readers, then run:

```powershell
./Tests/run_app_tests.ps1
./Tests/capture_sensors.ps1 -Port COM11 -Seconds 300
```

For only optical data in the terminal:

```powershell
./Tests/capture_sensors.ps1 -Port COM11 -Seconds 300 -Sensor FLOW
```

The terminal defaults to 1 update/s per sensor and shows each measurement on its own labeled line, grouped into IMU and OPTICAL FLOW blocks. Add `-Compact` to restore the original single-line console output. Use `-DisplayHz 5` for five updates/s or `-DisplayHz 20` for up to the full UART rate. Both streams are always saved as raw single-line records at the full incoming rate, regardless of terminal formatting/filter/display rate. Ctrl+C stops early and preserves flushed lines; restart the command to apply display changes. Existing `capture_imu.ps1` remains compatible. The IMU analyzer ignores valid FLOW lines and reports their count separately.

FLOW fields: MCU `t_ms`, approximate burst-start `sample_ms`, `seq`, raw signed `dx/dy`, `mot`, `squal`, `shutter`, raw image sum/min/max, observation byte, status and identity. The synchronous burst takes about 0.2 ms; the timestamp is not a hardware frame timestamp. Logged deltas are the latest 100 Hz incremental read sampled for 20 Hz UART output; the default terminal selects a 1 Hz subset of those rows. Do not sum logged/displayed deltas for total displacement or treat counts as physical velocity/distance.

| Status | Meaning |
| --- | --- |
| `INITIALIZING`, `WAITING_SAMPLE` | Startup/recovery or first sample pending |
| `TRACKING` | Recent successful burst with usable scene and motion-consistency checks |
| `NO_TRACKING` | Recent burst, but startup/optical validity checks reject tracking |
| `WRONG_ID` | One of the ID checks failed; inspect selection, reset, power and SPI wiring |
| `SPI_ERROR` | MCU transport failure; old measurement fields are omitted |
| `STALE` | No successful read for 100 ms; old measurement fields are omitted |

The current driver reads all three identity registers before checking the triple, so a wrong product ID still leaves actual revision/inverse values available for diagnostics. Transport failures stop immediately. `spi=0` means no MCU peripheral fault was detected; it does not prove a slave answered.

## Hardware evidence — 2026-10-27

**Both optical-flow and IMU hardware are now working in the combined firmware.** The user found that a power feed to the PAW3902 breakout was missing and restored it. The user also confirmed that the breakout has support capacitors and resistors. The sensor then returned product/revision/inverse IDs `49/01/B6` and usable scene readings with `status=TRACKING`, while the MPU6050 remained `status=OK`.

All 25 named host scenarios pass (8 application/UART, 8 MPU6050, 9 PAW3902). Debug and Release builds pass. Final images use 30,100/17,928 bytes flash and 3,016/3,024 bytes RAM respectively. The Debug image was programmed, verified and reset to run on the attached NUCLEO-F103RB. Earlier measured rail values were VDD 1.99 V and VDDIO 3.2 V; the successful readout followed restoration of the actual feed to the breakout.

### Successful post-repair captures

The first 5-second check after restoring power captured 100 FLOW lines reporting `TRACKING` and 100 IMU lines reporting `OK`. The existing FLOW error count stayed at 672 and the IMU count at 2, reflecting accumulated troubleshooting history rather than new errors in that check. The MCU was then reset to obtain clean counters for the fresh 30-second capture below.

Fresh raw evidence: [2026-10-27-combined-powered-30s.log](Tests/Evidence/2026-10-27-combined-powered-30s.log). It contains 600 FLOW and 599 IMU lines with zero reported errors in either stream and no unexpected lines. The sampled FLOW sequence advances from 108 to 3103, and sample timestamps from 1200 to 31150 ms. The capture starts between paired serial lines; the one-line difference is not evidence of a lost acquired sample.

| Observation | MPU6050 | PAW3902 |
| --- | --- | --- |
| Displayed lines | 599, all `OK` | 600, all `TRACKING` |
| Identity | `0x68` | Product/revision/inverse `49/01/B6` |
| Reported sensor errors | 0 throughout | 0 throughout |
| Acquisition rate from sequence/sample times | 99.93 samples/s | 100.00 samples/s |
| Display rate from MCU uptime | 20.00 lines/s | 20.00 lines/s |
| Maximum displayed sample age | 11 ms | 4 ms |
| Timestamp and sequence progress | Strictly advancing | Strictly advancing |

In the displayed FLOW subset, dx ranged from -15 to +16 counts and dy from -12 to +14 counts. SQUAL ranged 47-100 (mean 73.84), shutter 36-52 (mean 42.91), and raw image sum 87-149 (mean 118.53). MOT was set on 346 of 600 readings; no reading combined a cleared MOT flag with nonzero deltas. The IMU acceleration magnitude averaged 982.82 mg during this capture; this is an observed value, not a controlled stationary calibration result.

These observations establish optical identity, usable scene data and continuing combined acquisition. They do not establish a controlled axis-direction/reversal test, a calibrated scale, a stationary noise specification or physical disconnect/reconnect behavior.

### Earlier troubleshooting history: before the power feed was restored

In the earlier 5-second combined capture, 101 IMU and 100 FLOW lines arrived. IMU stayed `status=OK errors=0`; flow repeatedly returned `WRONG_ID id=0x00` and acquired no motion samples. The board's GPIO/SPI registers agreed with intended configuration: GPIOA_CRL `B4B44A44`, GPIOB_CRL `42484444`, GPIOC_CRL `24444444`, SPI1_CR1 `00000377`, AFIO_MAPR `02000002` (SWD retained, I2C remapped, SPI1 not remapped).

For comparison, the original optical-flow firmware backed up before the IMU restoration was temporarily loaded. Its startup reported **`Product ID = 0x00 BAD`** and continued with zero quality/shutter/motion. The combined firmware was then restored and verified. This comparison supports checking the physical communication path rather than accepting the old zero-motion text as success.

A temporary weak pull-up on PA6 still yielded product ID `0x00`; the normal floating-input configuration and ODR were restored afterward. This is consistent with the MISO line being held low, but does not identify the exact cause. Candidate checks are wrong signal/short/connection, sensor power or reset/selection. Optical texture cannot explain a wrong fixed product ID.

Saved evidence: [first combined run](Tests/Evidence/2026-10-27-combined-first.log), [original test firmware comparison](Tests/Evidence/2026-10-27-optical-reference.log), [temporary MISO pull-up](Tests/Evidence/2026-10-27-miso-pullup.log), and [latest reset-released run](Tests/Evidence/2026-10-27-combined-reset-released.log). The reference-log leading combined lines were queued before the temporary reflash; use the `--- PAW3902 bring-up ---` marker for the actual reference boot. Flash backups remain under ignored `build/hardware-tests/`.

The diagnostic update left NRESET released between retries; only the reset pulse was low. At that point all three IDs were actually read and returned `00/00/00`. MCU input snapshots were GPIOC_IDR `0000FBFF` (PC7/NRESET high), GPIOB_IDR `0000FCDF` (PB6/NCS high), and GPIOA_IDR `0000DD3F` (PA5 clock high, PA6/MISO low). These established MCU-side logic levels, not continuity or the actual power feed to the sensor pads.

During that diagnosis, the reference support circuit and sensor-side reset/select/power connections were identified as physical checks. Datasheet p12 Figure7 shows VREG pin4 with 4.7 uF and 100 nF capacitors to ground, plus separate VDD/VDDIO decoupling and SPI pull-ups. Pin4 is an internal voltage output, not another supply input. The user subsequently confirmed the supported breakout and restored its missing power feed; identity and tracking succeeded immediately afterward. The earlier zero-ID records above are retained as history, not the current hardware result.

Latest Debug ELF SHA-256: `3B25A9D7C5FBC8A78C957956BCD91F9117A5803602294194367B80F586D28237`. Host test transcript: [25 scenarios](Tests/Evidence/2026-10-27-host-tests.txt).

Remaining controlled physical validation: test the correct lens at >=80 mm over a textured illuminated surface, stationary behavior, X/Y motion and reversal, scene loss, and disconnect/reconnect. Record these outcomes separately from software tests and from the successful general tracking capture. No MAVLink, optical velocity estimate or automatic light-mode switching is included in this initial bring-up.
