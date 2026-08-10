#!/usr/bin/env python3
"""Run reproducible firmware arpeggiator timing fixtures through console_host."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import arp_timing_analyze as timing
import audio_midi_correlate as audio_timing


ROOT = Path(__file__).resolve().parent.parent
STEP_BASES = (
    ("quarter", 0, 24),
    ("quarter_triplet", 1, 16),
    ("eighth", 2, 12),
    ("eighth_triplet", 3, 8),
    ("sixteenth", 4, 6),
    ("sixteenth_triplet", 5, 4),
)


@dataclass(frozen=True)
class Case:
    name: str
    step_base: int = 2
    expected_clocks_per_onset: int = 12
    speed: int = 128
    block: int = 512
    external_bpm: float | None = None
    fixture: str = "factory"
    octaves: int = 1
    latch: bool = False
    key_sync: bool = False
    note_off_time: float = 15.0
    analysis_end: float = 14.9
    input_notes: tuple[int, ...] = (60, 64, 67, 71)
    driver_note_start: float | None = None
    arp_off_time: float = 15.4
    run_seconds: int = 17
    stress: str = ""
    capture_audio: bool = False


def parameter_change(group: int, parameter: int, value: int) -> str:
    value &= 0x3FFF
    parameter &= 0x3FFF
    data = (0xF0, 0x42, 0x30, 0x41, 0x41, group & 0x7F,
            parameter & 0x7F, (parameter >> 7) & 0x7F,
            value & 0x7F, (value >> 7) & 0x7F, 0xF7)
    return " ".join(f"{byte:02x}" for byte in data)


def scheduled_fixture(case: Case) -> str:
    # Every case gets its own copy of NVRAM. Pattern-memory protection is cleared only
    # in that copy; UP is selected and edited through documented group-2 parameter 18.
    user = case.fixture != "factory"
    events: list[tuple[float, str]] = [
        (8.4, parameter_change(0, 171, 0)),       # Pattern Memory Protect OFF
        (8.8, parameter_change(0, 187, 1 if case.external_bpm else 0)),
        (9.1, f"b0 63 00 b0 62 01 b0 06 {5 if user else 0:02x}"),
    ]
    if case.capture_audio:
        # Verified clean-saw recipe from the DSP capture campaign: one standard saw,
        # filters THRU, effects dry, instant full Amp EG and zero release. Combined
        # with the factory 80% gate this gives a true silent gap before every onset.
        clean_params = [
            (154, 0), (4484, 0), (4485, 99), (4486, 99), (4487, 0),
            (4488, 0), (4490, 0), (4492, 0),
            (176, 2), (177, 0), (178, 0), (179, 0), (185, 0), (186, 0), (187, 0),
            (155, 0), (157, 0), (159, 0), (163, 0), (164, 0), (166, 0), (174, 0),
            (238, 99), (241, 0), (244, 0), (247, 0), (250, 0), (253, 0),
            (256, 0), (259, 0), (262, 0), (265, 0),
            (222, 0), (216, 0), (217, 0), (236, 0), (230, 0), (231, 0),
            (269, 0), (285, 0),
            (301, 99), (310, 99), (309, 0), (318, 0),
            (304, 0), (305, 0), (313, 0), (314, 0),
            (319, 99), (320, 0), (321, 99), (322, 0),
            (323, 99), (324, 0), (325, 99), (326, 0),
            (382, 64), (384, 0),
            (342, 0), (351, 0), (361, 0), (367, 0), (373, 0),
            (378, 0), (381, 0), (154, 0),
        ]
        for index, (parameter, value) in enumerate(clean_params):
            events.append((6.20 + index * 0.025, parameter_change(1, parameter, value)))
    if user:
        velocity_mode = 129 if case.fixture == "velocity" else 100
        gate_mode = 101 if case.fixture == "gate" else 80
        pattern_params = [
            (18, case.step_base), (19, 1), (20, 0), (21, 127),
            (22, velocity_mode), (24, 0), (25, gate_mode), (27, 0),
            (28, 2), (29, 0),
        ]
        offsets = [-25, 0, 25, 0] if case.fixture == "offset" else [0, 0, 0, 0]
        velocities = [32, 120, 32, 120] if case.fixture == "velocity" else [100] * 4
        gates = [100, 0, 50, 0] if case.fixture == "gate" else [80] * 4
        for index in range(4):
            first = 33 + index * 4
            pattern_params.extend(((first, offsets[index]), (first + 1, index + 1),
                                   (first + 2, velocities[index]), (first + 3, gates[index])))
        pattern_params.extend(((49, 0), (50, 13), (51, 100), (52, 0)))  # step 5 = LOOP
        for index, (parameter, value) in enumerate(pattern_params):
            events.append((9.4 + index * 0.045, parameter_change(2, parameter, value)))
        # Read the edited record back through the authoritative 0x69 path. The raw
        # MIDI artifact then proves signed fields and template values reached firmware.
        events.append((10.9, "f0 42 30 41 34 05 00 f7"))
    else:
        events.append((10.8, parameter_change(2, 18, case.step_base)))
    note_on = " ".join(f"90 {note:02x} 64" for note in case.input_notes)
    note_off = " ".join(f"80 {note:02x} 00" for note in case.input_notes)
    events.extend((
        (10.95, f"b0 63 00 b0 62 03 b0 06 {case.octaves - 1:02x}"),
        (11.05, f"b0 63 00 b0 62 04 b0 06 {0x7f if case.latch else 0:02x}"),
        (11.15, f"b0 63 00 b0 62 05 b0 06 {0x7f if case.key_sync else 0:02x}"),
        (11.3, "b0 63 00 b0 62 02 b0 06 7f"),   # arpeggiator ON
        (case.arp_off_time, "b0 63 00 b0 62 02 b0 06 00"),  # arpeggiator OFF
    ))
    if case.stress == "subdivision":
        step_values = (2, 5, 0, 4, 1, 3)
        for index in range(72):
            events.append((12.25 + index * 0.09,
                           parameter_change(2, 18, step_values[index % len(step_values)])))
    elif case.stress == "toggle":
        for index in range(32):
            enabled = 0x7F if index % 2 else 0
            events.append((12.30 + index * 0.20,
                           f"b0 63 00 b0 62 02 b0 06 {enabled:02x}"))
    if case.driver_note_start is None:
        events.extend(((12.0, note_on), (case.note_off_time, note_off)))
    if case.external_bpm is not None:
        # Drive external clock through the public host MIDI seam.  The old
        # KPROP_INJECT_MIDI_CLOCK environment variable was a research-driver
        # hook and is deliberately absent from the clean/public MAME core.
        period = 60.0 / (24.0 * case.external_bpm)
        clock_at = 11.5
        while clock_at <= 15.5 + 1.0e-9:
            events.append((clock_at, "f8"))
            clock_at += period
    events.sort(key=lambda item: item[0])
    return " ; ".join(f"{message}@{when:.6f}" for when, message in events)


def cases_for(suite: str) -> list[Case]:
    if suite == "subdivision":
        return [Case(name=name, step_base=value, expected_clocks_per_onset=clocks)
                for name, value, clocks in STEP_BASES]
    if suite == "speed":
        return [Case(name=f"speed_{speed:03d}", speed=speed) for speed in (32, 64, 128, 192, 224)]
    if suite == "block":
        return [Case(name=f"block_{block:04d}", block=block) for block in (32, 128, 512, 1024)]
    if suite == "external":
        return [Case(name="external_" + name, step_base=value,
                     expected_clocks_per_onset=clocks, external_bpm=120.0)
                for name, value, clocks in STEP_BASES]
    if suite == "pattern":
        return [
            Case(name="pattern_order", fixture="order"),
            Case(name="pattern_velocity", fixture="velocity"),
            Case(name="pattern_gate_rest", fixture="gate", expected_clocks_per_onset=24),
            Case(name="pattern_offset", fixture="offset"),
        ]
    if suite == "controls":
        return [
            Case(name="octaves_1", octaves=1),
            Case(name="octaves_2", octaves=2),
            Case(name="octaves_3", octaves=3),
            Case(name="octaves_4", octaves=4),
            Case(name="latch_off_release", note_off_time=13.0, analysis_end=15.3),
            Case(name="latch_on_release", latch=True, note_off_time=13.0, analysis_end=15.3),
        ]
    if suite == "keysync":
        return [
            Case(name="keysync_off_phase_a", key_sync=False, block=32, input_notes=(60,),
                 driver_note_start=12.000, note_off_time=14.5, analysis_end=14.4),
            Case(name="keysync_off_phase_b", key_sync=False, block=32, input_notes=(60,),
                 driver_note_start=12.040, note_off_time=14.5, analysis_end=14.4),
            Case(name="keysync_on_phase_a", key_sync=True, block=32, input_notes=(60,),
                 driver_note_start=12.000, note_off_time=14.5, analysis_end=14.4),
            Case(name="keysync_on_phase_b", key_sync=True, block=32, input_notes=(60,),
                 driver_note_start=12.040, note_off_time=14.5, analysis_end=14.4),
        ]
    if suite == "drift":
        return [Case(name="internal_clock_two_minutes", note_off_time=130.0,
                     analysis_end=129.9, arp_off_time=130.4, run_seconds=132)]
    if suite == "stress":
        common = dict(note_off_time=19.5, analysis_end=21.0,
                      arp_off_time=20.0, run_seconds=22)
        return [
            Case(name="stress_speed_sweep", stress="speed", **common),
            Case(name="stress_subdivision_changes", stress="subdivision", **common),
            Case(name="stress_arp_toggle", stress="toggle", **common),
        ]
    if suite == "audio":
        return [Case(name="audio_midi_onset", step_base=0, expected_clocks_per_onset=24,
                     input_notes=(60,), capture_audio=True)]
    raise ValueError(suite)


def key_sync_clock_metrics(case: Case, result: dict[str, object]) -> dict[str, object]:
    """Separate the intentional key-sync reset interval from steady clock timing."""
    clock_times = result.get("clock_times_s", [])
    assert isinstance(clock_times, list)
    intervals = [float(right) - float(left)
                 for left, right in zip(clock_times, clock_times[1:])]
    positive = [value for value in intervals if value > 0]
    median = statistics.median(positive) if positive else 0.0
    short = [index for index, value in enumerate(intervals)
             if 0 < value < median * 0.98]
    reset_index = short[0] if len(short) == 1 else None
    reset_end = (float(clock_times[reset_index + 1])
                 if reset_index is not None else None)
    reset_latency = (reset_end - float(case.driver_note_start)
                     if reset_end is not None and case.driver_note_start is not None
                     else None)
    reset_valid = (
        reset_index is not None
        and reset_latency is not None
        and 0.0 <= reset_latency < median * 2.0
    )
    stable = [value for index, value in enumerate(intervals)
              if index != reset_index]
    stable_positive = [value for value in stable if value > 0]
    stable_median = statistics.median(stable_positive) if stable_positive else 0.0
    stable_min = min(stable_positive, default=0.0)
    stable_max = max(stable_positive, default=0.0)
    stable_jitter = max((abs(value - stable_median) for value in stable_positive),
                        default=999.0)
    return {
        "reset_valid": reset_valid,
        "reset_count": len(short),
        "reset_interval_s": intervals[reset_index] if reset_index is not None else None,
        "reset_latency_s": reset_latency,
        "stable_count": len(stable_positive),
        "stable_median_s": stable_median,
        "stable_min_s": stable_min,
        "stable_max_s": stable_max,
        "stable_max_jitter_s": stable_jitter,
    }


def grade(case: Case, result: dict[str, object], console_log: str) -> tuple[list[str], list[str]]:
    passed: list[str] = []
    failed: list[str] = []
    clock = result["clock"]
    notes = result["note_on"]
    gate = result["gate"]
    assert isinstance(clock, dict) and isinstance(notes, dict) and isinstance(gate, dict)

    def check(condition: bool, label: str, detail: str) -> None:
        (passed if condition else failed).append(f"{label}: {detail}")

    drop_match = re.search(r"midi-tx-events=\d+ clocks=\d+ dropped=(\d+)", console_log)
    drops = int(drop_match.group(1)) if drop_match else -1
    check(drops == 0, "observer", f"dropped={drops}")
    scheduled_drop_match = re.search(r"scheduled-midi-dropped=(\d+)", console_log)
    scheduled_drops = int(scheduled_drop_match.group(1)) if scheduled_drop_match else -1
    check(scheduled_drops == 0, "scheduled-midi", f"dropped={scheduled_drops}")
    if case.stress:
        note_on_count = int(notes.get("count", 0))
        note_off_count = int(result.get("note_off_count", 0))
        unmatched = int(result.get("unmatched_note_ons", -1))
        onset_times = result.get("onset_times_s", [])
        assert isinstance(onset_times, list)
        minimum_notes = 3 if case.stress == "toggle" else 12
        check(note_on_count >= minimum_notes, "stress-produced-notes",
              f"note_ons={note_on_count} minimum={minimum_notes}")
        check(note_off_count >= note_on_count and unmatched == 0, "stress-no-stuck-notes",
              f"note_ons={note_on_count} note_offs={note_off_count} unmatched={unmatched}")
        late = [float(value) for value in onset_times if float(value) > case.arp_off_time + 0.2]
        check(not late, "stress-final-off-stops", f"late_onsets={late[:4]}")
        intervals = [float(value) for value in result.get("onset_intervals_s", [])]
        if case.stress == "speed":
            clock_times = result.get("clock_times_s", [])
            assert isinstance(clock_times, list)
            clock_intervals = [float(b) - float(a) for a, b in zip(clock_times, clock_times[1:])]
            positive = [value for value in clock_intervals if value > 0]
            ratio = max(positive) / min(positive) if positive else 0.0
            check(len(positive) >= 300 and ratio > 2.0, "stress-speed-changed",
                  f"clock_intervals={len(positive)} max/min={ratio:.3f}")
        elif case.stress == "subdivision":
            clock_interval = float(clock.get("fit_interval_s", 0.0))
            rounded_clock_counts = sorted({round(value / clock_interval)
                                           for value in intervals if clock_interval > 0})
            check(len(rounded_clock_counts) >= 3, "stress-subdivisions-changed",
                  f"observed_clock_counts={rounded_clock_counts}")
        elif case.stress == "toggle":
            clock_times = result.get("clock_times_s", [])
            assert isinstance(clock_times, list)
            gaps = [float(b) - float(a) for a, b in zip(clock_times, clock_times[1:])]
            off_gaps = [value for value in gaps if value > 0.050]
            check(int(clock.get("count", 0)) >= 200 and len(off_gaps) >= 12,
                  "stress-toggle-clock-bursts-resume",
                  f"ticks={clock.get('count', 0)} off_gaps={len(off_gaps)}")
        return passed, failed
    if case.external_bpm is None:
        check(int(clock.get("count", 0)) >= 100, "clock-present", f"ticks={clock.get('count', 0)}")
        if case.key_sync:
            reset = key_sync_clock_metrics(case, result)
            reset_interval = reset["reset_interval_s"]
            reset_latency = reset["reset_latency_s"]
            check(bool(reset["reset_valid"]), "clock-key-sync-reset",
                  f"count={reset['reset_count']} interval={reset_interval} "
                  f"latency={reset_latency}")
            median_clock = float(reset["stable_median_s"])
            min_clock = float(reset["stable_min_s"])
            max_clock = float(reset["stable_max_s"])
            max_jitter = float(reset["stable_max_jitter_s"])
        else:
            median_clock = float(clock.get("median_interval_s", 0.0))
            min_clock = float(clock.get("min_interval_s", 0.0))
            max_clock = float(clock.get("max_interval_s", 0.0))
            max_jitter = float(clock.get("max_abs_interval_jitter_s", 999.0))
        bounded = median_clock > 0 and min_clock > median_clock * 0.98 and max_clock < median_clock * 1.02
        check(bounded, "clock-continuity",
              f"min/median/max={min_clock:.9f}/{median_clock:.9f}/{max_clock:.9f}s")
        check(max_jitter < 0.000100, "clock-jitter", f"max={max_jitter * 1e6:.3f}us")
        clocks_per_onset = float(notes.get("clocks_per_onset", -1.0))
    else:
        # In EXTERNAL mode the Prophecy consumes F8 but, as documented, does not echo it.
        check(int(clock.get("count", 0)) == 0, "no-clock-echo", f"ticks={clock.get('count', 0)}")
        input_clock_interval = 60.0 / (24.0 * case.external_bpm)
        clocks_per_onset = float(notes.get("fit_interval_s", -1.0)) / input_clock_interval
        notes["external_input_bpm"] = case.external_bpm
        notes["external_clocks_per_onset"] = clocks_per_onset
    check(abs(clocks_per_onset - case.expected_clocks_per_onset) < 0.02,
          "subdivision", f"measured={clocks_per_onset:.6f} expected={case.expected_clocks_per_onset}")
    sequence = result["note_sequence"]
    velocities = result["velocity_sequence"]
    assert isinstance(sequence, list) and isinstance(velocities, list)
    checked_notes = min(len(sequence), 12)
    if case.fixture == "gate":
        pitch_cycle = [60, 67]
    else:
        pitch_cycle = [note + octave * 12
                       for octave in range(case.octaves) for note in case.input_notes]
    expected = (pitch_cycle * 12)[:checked_notes]
    check(checked_notes >= 4 and sequence[:checked_notes] == expected,
          "pitch-order", f"first{checked_notes}={sequence[:checked_notes]}")
    expected_velocity_cycle = [32, 120, 32, 120] if case.fixture == "velocity" else [100]
    expected_velocities = (expected_velocity_cycle * 24)[:checked_notes]
    check(len(velocities) >= 4 and velocities[:checked_notes] == expected_velocities,
          "velocity", f"first{checked_notes}={velocities[:checked_notes]}")

    gate_duration = float(gate.get("median_duration_s", 0.0))
    onset_interval = float(notes.get("median_interval_s", 0.0))
    gate_ratio = gate_duration / onset_interval if onset_interval > 0 else 0.0
    if case.fixture == "gate":
        gate_records = result.get("gates", [])
        assert isinstance(gate_records, list)
        clock_interval = float(clock.get("fit_interval_s", 0.0))
        measured_gate_clocks = [float(item["duration_s"]) / clock_interval
                                for item in gate_records[:6]] if clock_interval > 0 else []
        expected_gate_clocks = ([12.0, 6.0] * 3)[:len(measured_gate_clocks)]
        good_gates = len(measured_gate_clocks) >= 4 and all(
            abs(measured - expected) < 0.25
            for measured, expected in zip(measured_gate_clocks, expected_gate_clocks))
        check(good_gates, "step-gate-rest",
              f"measured_clocks={[round(value, 4) for value in measured_gate_clocks]}")
    elif case.external_bpm is None and case.fixture != "offset":
        # Factory UP is 80%. UART/message scheduling produces a repeatable ~79% measured
        # completion-to-completion ratio; hardware comparison will determine whether that
        # one-percent shortening is part of the instrument contract.
        check(0.77 < gate_ratio < 0.81, "factory-gate", f"ratio={gate_ratio:.6f}")
    elif case.external_bpm is not None:
        # Firmware driven by discrete F8 clocks rounds an 80% gate upward to a whole
        # clock: 24->20, 16->13, 12->10, 8->7, 6->5, 4->4. The shortest case measures
        # just below 1.0 because note-off and following note-on serialize back-to-back.
        expected_gate_ratio = math.ceil(0.80 * case.expected_clocks_per_onset) / case.expected_clocks_per_onset
        check(abs(gate_ratio - expected_gate_ratio) < 0.015, "external-gate-clock-quantized",
              f"ratio={gate_ratio:.6f} expected={expected_gate_ratio:.6f}")
        gate["expected_whole_clock_ratio"] = expected_gate_ratio

    if case.fixture == "offset":
        clock_interval = float(clock.get("fit_interval_s", 0.0))
        nominal_step = clock_interval * 12.0
        onset_intervals = result.get("onset_intervals_s", [])
        assert isinstance(onset_intervals, list)
        normalized = [float(value) / nominal_step for value in onset_intervals[:8]] if nominal_step > 0 else []
        # The authoritative readback contains -25,0,+25,0, yet MIDI OUT shows the
        # negative step at its nominal boundary while +25 delays normally. This is
        # deterministic ROM behavior in emulation, but remains a named hardware A/B item.
        expected_offsets = [1.0, 1.25, 0.75, 1.0] * 2
        good_offsets = len(normalized) == 8 and all(
            abs(measured - expected) < 0.02 for measured, expected in zip(normalized, expected_offsets))
        check(good_offsets, "negative-offset-boundary-clamp",
              f"normalized={[round(value, 5) for value in normalized]}")

    if case.fixture != "factory":
        dumps = result.get("arpeggio_pattern_dumps", [])
        assert isinstance(dumps, list)
        raw = dumps[-1].get("raw", []) if dumps else []
        assert isinstance(raw, list)
        expected_offsets = [-25, 0, 25, 0] if case.fixture == "offset" else [0, 0, 0, 0]
        stored_offsets = [(int(raw[32 + index * 4]) + 128) % 256 - 128
                          for index in range(4)] if len(raw) == 128 else []
        stored_tones = [int(raw[33 + index * 4]) for index in range(5)] if len(raw) == 128 else []
        check(stored_offsets == expected_offsets and stored_tones == [1, 2, 3, 4, 13],
              "pattern-readback", f"offsets={stored_offsets} tones={stored_tones}")
    check(int(result.get("note_off_count", 0)) >= 4, "note-offs",
          f"count={result.get('note_off_count', 0)}")
    if case.note_off_time < 14.0:
        onset_times = result.get("onset_times_s", [])
        assert isinstance(onset_times, list)
        late_onsets = [float(value) for value in onset_times if float(value) > 14.0]
        if case.latch:
            check(len(late_onsets) >= 4, "latch-continues", f"late_onsets={len(late_onsets)}")
        else:
            check(not late_onsets, "release-stops-unlatched", f"late_onsets={late_onsets[:4]}")
    if case.run_seconds >= 60 and case.external_bpm is None:
        span = float(clock.get("span_s", 0.0))
        drift_ppm = float(clock.get("first_to_last_window_drift_ppm", 999.0))
        endpoint_error = float(clock.get("endpoint_phase_error_from_first_window_s", 999.0))
        phase_residual = float(clock.get("max_abs_phase_residual_s", 999.0))
        check(span >= 110.0, "long-clock-span", f"span={span:.6f}s ticks={clock.get('count', 0)}")
        check(abs(drift_ppm) < 1.0, "long-clock-frequency-drift",
              f"first-to-last={drift_ppm:.6f}ppm")
        check(abs(endpoint_error) < 0.000100, "long-clock-accumulated-phase",
              f"endpoint_error={endpoint_error * 1e6:.3f}us")
        check(phase_residual < 0.000100, "long-clock-bounded-phase",
              f"max_residual={phase_residual * 1e6:.3f}us")
    if case.capture_audio:
        correlation = result.get("audio_correlation", {})
        assert isinstance(correlation, dict)
        checks = correlation.get("checks", {})
        assert isinstance(checks, dict)
        for label, condition in checks.items():
            check(bool(condition), f"audio-{label}",
                  f"median_latency={float(correlation.get('median_latency_s', 0.0)) * 1000:.3f}ms "
                  f"span={float(correlation.get('latency_span_s', 0.0)) * 1000:.3f}ms")
    return passed, failed


def run_case(case: Case, output: Path, rompath: Path, nvram_seed: Path,
             console: Path) -> dict[str, object]:
    case_dir = output / case.name
    nvram_dir = case_dir / "nvram"
    (nvram_dir / "korgprop").mkdir(parents=True)
    shutil.copy2(nvram_seed, nvram_dir / "korgprop" / "sysram")
    capture = case_dir / "midi.jsonl"
    log_path = case_dir / "console.log"
    wav_path = case_dir / "audio.wav"

    environment = os.environ.copy()
    environment.pop("KPROP_INJECT_NOTE", None)
    environment.update({
        "SDL_VIDEODRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
        "KPROP_LIE_BATTERY_OK": "1",
        "PROPHOST_BLOCK": str(case.block),
        "PROPHOST_RING_FRAMES": "8192",
        "PROPHOST_ADIN_AT": f"10.0:0,{case.speed}",
        "PROPHOST_TEST_MIDI": scheduled_fixture(case),
        "PROPHOST_MIDI_TX_OUT": str(capture),
        "PROPHOST_WAV_OUT": str(wav_path) if case.capture_audio else "/dev/null",
    })
    if case.stress == "speed":
        speed_events = [f"{12.20 + index * 0.05:.3f}:0,{16 if index % 2 else 240}"
                        for index in range(128)]
        environment["PROPHOST_ADIN_AT"] += ";" + ";".join(speed_events)
    if case.driver_note_start is not None:
        environment["KPROP_INJECT_NOTE"] = (
            f"{case.driver_note_start}:{case.note_off_time}:{case.input_notes[0]}:100")
    command = (str(console), "korgprop", "-rompath", str(rompath),
               "-nvram_directory", str(nvram_dir), "-video", "none", "-sound", "none",
               "-nothrottle", "-skip_gameinfo", "-seconds_to_run", str(case.run_seconds))
    with log_path.open("w", encoding="utf-8") as log:
        completed = subprocess.run(command, cwd=ROOT, env=environment, stdout=log,
                                   stderr=subprocess.STDOUT, check=False)
    console_log = log_path.read_text(encoding="utf-8", errors="replace")
    if completed.returncode != 0:
        raise RuntimeError(f"{case.name}: console_host exited {completed.returncode}; see {log_path}")
    all_records = timing.load_records(capture, None, None)
    result = timing.analyze([(timestamp, byte) for timestamp, byte in all_records
                             if 12.0 <= timestamp <= case.analysis_end])
    result["arpeggio_pattern_dumps"] = timing.parse_arpeggio_pattern_dumps(all_records)
    if case.capture_audio:
        result["audio_correlation"] = audio_timing.analyze(wav_path, result)
    passed, failed = grade(case, result, console_log)
    record: dict[str, object] = {
        "case": case.__dict__,
        "result": result,
        "passed": passed,
        "failed": failed,
        "verdict": "PASS" if not failed else "FAIL",
        "artifacts": {"midi": str(capture), "console_log": str(log_path),
                      "audio": str(wav_path) if case.capture_audio else None},
    }
    (case_dir / "result.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                                           encoding="utf-8")
    return record


def cross_case_checks(suite: str, cases: list[Case], records: list[dict[str, object]]) -> tuple[list[str], list[str]]:
    passed: list[str] = []
    failed: list[str] = []

    def check(condition: bool, label: str, detail: str) -> None:
        (passed if condition else failed).append(f"{label}: {detail}")

    if suite == "speed":
        bpms = [float(record["result"]["clock"]["bpm_24ppqn"]) for record in records]
        check(all(right > left for left, right in zip(bpms, bpms[1:])),
              "speed-monotonic", f"bpms={[round(value, 6) for value in bpms]}")
    elif suite == "block":
        bpms = [float(record["result"]["clock"]["bpm_24ppqn"]) for record in records]
        divisions = [float(record["result"]["note_on"]["clocks_per_onset"]) for record in records]
        check(max(bpms) - min(bpms) < 0.001, "block-bpm-invariant",
              f"range={max(bpms) - min(bpms):.9f}")
        check(max(divisions) - min(divisions) < 0.001, "block-division-invariant",
              f"range={max(divisions) - min(divisions):.9f}")
    elif suite == "keysync":
        first = [float(record["result"]["onset_times_s"][0]) for record in records]
        starts = [float(case.driver_note_start) for case in cases]
        off_latency = [first[index] - starts[index] for index in (0, 1)]
        on_latency = [first[index] - starts[index] for index in (2, 3)]
        off_periods = [float(records[index]["result"]["note_on"]["fit_interval_s"])
                       for index in (0, 1)]
        off_period = sum(off_periods) / len(off_periods)
        onset_phase = abs(first[1] - first[0]) % off_period
        onset_phase_error = min(onset_phase, off_period - onset_phase)
        check(onset_phase_error < 0.001, "keysync-off-free-running-boundary",
              f"first_onsets={[round(first[index], 9) for index in (0, 1)]} "
              f"period={off_period:.9f} phase_error={onset_phase_error:.9f}")
        check(max(on_latency) - min(on_latency) < 0.001 and max(on_latency) < 0.005,
              "keysync-on-immediate-reset",
              f"latency_ms={[round(value * 1000, 6) for value in on_latency]}")
        expected_phase_delta = (starts[1] - starts[0]) % off_period
        observed_phase_delta = (off_latency[0] - off_latency[1]) % off_period
        phase_delta_error = abs(observed_phase_delta - expected_phase_delta)
        phase_delta_error = min(phase_delta_error, off_period - phase_delta_error)
        check(phase_delta_error < 0.001,
              "keysync-off-preserves-phase",
              f"latency_ms={[round(value * 1000, 6) for value in off_latency]} "
              f"period={off_period:.9f} phase_error={phase_delta_error:.9f}")
    return passed, failed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", choices=("subdivision", "speed", "block", "external", "pattern", "controls", "keysync", "drift", "stress", "audio"), default="subdivision")
    parser.add_argument("--case", help="run one generated case by exact name")
    parser.add_argument("--print-fixture", action="store_true",
                        help="print generated MIDI/external-clock schedules as JSON and exit")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--console", type=Path, default=ROOT / "console_host",
                        help="console host binary (default: ./console_host)")
    parser.add_argument("--rompath", type=Path, default=ROOT.parent / "mame" / "00-roms")
    parser.add_argument("--nvram-seed", type=Path,
                        default=ROOT.parent / "mame" / "nvram" / "korgprop" / "sysram")
    args = parser.parse_args()

    cases = cases_for(args.suite)
    if args.case:
        cases = [case for case in cases if case.name == args.case]
        if not cases:
            print(f"unknown case {args.case!r} for {args.suite}", file=sys.stderr)
            return 2
    if args.print_fixture:
        fixtures = [{
            "case": case.__dict__,
            "midi_schedule": scheduled_fixture(case),
            "external_clock": ({"start_s": 11.5, "end_s": 15.5,
                                "bpm": case.external_bpm, "ppqn": 24}
                               if case.external_bpm is not None else None),
        } for case in cases]
        print(json.dumps(fixtures, indent=2, sort_keys=True))
        return 0

    output = args.output or Path(tempfile.gettempdir()) / (
        "prophecy_arp_timing_" + dt.datetime.now().strftime("%Y%m%d_%H%M%S"))
    output.mkdir(parents=True, exist_ok=False)
    console = args.console.expanduser().resolve()
    if not console.is_file():
        print(f"console host is missing: {console}; run ./scripts/build_console.sh", file=sys.stderr)
        return 2
    if not args.rompath.is_dir() or not args.nvram_seed.is_file():
        print(f"missing ROM path or NVRAM seed: {args.rompath} / {args.nvram_seed}", file=sys.stderr)
        return 2

    records = []
    for index, case in enumerate(cases, 1):
        print(f"[{index}/{len(cases)}] {case.name}", flush=True)
        try:
            record = run_case(case, output, args.rompath, args.nvram_seed, console)
        except (OSError, RuntimeError, ValueError) as error:
            print(error, file=sys.stderr)
            return 2
        records.append(record)
        result = record["result"]
        assert isinstance(result, dict)
        clock = result["clock"]
        notes = result["note_on"]
        assert isinstance(clock, dict) and isinstance(notes, dict)
        measured_division = notes.get("external_clocks_per_onset", notes.get("clocks_per_onset", "n/a"))
        print(f"  {record['verdict']} BPM={clock.get('bpm_24ppqn', case.external_bpm or 'n/a')} "
              f"clocks/onset={measured_division}")
        for failure in record["failed"]:
            print(f"    FAIL {failure}")

    suite_passed, suite_failed = cross_case_checks(args.suite, cases, records)
    for item in suite_passed:
        print(f"  PASS {item}")
    for item in suite_failed:
        print(f"  FAIL {item}")
    case_fail_count = sum(record["verdict"] != "PASS" for record in records)
    summary = {
        "schema": 1,
        "suite": args.suite,
        "console": str(console),
        "output": str(output),
        "case_count": len(records),
        "pass_count": sum(record["verdict"] == "PASS" for record in records),
        "fail_count": case_fail_count + len(suite_failed),
        "suite_checks_passed": suite_passed,
        "suite_checks_failed": suite_failed,
        "cases": records,
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                                          encoding="utf-8")
    print(f"summary: {summary['pass_count']} PASS / {summary['fail_count']} FAIL  {output}")
    return 0 if summary["fail_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
