"""Summarize serial evidence; uses only the Python standard library.

Window statistics describe logged UART (20 Hz) readings, not all 100 Hz samples.
They establish a stationary bias/noise measurement only if the sensor was still.
"""
import argparse
from collections import Counter
import json
import math
from pathlib import Path
import statistics


def delta(later, earlier):
    return (later - earlier) & 0xFFFFFFFF


def summarize(path, stats_seconds):
    rows = []
    malformed = 0
    other_sensor_lines = 0
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if line.startswith(("FLOW ", "TOF ")):
            other_sensor_lines += 1
            continue
        if not line.startswith("IMU "):
            malformed += 1
            continue
        fields = dict(item.split("=", 1) for item in line.split()[1:] if "=" in item)
        try:
            for key in ("t_ms", "seq", "errors"):
                fields[key] = int(fields[key])
            if fields["status"] == "OK":
                for key in ("sample_ms", "ax_mg", "ay_mg", "az_mg", "gx_mdps", "gy_mdps", "gz_mdps"):
                    fields[key] = int(fields[key])
            rows.append(fields)
        except (KeyError, ValueError):
            malformed += 1
    if len(rows) < 2:
        raise ValueError("Need at least two complete IMU lines")
    good = [row for row in rows if row["status"] == "OK"]
    duration_ms = delta(rows[-1]["t_ms"], rows[0]["t_ms"])
    result = {
        "source": str(path), "lines": len(rows), "malformed_lines": malformed,
        "other_sensor_lines": other_sensor_lines,
        "status_counts": dict(Counter(row["status"] for row in rows)),
        "mcu_span_seconds": duration_ms / 1000,
        "display_lines_per_second": (len(rows) - 1) * 1000 / duration_ms if duration_ms else None,
        "max_reported_errors": max(row["errors"] for row in rows),
        "timestamp_advances": all(0 < delta(b["t_ms"], a["t_ms"]) < 0x80000000 for a, b in zip(rows, rows[1:])),
    }
    if len(good) > 1:
        span = delta(good[-1]["sample_ms"], good[0]["sample_ms"])
        result.update({
            "samples_per_second": delta(good[-1]["seq"], good[0]["seq"]) * 1000 / span if span else None,
            "sample_sequence_advances": all(0 < delta(b["seq"], a["seq"]) < 0x80000000 for a, b in zip(good, good[1:])),
            "maximum_sample_age_ms": max(delta(row["t_ms"], row["sample_ms"]) for row in good),
        })
        window = [row for row in good if delta(row["t_ms"], good[0]["t_ms"]) < stats_seconds * 1000]
        norms = [math.sqrt(sum(row[axis] ** 2 for axis in ("ax_mg", "ay_mg", "az_mg"))) for row in window]
        result["first_window"] = {
            "requested_seconds": stats_seconds, "displayed_samples": len(window),
            "observed_span_seconds": delta(window[-1]["t_ms"], window[0]["t_ms"]) / 1000,
            "acceleration_magnitude_mean_mg": statistics.mean(norms),
            "acceleration_magnitude_stddev_mg": statistics.pstdev(norms),
            "gyro_mean_mdps": {axis: statistics.mean(row[axis] for row in window) for axis in ("gx_mdps", "gy_mdps", "gz_mdps")},
            "gyro_stddev_mdps": {axis: statistics.pstdev(row[axis] for row in window) for axis in ("gx_mdps", "gy_mdps", "gz_mdps")},
            "interpretation": "Bias/noise estimate requires the sensor to have remained stationary throughout this window; uses the logged 20 Hz subset.",
        }
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--stats-seconds", type=float, default=10)
    args = parser.parse_args()
    if args.stats_seconds <= 0:
        parser.error("--stats-seconds must be positive")
    print(json.dumps(summarize(args.log, args.stats_seconds), indent=2))
