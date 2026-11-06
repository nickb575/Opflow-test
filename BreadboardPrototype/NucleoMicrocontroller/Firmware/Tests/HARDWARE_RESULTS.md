# MPU6050 verification evidence — 2026-10-23

## Software checks

- Native MSVC `/W4 /WX`: **7 UART/application scenarios and 8 MPU6050 scenarios passed**, zero failed checks. The original six UART scenarios are retained.
- ARM Debug and Release builds passed. Debug: 25,424 bytes flash / 2,592 bytes static RAM; Release: 15,172 bytes flash / 2,592 bytes static RAM. Linker-reported RAM includes the configured heap/stack reservations.
- The Debug image was programmed to the connected NUCLEO-F103RB and verified by STM32CubeProgrammer 2.23.0, then reset to run.
- Flashed ELF SHA-256: `660D6574A5AEEE23B398CB8C529B0739C7061FEA3D58EEC9E5A96A69374A4521`.

Host tests simulate failures and recovery. They are not evidence that sensor wires were physically disconnected or that the board was rotated.

## Actual board / serial capture

Board: NUCLEO-F103RB, ST-LINK serial `0671FF495786534867181650`; serial reception via COM11 at 115200 8N1. Configured wiring: PB8 SCL, PB9 SDA, 3.3 V, common ground, AD0 grounded. Wiring was not physically inspected by the coding agent.

Raw evidence: [2026-10-23-initial-30s.log](Evidence/2026-10-23-initial-30s.log). Reproducible analysis: [2026-10-23-initial-30s.json](Evidence/2026-10-23-initial-30s.json), produced with `py -3 Tests/analyze_imu_log.py Tests/Evidence/2026-10-23-initial-30s.log`.

| Observation | Actual result |
| --- | --- |
| Capture duration on PC | 30.05 seconds |
| Complete serial lines | 600; no malformed lines |
| Identity/configuration | All lines `id=0x68 config=OK`; firmware permits this only after identity and all configuration reads match |
| Status/error history | 600 `status=OK`; accumulated sensor errors remained 0 |
| MCU uptime range | 76,307–106,259 ms; strictly advancing |
| Acquired sequence range | 7,612–10,606; strictly advancing |
| Acquisition rate | 99.95 samples/s, from sequence difference / sample-completion timestamp difference |
| Display rate | 20.00 lines/s, from line intervals / MCU uptime difference |
| Maximum displayed sample age | 9 ms |

The continuous-run and identity/configuration checks passed for this capture. UART watchdog/busy counters were not separately read from the debugger; the evidence establishes intact PC reception and zero sensor-reported faults during the captured interval. LD2 is held low in firmware; its physical appearance was not inspected.

## First 10-second window: observed measurements

The first 200 displayed readings span 9.95 seconds (samples are 50 ms apart). The sensor's pose and whether it remained completely motionless were not independently confirmed. These are observed statistics, **not a completed controlled stationary calibration**. They use the displayed 20 Hz subset, not all acquired 100 Hz samples.

- Acceleration magnitude: mean **989.34 mg**, standard deviation **2.37 mg** (about 0.989 g).

| Gyro axis | Mean, mdps | Standard deviation, mdps |
| --- | ---: | ---: |
| X | −1399.33 | 29.02 |
| Y | +523.45 | 29.80 |
| Z | +307.00 | 26.11 |

If confirmed stationary, the means estimate the resting gyro bias and the standard deviations estimate variation under these settings. No bias correction is applied by this firmware.

## Physical tests still to record

| Test | Status / required evidence |
| --- | --- |
| Controlled stationary ≥10 seconds | Pending confirmation of motionless pose and a dedicated capture; report acceleration magnitude and gyro means/noise |
| Six stationary orientations | Pending physical repositioning; record ±gravity on each labeled axis |
| Rotate each axis | Pending physical movement; record corresponding gyro response, reversal, return to baseline |
| Disconnect/reconnect | Pending physical wire/power intervention; record explicit error, invalid measurements and automatic recovery |

Follow the physical-test procedure in [../README.md](../README.md). Append actual dated results and log paths here. Do not replace these pending entries with software-test results.
