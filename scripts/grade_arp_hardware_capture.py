#!/usr/bin/env python3
"""Grade common-clock physical Prophecy arpeggiator captures.

The input manifest names raw MIDI IN/OUT JSONL files. Each line is one UART
byte-stop event as ``{"t": seconds, "byte": 0..255}``, the same representation
used by ``arp_timing_analyze.py``. Paths are relative to the manifest.

This grader deliberately distinguishes FAIL from CAPTURED-UNGRADABLE. Missing
raw SPEED ADC evidence, excessive interface loopback jitter, or insufficient
events never become an accidental PASS.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import statistics
import sys
from pathlib import Path
from typing import Any

import arp_timing_analyze as timing
import audio_midi_correlate as audio_timing
import run_arp_timing_suite as suite


SPEED_REFERENCES = {32: 123.451, 64: 187.126, 128: 311.876,
                    192: 436.627, 224: 499.002}
VALID_KINDS = {"loopback", "internal", "speed", "external", "offset",
               "keysync", "audio"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def resolve(base: Path, value: object) -> Path | None:
    if value in (None, ""):
        return None
    path = Path(str(value))
    return path if path.is_absolute() else base / path


def load_capture(base: Path, take: dict[str, Any], field: str) -> list[tuple[float, int]]:
    path = resolve(base, take.get(field))
    if path is None:
        return []
    records = timing.load_records(path, None, None)
    times = [record[0] for record in records]
    if times != sorted(times):
        raise ValueError(f"{path}: timestamps are not monotonic")
    start = take.get("start_s")
    end = take.get("end_s")
    return [(timestamp, byte) for timestamp, byte in records
            if (start is None or timestamp >= float(start)) and
            (end is None or timestamp <= float(end))]


def fit_interval(times: list[float]) -> float | None:
    value = timing.series_stats(times).get("fit_interval_s")
    return float(value) if isinstance(value, (float, int)) and value > 0 else None


def channel_note_ons(records: list[tuple[float, int]]) -> list[dict[str, int | float]]:
    return [message for message in timing.parse_channel_messages(records)
            if (int(message["status"]) & 0xF0) == 0x90 and int(message.get("data2", 0)) > 0]


def add_check(result: dict[str, Any], state: str, label: str, detail: object) -> None:
    result["checks"].append({"state": state, "label": label, "detail": detail})


def require(result: dict[str, Any], condition: bool, label: str, detail: object) -> None:
    add_check(result, "PASS" if condition else "FAIL", label, detail)


def ungradable(result: dict[str, Any], label: str, detail: object) -> None:
    add_check(result, "CAPTURED-UNGRADABLE", label, detail)


def check_cycle(result: dict[str, Any], actual: list[int], expected: list[int],
                label: str, minimum: int) -> None:
    count = min(len(actual), max(minimum, len(expected) * 2))
    wanted = (expected * (count // len(expected) + 1))[:count] if expected else []
    require(result, len(actual) >= minimum and count >= max(minimum, len(expected)) and
            actual[:count] == wanted, label,
            {"actual": actual[:count], "expected": wanted})


def grade_loopback(result: dict[str, Any], midi_in: list[tuple[float, int]],
                   midi_out: list[tuple[float, int]]) -> None:
    count = min(len(midi_in), len(midi_out))
    require(result, count >= 200, "loopback-event-count", count)
    if count == 0:
        return
    input_bytes = [byte for _, byte in midi_in[:count]]
    output_bytes = [byte for _, byte in midi_out[:count]]
    require(result, input_bytes == output_bytes and len(midi_in) == len(midi_out),
            "loopback-byte-identity",
            {"input": len(midi_in), "output": len(midi_out),
             "first_mismatch": next((index for index, pair in enumerate(zip(input_bytes, output_bytes))
                                     if pair[0] != pair[1]), None)})
    latencies = [midi_out[index][0] - midi_in[index][0] for index in range(count)]
    median = statistics.median(latencies)
    errors = [abs(value - median) for value in latencies]
    p95 = timing.percentile(errors, 0.95)
    maximum = max(errors)
    result["loopback"] = {"count": count, "median_latency_s": median,
                          "p95_abs_error_s": p95, "max_abs_error_s": maximum}
    require(result, p95 is not None and p95 <= 0.0005, "loopback-p95",
            {"p95_ms": None if p95 is None else p95 * 1000,
             "max_ms": maximum * 1000})


def grade_rhythm(result: dict[str, Any], take: dict[str, Any],
                 midi_in: list[tuple[float, int]], midi_out: list[tuple[float, int]]) -> None:
    analyzed = timing.analyze(midi_out)
    result["midi_out"] = analyzed
    notes = analyzed["note_on"]
    gates = analyzed["gate"]
    assert isinstance(notes, dict) and isinstance(gates, dict)
    expected = int(take.get("expected_clocks_per_onset", 12))
    kind = str(take["kind"])
    minimum_onsets = int(take.get("minimum_note_onsets", 8))

    if kind == "external":
        input_clocks = [timestamp for timestamp, byte in midi_in if byte == 0xF8]
        clock_interval = fit_interval(input_clocks)
        require(result, len(input_clocks) >= 100, "input-clock-count", len(input_clocks))
        require(result, int(analyzed["clock"].get("count", 0)) == 0,
                "no-output-clock-echo", analyzed["clock"].get("count", 0))
    else:
        output_clocks = [timestamp for timestamp, byte in midi_out if byte == 0xF8]
        clock_interval = fit_interval(output_clocks)
        minimum = 100 if kind in {"internal", "speed"} else 12
        require(result, len(output_clocks) >= minimum, "output-clock-count", len(output_clocks))

    onset_interval = notes.get("fit_interval_s")
    clocks_per_onset = (float(onset_interval) / clock_interval
                        if isinstance(onset_interval, (float, int)) and clock_interval else None)
    result["clocks_per_onset"] = clocks_per_onset
    require(result, clocks_per_onset is not None and abs(clocks_per_onset - expected) < 0.02,
            "subdivision", {"measured": clocks_per_onset, "expected": expected})

    expected_notes = [int(value) for value in take.get("expected_note_cycle", [60, 64, 67, 71])]
    expected_velocities = [int(value) for value in take.get("expected_velocity_cycle", [100])]
    check_cycle(result, [int(value) for value in analyzed["note_sequence"]],
                expected_notes, "note-cycle", minimum_onsets)
    check_cycle(result, [int(value) for value in analyzed["velocity_sequence"]],
                expected_velocities, "velocity-cycle", minimum_onsets)
    # A common-clock capture may stop in the middle of one ordinary gate. More than
    # one unmatched onset, or more than one missing note-off, is still a hard failure.
    require(result, int(analyzed["unmatched_note_ons"]) <= 1 and
            int(analyzed["note_off_count"]) >= int(notes.get("count", 0)) - 1,
            "balanced-note-offs",
            {"on": notes.get("count", 0), "off": analyzed["note_off_count"],
             "unmatched": analyzed["unmatched_note_ons"]})

    if kind == "external" and clock_interval:
        durations = [float(item["duration_s"]) / clock_interval for item in analyzed["gates"]]
        predicted = math.ceil(0.8 * expected)
        median_gate = statistics.median(durations) if durations else None
        minimum_gates = int(take.get("minimum_gates", max(1, minimum_onsets - 1)))
        result["external_gate_clocks"] = durations
        require(result, len(durations) >= minimum_gates and median_gate is not None and
                abs(median_gate - predicted) <= 0.10,
                "whole-clock-gate-rounding",
                {"median": median_gate, "predicted": predicted,
                 "first": durations[:12]})
        rounded = [round(value) for value in durations]
        require(result, bool(rounded) and len(set(rounded)) == 1 and rounded[0] == predicted,
                "gate-does-not-alternate", rounded[:24])

    if kind == "speed":
        clock = analyzed["clock"]
        assert isinstance(clock, dict)
        result["bpm"] = clock.get("bpm_24ppqn")
        raw_code = take.get("speed_adc_code")
        if raw_code is None:
            ungradable(result, "speed-hardware-equivalence",
                       "no raw ADC code; knob-position curve is approximate only")
        else:
            nearest = min(SPEED_REFERENCES, key=lambda value: abs(value - float(raw_code)))
            measured_bpm = float(clock.get("bpm_24ppqn", 0.0))
            result["speed_reference"] = {"actual_adc": float(raw_code),
                                         "nearest_reference_adc": nearest,
                                         "reference_bpm": SPEED_REFERENCES[nearest],
                                         "bpm_error": measured_bpm - SPEED_REFERENCES[nearest]}
            if abs(float(raw_code) - nearest) > 0.5:
                ungradable(result, "speed-hardware-equivalence",
                           f"ADC {raw_code} is not the measured reference code {nearest}")
            else:
                tolerance_percent = float(take.get("speed_bpm_tolerance_percent", 0.5))
                tolerance_bpm = SPEED_REFERENCES[nearest] * tolerance_percent / 100.0
                require(result, abs(measured_bpm - SPEED_REFERENCES[nearest]) <= tolerance_bpm,
                        "speed-reference-bpm",
                        {"measured": measured_bpm, "reference": SPEED_REFERENCES[nearest],
                         "adc": raw_code, "tolerance_percent": tolerance_percent,
                         "tolerance_bpm": tolerance_bpm})


def grade_offset(result: dict[str, Any], take: dict[str, Any],
                 midi_out: list[tuple[float, int]]) -> None:
    analyzed = timing.analyze(midi_out)
    result["midi_out"] = analyzed
    clock_interval = fit_interval([timestamp for timestamp, byte in midi_out if byte == 0xF8])
    intervals = [float(value) for value in analyzed["onset_intervals_s"]]
    normalized = [value / (12.0 * clock_interval) for value in intervals] if clock_interval else []
    expected = [1.0, 1.25, 0.75, 1.0]
    phase_values = [normalized[index::4] for index in range(4)]
    phase_means = [statistics.fmean(values) if values else None for values in phase_values]
    result["normalized_intervals"] = normalized
    result["phase_means"] = phase_means
    require(result, all(len(values) >= 3 for values in phase_values),
            "offset-three-loops", [len(values) for values in phase_values])
    require(result, all(value is not None and abs(value - wanted) < 0.02
                        for value, wanted in zip(phase_means, expected)),
            "negative-offset-boundary-clamp",
            {"phase_means": phase_means, "expected": expected})

    dumps = timing.parse_arpeggio_pattern_dumps(midi_out)
    raw = dumps[-1]["raw"] if dumps else []
    offsets = [((int(raw[32 + index * 4]) + 128) % 256) - 128
               for index in range(4)] if len(raw) == 128 else []
    tones = [int(raw[33 + index * 4]) for index in range(5)] if len(raw) == 128 else []
    require(result, offsets == [-25, 0, 25, 0] and tones == [1, 2, 3, 4, 13],
            "authoritative-pattern-readback", {"offsets": offsets, "tones": tones})


def grade_keysync_single(result: dict[str, Any], take: dict[str, Any],
                         midi_in: list[tuple[float, int]],
                         midi_out: list[tuple[float, int]]) -> None:
    inputs = channel_note_ons(midi_in)
    outputs = channel_note_ons(midi_out)
    require(result, len(inputs) == 1, "one-input-note", len(inputs))
    require(result, len(outputs) >= 1, "output-note-present", len(outputs))
    if inputs and outputs:
        input_time = float(inputs[0]["t"])
        output_time = float(outputs[0]["t"])
        result["input_note_s"] = input_time
        result["first_output_note_s"] = output_time
        result["latency_s"] = output_time - input_time


def grade_audio(result: dict[str, Any], take: dict[str, Any],
                base: Path, midi_out: list[tuple[float, int]]) -> None:
    analyzed = timing.analyze(midi_out)
    result["midi_out"] = analyzed
    wav = resolve(base, take.get("wav"))
    if wav is None:
        ungradable(result, "audio-correlation", "WAV path missing")
        return
    wav_origin = float(take.get("wav_time_origin_s", 0.0))
    audio_ruler = dict(analyzed)
    audio_ruler["onset_times_s"] = [float(value) - wav_origin
                                     for value in analyzed["onset_times_s"]]
    correlation = audio_timing.analyze(wav, audio_ruler)
    correlation["wav_time_origin_s"] = wav_origin
    result["audio_correlation"] = correlation
    matched = len(correlation["matched_audio_onsets_s"])
    # Hardware absolute delay includes the physical DAC/reconstruction path. Grade
    # repeated timing, not equality to the emulator's 5.703 ms median.
    require(result, matched >= 15, "audio-matched-onsets", matched)
    require(result, float(correlation["latency_span_s"]) < 0.002,
            "audio-latency-spread", correlation["latency_span_s"])
    require(result, abs(float(correlation["fit_interval_delta_s"])) < 0.000050,
            "audio-fitted-period", correlation["fit_interval_delta_s"])
    require(result, float(correlation["max_abs_individual_interval_delta_s"]) < 0.001,
            "audio-individual-periods", correlation["max_abs_individual_interval_delta_s"])


def finalize(result: dict[str, Any]) -> None:
    states = [item["state"] for item in result["checks"]]
    if "FAIL" in states:
        result["verdict"] = "FAIL"
    elif "CAPTURED-UNGRADABLE" in states:
        result["verdict"] = "CAPTURED-UNGRADABLE"
    else:
        result["verdict"] = "PASS"


def grade_take(base: Path, take: dict[str, Any]) -> dict[str, Any]:
    name = str(take.get("name", "unnamed"))
    kind = str(take.get("kind", ""))
    if kind not in VALID_KINDS:
        raise ValueError(f"{name}: unknown kind {kind!r}")
    result: dict[str, Any] = {"name": name, "kind": kind, "checks": []}
    midi_in = load_capture(base, take, "midi_in")
    midi_out = load_capture(base, take, "midi_out")
    result["record_counts"] = {"midi_in": len(midi_in), "midi_out": len(midi_out)}
    if kind == "loopback":
        grade_loopback(result, midi_in, midi_out)
    elif kind in {"internal", "speed", "external"}:
        grade_rhythm(result, take, midi_in, midi_out)
    elif kind == "offset":
        grade_offset(result, take, midi_out)
    elif kind == "keysync":
        grade_keysync_single(result, take, midi_in, midi_out)
    elif kind == "audio":
        grade_audio(result, take, base, midi_out)
    finalize(result)
    return result


def cross_checks(takes: list[dict[str, Any]], results: list[dict[str, Any]]) -> list[dict[str, Any]]:
    checks: list[dict[str, Any]] = []

    speed_groups: dict[float, list[float]] = {}
    for take, result in zip(takes, results):
        if take.get("kind") == "speed" and isinstance(result.get("bpm"), (int, float)):
            speed_groups.setdefault(float(take.get("speed_position", 0.0)), []).append(float(result["bpm"]))
    if len(speed_groups) >= 2:
        pooled = [(position, statistics.median(values), values)
                  for position, values in sorted(speed_groups.items())]
        bpms = [item[1] for item in pooled]
        checks.append({"state": "PASS" if all(b > a for a, b in zip(bpms, bpms[1:])) else "FAIL",
                       "label": "speed-curve-monotonic",
                       "detail": [{"position": item[0], "median_bpm": item[1],
                                   "takes": item[2]} for item in pooled]})

    keys = [(take, result) for take, result in zip(takes, results)
            if take.get("kind") == "keysync" and isinstance(result.get("latency_s"), (int, float))]
    key_repeats = sorted({int(take.get("repeat", 1)) for take, _ in keys})
    for repeat in key_repeats:
        group = [(take, result) for take, result in keys if int(take.get("repeat", 1)) == repeat]
        by_sync: dict[bool, list[tuple[dict[str, Any], dict[str, Any]]]] = {False: [], True: []}
        for pair in group:
            by_sync[bool(pair[0].get("key_sync"))].append(pair)
        if len(by_sync[False]) < 2 or len(by_sync[True]) < 2:
            continue
        off = sorted(by_sync[False], key=lambda pair: str(pair[0].get("phase", "")))[:2]
        on = sorted(by_sync[True], key=lambda pair: str(pair[0].get("phase", "")))[:2]
        off_outputs = [float(result["first_output_note_s"]) for _, result in off]
        off_inputs = [float(result["input_note_s"]) for _, result in off]
        off_latencies = [float(result["latency_s"]) for _, result in off]
        on_inputs = [float(result["input_note_s"]) for _, result in on]
        on_latencies = [float(result["latency_s"]) for _, result in on]
        checks.extend([
            {"state": "PASS" if abs((off_inputs[1] - off_inputs[0]) - 0.040) < 0.0001 and
                                abs((on_inputs[1] - on_inputs[0]) - 0.040) < 0.0001 else "FAIL",
             "label": f"keysync-phase-displacement-take-{repeat}",
             "detail": {"off_s": off_inputs[1] - off_inputs[0],
                        "on_s": on_inputs[1] - on_inputs[0]}},
            {"state": "PASS" if abs(off_outputs[1] - off_outputs[0]) < 0.001 else "FAIL",
             "label": f"keysync-off-same-free-running-boundary-take-{repeat}",
             "detail": off_outputs},
            {"state": "PASS" if abs((off_latencies[0] - off_latencies[1]) -
                                     (off_inputs[1] - off_inputs[0])) < 0.001 else "FAIL",
             "label": f"keysync-off-preserves-input-phase-take-{repeat}",
             "detail": off_latencies},
            {"state": "PASS" if max(on_latencies) - min(on_latencies) < 0.001 else "FAIL",
             "label": f"keysync-on-latencies-cluster-take-{repeat}",
             "detail": on_latencies},
        ])
    return checks


def validate_manifest(document: dict[str, Any], base: Path) -> None:
    hashes = document.get("fixture_sha256", {})
    if hashes and not isinstance(hashes, dict):
        raise ValueError("fixture_sha256 must be an object")
    fixture_cases: dict[str, set[str]] = {}
    for take in document["takes"]:
        fixture_file = take.get("fixture_file")
        fixture_case = take.get("fixture_case")
        if fixture_file is None and fixture_case is None:
            continue
        if not isinstance(fixture_file, str) or not isinstance(fixture_case, str):
            raise ValueError(f"{take.get('name', 'unnamed')}: fixture_file and fixture_case must both be strings")
        path = resolve(base, fixture_file)
        assert path is not None
        if fixture_file not in fixture_cases:
            try:
                fixtures = json.loads(path.read_text(encoding="utf-8"))
                fixture_cases[fixture_file] = {str(item["case"]["name"]) for item in fixtures}
            except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
                raise ValueError(f"cannot read fixture {path}: {error}") from error
            expected_hash = hashes.get(fixture_file) if isinstance(hashes, dict) else None
            if expected_hash is None:
                raise ValueError(f"{fixture_file}: missing immutable fixture SHA-256")
            actual_hash = sha256(path)
            if actual_hash != expected_hash:
                raise ValueError(f"{fixture_file}: SHA-256 changed ({actual_hash} != {expected_hash})")
        if fixture_case not in fixture_cases[fixture_file]:
            raise ValueError(f"{fixture_file}: no fixture case {fixture_case!r}")


def campaign_checks(document: dict[str, Any], base: Path) -> list[dict[str, Any]]:
    if not document.get("require_campaign_metadata", False):
        return []
    metadata = document.get("metadata", {})
    if not isinstance(metadata, dict):
        return [{"state": "CAPTURED-UNGRADABLE", "label": "campaign-metadata",
                 "detail": "metadata must be an object"}]
    checks: list[dict[str, Any]] = []

    def evidence(condition: bool, label: str, detail: object) -> None:
        checks.append({"state": "PASS" if condition else "CAPTURED-UNGRADABLE",
                       "label": label, "detail": detail})

    text_fields = ("unit_serial", "firmware_version", "midi_channel", "capture_tool",
                   "capture_tool_version", "sample_clock_identity",
                   "interface_or_logic_analyzer", "starting_program_name",
                   "front_panel_record", "pattern_memory_protect_original")
    for field in text_fields:
        value = metadata.get(field)
        evidence(value not in (None, ""), f"metadata-{field}", value)
    midi_rate = metadata.get("midi_digital_sample_rate_hz")
    audio_rate = metadata.get("audio_sample_rate_hz")
    evidence(isinstance(midi_rate, (int, float)) and midi_rate >= 2_000_000,
             "metadata-midi-sample-rate", midi_rate)
    evidence(isinstance(audio_rate, (int, float)) and audio_rate >= 96_000,
             "metadata-audio-sample-rate", audio_rate)
    for field in ("common_clock_confirmed", "no_write_confirmed",
                  "no_reflash_confirmed", "teardown_restored"):
        evidence(metadata.get(field) is True, f"metadata-{field}", metadata.get(field))
    for field in ("preflight_current_program_dump", "preflight_global_dump",
                  "preflight_pattern_dump", "final_current_program_dump",
                  "final_pattern_dump"):
        path = resolve(base, metadata.get(field))
        evidence(path is not None and path.is_file(), f"artifact-{field}",
                 None if path is None else str(path))
    return checks


def fixture_document(suite_name: str) -> list[dict[str, Any]]:
    return [{"case": case.__dict__, "midi_schedule": suite.scheduled_fixture(case),
             "external_clock": ({"start_s": 11.5, "end_s": 15.5,
                                  "bpm": case.external_bpm, "ppqn": 24}
                                 if case.external_bpm is not None else None)}
            for case in suite.cases_for(suite_name)]


def write_template(directory: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=False)
    fixtures = directory / "fixtures"
    fixtures.mkdir()
    fixture_hashes: dict[str, str] = {}
    for suite_name in ("subdivision", "speed", "external", "pattern", "keysync", "audio"):
        fixture_path = fixtures / f"{suite_name}.json"
        fixture_path.write_text(
            json.dumps(fixture_document(suite_name), indent=2, sort_keys=True) + "\n",
            encoding="utf-8")
        fixture_hashes[f"fixtures/{suite_name}.json"] = sha256(fixture_path)

    takes: list[dict[str, Any]] = [{
        "name": "interface_loopback", "kind": "loopback",
        "midi_in": "captures/loopback_source.jsonl",
        "midi_out": "captures/loopback_return.jsonl",
    }]
    divisions = [("quarter", 24), ("quarter_triplet", 16), ("eighth", 12),
                 ("eighth_triplet", 8), ("sixteenth", 6), ("sixteenth_triplet", 4)]
    for name, clocks in divisions:
        common = {"expected_clocks_per_onset": clocks,
                  "expected_note_cycle": [60, 64, 67, 71],
                  "expected_velocity_cycle": [100],
                  "minimum_note_onsets": 32, "minimum_gates": 31}
        takes.append({"name": f"internal_{name}", "kind": "internal",
                      "midi_out": f"captures/internal_{name}_out.jsonl",
                      "fixture_file": "fixtures/subdivision.json", "fixture_case": name,
                      **common})
        for repeat in range(1, 4):
            takes.append({"name": f"external_{name}_take_{repeat}", "kind": "external",
                          "midi_in": f"captures/external_{name}_take_{repeat}_in.jsonl",
                          "midi_out": f"captures/external_{name}_take_{repeat}_out.jsonl",
                          "fixture_file": "fixtures/external.json",
                          "fixture_case": f"external_{name}", "repeat": repeat, **common})
    for code, bpm in SPEED_REFERENCES.items():
        for repeat in range(1, 4):
            takes.append({"name": f"speed_{code}_take_{repeat}", "kind": "speed",
                          "midi_out": f"captures/speed_{code}_take_{repeat}_out.jsonl",
                          "fixture_file": "fixtures/speed.json",
                          "fixture_case": f"speed_{code:03d}", "repeat": repeat,
                          "speed_position": code, "speed_adc_code": None,
                          "speed_bpm_tolerance_percent": 0.5,
                          "emulator_reference_adc": code, "emulator_reference_bpm": bpm,
                          "expected_clocks_per_onset": 12,
                          "minimum_note_onsets": 32})
    for repeat in range(1, 4):
        takes.append({"name": f"offset_take_{repeat}", "kind": "offset",
                      "midi_out": f"captures/offset_take_{repeat}_out.jsonl",
                      "fixture_file": "fixtures/pattern.json",
                      "fixture_case": "pattern_offset"})
    for repeat in range(1, 4):
        for sync in (False, True):
            for phase in ("a", "b"):
                label = "on" if sync else "off"
                takes.append({"name": f"keysync_{label}_{phase}_take_{repeat}",
                              "kind": "keysync",
                              "midi_in": f"captures/keysync_{label}_{phase}_take_{repeat}_in.jsonl",
                              "midi_out": f"captures/keysync_{label}_{phase}_take_{repeat}_out.jsonl",
                              "fixture_file": "fixtures/keysync.json",
                              "fixture_case": f"keysync_{label}_phase_{phase}",
                              "key_sync": sync, "phase": phase, "repeat": repeat})
    for repeat in range(1, 4):
        takes.append({"name": f"audio_take_{repeat}", "kind": "audio",
                      "midi_out": f"captures/audio_take_{repeat}_out.jsonl",
                      "wav": f"captures/audio_take_{repeat}.wav",
                      "wav_time_origin_s": 0.0,
                      "fixture_file": "fixtures/audio.json",
                      "fixture_case": "audio_midi_onset"})

    manifest = {
        "schema": 1,
        "timestamp_convention": "MIDI byte stop-bit seconds",
        "fixture_directory": "fixtures",
        "fixture_sha256": fixture_hashes,
        "require_campaign_metadata": True,
        "metadata": {
            "unit_serial": "",
            "firmware_version": "",
            "midi_channel": "",
            "capture_tool": "",
            "capture_tool_version": "",
            "sample_clock_identity": "",
            "interface_or_logic_analyzer": "",
            "midi_digital_sample_rate_hz": None,
            "audio_sample_rate_hz": None,
            "starting_program_name": "",
            "front_panel_record": "",
            "pattern_memory_protect_original": "",
            "preflight_current_program_dump": "captures/preflight_current_program.syx",
            "preflight_global_dump": "captures/preflight_global.syx",
            "preflight_pattern_dump": "captures/preflight_pat1.syx",
            "final_current_program_dump": "captures/final_current_program.syx",
            "final_pattern_dump": "captures/final_pat1.syx",
            "common_clock_confirmed": False,
            "no_write_confirmed": False,
            "no_reflash_confirmed": False,
            "teardown_restored": False,
        },
        "takes": takes,
    }
    path = directory / "manifest.json"
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (directory / "captures").mkdir()
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", nargs="?", type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--write-template", type=Path, metavar="DIRECTORY")
    args = parser.parse_args()
    if args.write_template:
        try:
            path = write_template(args.write_template)
        except OSError as error:
            print(error, file=sys.stderr)
            return 2
        print(path)
        return 0
    if args.manifest is None:
        parser.error("manifest is required unless --write-template is used")
    try:
        parsed = json.loads(args.manifest.read_text(encoding="utf-8"))
        if not isinstance(parsed, dict):
            raise ValueError("manifest root must be an object")
        document = parsed
        if document.get("schema") != 1 or not isinstance(document.get("takes"), list):
            raise ValueError("manifest must have schema=1 and a takes array")
        if not all(isinstance(take, dict) for take in document["takes"]):
            raise ValueError("every takes entry must be an object")
        takes = document["takes"]
        validate_manifest(document, args.manifest.parent)
        results = [grade_take(args.manifest.parent, take) for take in takes]
        shared = cross_checks(takes, results)
        campaign = campaign_checks(document, args.manifest.parent)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(error, file=sys.stderr)
        return 2
    states = ([result["verdict"] for result in results] +
              [item["state"] for item in shared] + [item["state"] for item in campaign])
    verdict = ("FAIL" if "FAIL" in states else
               "CAPTURED-UNGRADABLE" if "CAPTURED-UNGRADABLE" in states else "PASS")
    output = {"schema": 1, "verdict": verdict, "take_count": len(results),
              "pass_count": states.count("PASS"), "fail_count": states.count("FAIL"),
              "ungradable_count": states.count("CAPTURED-UNGRADABLE"),
              "campaign_checks": campaign, "cross_checks": shared, "takes": results}
    rendered = json.dumps(output, indent=2, sort_keys=True) + "\n"
    if args.json_out:
        args.json_out.write_text(rendered, encoding="utf-8")
    print(f"{verdict}: {len(results)} takes; {output['fail_count']} failed, "
          f"{output['ungradable_count']} ungradable")
    for result in results:
        print(f"  {result['verdict']:21s} {result['name']}")
        for check in result["checks"]:
            if check["state"] != "PASS":
                print(f"    {check['state']} {check['label']}: {check['detail']}")
    for check in shared:
        print(f"  {check['state']:21s} {check['label']}: {check['detail']}")
    for check in campaign:
        if check["state"] != "PASS":
            print(f"  {check['state']:21s} {check['label']}: {check['detail']}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
