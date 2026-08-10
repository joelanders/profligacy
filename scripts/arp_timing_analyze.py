#!/usr/bin/env python3
"""Analyze timestamped Prophecy MIDI OUT bytes captured by console_host.

Input is PROPHOST_MIDI_TX_OUT JSONL: one {"t": seconds, "byte": 0..255}
record per successfully decoded 31.25-kbaud UART byte. Realtime bytes may appear
between channel-message bytes and do not disturb running status.
"""

from __future__ import annotations

import argparse
import bisect
import json
import math
import statistics
import sys
from collections import defaultdict, deque
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def series_stats(times: list[float]) -> dict[str, float | int | None]:
    intervals = [b - a for a, b in zip(times, times[1:])]
    if not intervals:
        return {"count": len(times), "interval_count": 0}
    mean = statistics.fmean(intervals)
    median = statistics.median(intervals)
    deviations = [abs(value - median) for value in intervals]

    # Least-squares phase ruler. Residuals expose bounded jitter independently
    # of the absolute start phase and the best measured long-term frequency.
    def linear_fit(values: list[float], index_offset: int = 0) -> tuple[float, float]:
        count = len(values)
        x_mean = (count - 1) / 2.0
        y_mean = statistics.fmean(values)
        denominator = sum((index - x_mean) ** 2 for index in range(count))
        fit_slope = (sum((index - x_mean) * (value - y_mean)
                         for index, value in enumerate(values)) / denominator) if denominator else 0.0
        # Express the intercept against the original series index even for a window.
        fit_intercept = y_mean - fit_slope * (x_mean + index_offset)
        return fit_slope, fit_intercept

    n = len(times)
    slope, intercept = linear_fit(times)
    residuals = [value - (intercept + index * slope) for index, value in enumerate(times)]
    abs_residuals = [abs(value) for value in residuals]
    window_count = min(1000, n // 4)
    first_window_interval = None
    last_window_interval = None
    drift_ppm = None
    endpoint_phase_error = None
    if window_count >= 2:
        first_window_interval, first_window_intercept = linear_fit(times[:window_count])
        last_window_interval, _ = linear_fit(times[-window_count:], n - window_count)
        if first_window_interval > 0:
            drift_ppm = ((last_window_interval / first_window_interval) - 1.0) * 1_000_000.0
            endpoint_phase_error = times[-1] - (
                first_window_intercept + (n - 1) * first_window_interval)
    return {
        "count": len(times),
        "interval_count": len(intervals),
        "mean_interval_s": mean,
        "median_interval_s": median,
        "min_interval_s": min(intervals),
        "max_interval_s": max(intervals),
        "p95_abs_interval_jitter_s": percentile(deviations, 0.95),
        "max_abs_interval_jitter_s": max(deviations),
        "fit_interval_s": slope,
        "p95_abs_phase_residual_s": percentile(abs_residuals, 0.95),
        "max_abs_phase_residual_s": max(abs_residuals),
        "comparison_window_count": window_count,
        "first_window_interval_s": first_window_interval,
        "last_window_interval_s": last_window_interval,
        "first_to_last_window_drift_ppm": drift_ppm,
        "endpoint_phase_error_from_first_window_s": endpoint_phase_error,
        "span_s": times[-1] - times[0],
    }


def load_records(path: Path, start: float | None, end: float | None) -> list[tuple[float, int]]:
    records: list[tuple[float, int]] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, 1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
                timestamp = float(item["t"])
                byte = int(item["byte"])
            except (ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
                raise ValueError(f"{path}:{line_number}: invalid MIDI event: {error}") from error
            if not 0 <= byte <= 255:
                raise ValueError(f"{path}:{line_number}: byte outside 0..255")
            if (start is None or timestamp >= start) and (end is None or timestamp <= end):
                records.append((timestamp, byte))
    return records


def parse_channel_messages(records: list[tuple[float, int]]) -> list[dict[str, int | float]]:
    messages: list[dict[str, int | float]] = []
    running_status: int | None = None
    status: int | None = None
    needed = 0
    data: list[int] = []
    in_sysex = False

    for timestamp, byte in records:
        if byte >= 0xF8:  # Realtime may be interleaved anywhere.
            continue
        if in_sysex:
            if byte == 0xF7:
                in_sysex = False
            continue
        if byte == 0xF0:
            in_sysex = True
            running_status = None
            status = None
            data.clear()
            continue
        if byte & 0x80:
            data.clear()
            if 0x80 <= byte <= 0xEF:
                status = byte
                running_status = byte
                needed = 1 if (byte & 0xE0) == 0xC0 else 2
            else:
                running_status = None
                status = None
                needed = 0
            continue

        if status is None:
            status = running_status
            if status is None:
                continue
            needed = 1 if (status & 0xE0) == 0xC0 else 2
        data.append(byte)
        if len(data) == needed:
            message = {"t": timestamp, "status": status, "channel": status & 0x0F,
                       "data1": data[0]}
            if needed == 2:
                message["data2"] = data[1]
            messages.append(message)
            data.clear()
            status = running_status
    return messages


def parse_arpeggio_pattern_dumps(records: list[tuple[float, int]]) -> list[dict[str, object]]:
    dumps: list[dict[str, object]] = []
    message: list[int] = []
    start = 0.0
    for timestamp, byte in records:
        if byte >= 0xF8:
            continue
        if byte == 0xF0:
            message = [byte]
            start = timestamp
            continue
        if not message:
            continue
        message.append(byte)
        if byte != 0xF7:
            continue
        if len(message) >= 9 and message[4] == 0x69:
            packed = message[7:-1]
            raw: list[int] = []
            position = 0
            while position < len(packed):
                high = packed[position]
                position += 1
                for bit in range(7):
                    if position >= len(packed):
                        break
                    raw.append((packed[position] & 0x7F) | (((high >> bit) & 1) << 7))
                    position += 1
            dumps.append({"start_s": start, "end_s": timestamp,
                          "pattern": message[5] & 0x1F, "raw": raw})
        message = []
    return dumps


def analyze(records: list[tuple[float, int]]) -> dict[str, object]:
    clocks = [timestamp for timestamp, byte in records if byte == 0xF8]
    clock_stats = series_stats(clocks)
    clock_interval = clock_stats.get("fit_interval_s")
    if isinstance(clock_interval, float) and clock_interval > 0:
        clock_stats["bpm_24ppqn"] = 60.0 / (24.0 * clock_interval)

    messages = parse_channel_messages(records)
    note_ons: list[dict[str, int | float]] = []
    note_offs: list[dict[str, int | float]] = []
    pending: dict[tuple[int, int], deque[dict[str, int | float]]] = defaultdict(deque)
    gates: list[dict[str, int | float]] = []
    for message in messages:
        kind = int(message["status"]) & 0xF0
        velocity = int(message.get("data2", 0))
        key = (int(message["channel"]), int(message["data1"]))
        if kind == 0x90 and velocity > 0:
            note_ons.append(message)
            pending[key].append(message)
        elif kind == 0x80 or (kind == 0x90 and velocity == 0):
            note_offs.append(message)
            if pending[key]:
                on = pending[key].popleft()
                gates.append({"channel": key[0], "note": key[1], "on_s": on["t"],
                              "off_s": message["t"], "duration_s": float(message["t"]) - float(on["t"])})

    onset_times = [float(message["t"]) for message in note_ons]
    onset_stats = series_stats(onset_times)
    gate_durations = [float(gate["duration_s"]) for gate in gates]
    gate_stats: dict[str, float | int | None] = {"count": len(gate_durations)}
    if gate_durations:
        gate_stats.update({
            "mean_duration_s": statistics.fmean(gate_durations),
            "median_duration_s": statistics.median(gate_durations),
            "min_duration_s": min(gate_durations),
            "max_duration_s": max(gate_durations),
        })

    if clocks and onset_times and isinstance(clock_interval, float) and clock_interval > 0:
        phases = []
        for onset in onset_times:
            index = bisect.bisect_left(clocks, onset)
            candidates = clocks[max(0, index - 1):min(len(clocks), index + 1)]
            if candidates:
                phases.append(min(abs(onset - clock) for clock in candidates))
        if phases:
            onset_stats["median_nearest_clock_distance_s"] = statistics.median(phases)
            onset_stats["max_nearest_clock_distance_s"] = max(phases)
        onset_interval = onset_stats.get("fit_interval_s")
        if isinstance(onset_interval, float):
            onset_stats["clocks_per_onset"] = onset_interval / clock_interval
        if gate_durations:
            gate_stats["median_gate_clocks"] = statistics.median(gate_durations) / clock_interval

    return {
        "record_count": len(records),
        "clock": clock_stats,
        "clock_times_s": clocks,
        "channel_message_count": len(messages),
        "note_on": onset_stats,
        "note_off_count": len(note_offs),
        "gate": gate_stats,
        "onset_times_s": onset_times,
        "onset_intervals_s": [b - a for a, b in zip(onset_times, onset_times[1:])],
        "gates": gates,
        "note_sequence": [int(message["data1"]) for message in note_ons],
        "velocity_sequence": [int(message.get("data2", 0)) for message in note_ons],
        "unmatched_note_ons": sum(len(queue) for queue in pending.values()),
        "arpeggio_pattern_dumps": parse_arpeggio_pattern_dumps(records),
    }


def milliseconds(value: object) -> str:
    return f"{float(value) * 1000.0:.6f}" if isinstance(value, (float, int)) else "n/a"


def print_report(result: dict[str, object]) -> None:
    clock = result["clock"]
    note = result["note_on"]
    gate = result["gate"]
    assert isinstance(clock, dict) and isinstance(note, dict) and isinstance(gate, dict)
    print(f"records: {result['record_count']}  channel messages: {result['channel_message_count']}")
    print(f"clock: {clock.get('count', 0)} ticks  BPM={clock.get('bpm_24ppqn', 'n/a')}")
    if clock.get("interval_count", 0):
        print("  interval ms: mean={} median={} min={} max={}".format(
            milliseconds(clock.get("mean_interval_s")), milliseconds(clock.get("median_interval_s")),
            milliseconds(clock.get("min_interval_s")), milliseconds(clock.get("max_interval_s"))))
        print("  jitter ms: p95={} max={}  phase residual ms: p95={} max={}".format(
            milliseconds(clock.get("p95_abs_interval_jitter_s")),
            milliseconds(clock.get("max_abs_interval_jitter_s")),
            milliseconds(clock.get("p95_abs_phase_residual_s")),
            milliseconds(clock.get("max_abs_phase_residual_s"))))
    print(f"notes: {note.get('count', 0)} on / {result['note_off_count']} off  unmatched={result['unmatched_note_ons']}")
    if note.get("interval_count", 0):
        print("  onset interval ms: median={}  clocks/onset={}".format(
            milliseconds(note.get("median_interval_s")), note.get("clocks_per_onset", "n/a")))
    if gate.get("count", 0):
        print("  gate ms: median={} min={} max={}  median clocks={}".format(
            milliseconds(gate.get("median_duration_s")), milliseconds(gate.get("min_duration_s")),
            milliseconds(gate.get("max_duration_s")), gate.get("median_gate_clocks", "n/a")))
    sequence = result["note_sequence"]
    velocities = result["velocity_sequence"]
    assert isinstance(sequence, list) and isinstance(velocities, list)
    print("  first notes:", " ".join(str(value) for value in sequence[:24]) or "(none)")
    print("  first velocities:", " ".join(str(value) for value in velocities[:24]) or "(none)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("--start", type=float)
    parser.add_argument("--end", type=float)
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()
    try:
        result = analyze(load_records(args.capture, args.start, args.end))
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 2
    print_report(result)
    if args.json_out:
        args.json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
