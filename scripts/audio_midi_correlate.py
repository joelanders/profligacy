#!/usr/bin/env python3
"""Grade sparse rendered-audio onsets against firmware MIDI OUT timestamps."""

from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path

import numpy as np

import arp_timing_analyze as timing


def read_pcm_wav(wav_path: Path) -> tuple[int, int, np.ndarray]:
    with wave.open(str(wav_path), "rb") as source:
        rate = source.getframerate()
        channels = source.getnchannels()
        width = source.getsampwidth()
        raw = source.readframes(source.getnframes())
    if width == 1:
        samples = np.frombuffer(raw, dtype=np.uint8).astype(np.int16) - 128
    elif width == 2:
        samples = np.frombuffer(raw, dtype="<i2")
    elif width == 3:
        octets = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        unsigned = octets[:, 0] | (octets[:, 1] << 8) | (octets[:, 2] << 16)
        samples = (unsigned ^ 0x800000) - 0x800000
    elif width == 4:
        samples = np.frombuffer(raw, dtype="<i4")
    else:
        raise ValueError(f"unsupported PCM sample width: {width} bytes")
    if len(samples) % channels:
        raise ValueError("WAV sample count is not divisible by its channel count")
    audio = samples.reshape(-1, channels).astype(np.float64)
    return rate, channels, audio


def analyze(wav_path: Path, midi_result: dict[str, object]) -> dict[str, object]:
    rate, channels, audio = read_pcm_wav(wav_path)
    power = np.mean(audio * audio, axis=1)
    window = max(1, round(rate * 0.001))
    hop = max(1, round(rate * 0.0005))
    smoothed = np.sqrt(np.convolve(power, np.ones(window) / window, mode="valid"))[::hop]
    times = (np.arange(len(smoothed)) * hop + window / 2.0) / rate

    midi_onsets = np.asarray(midi_result.get("onset_times_s", []), dtype=np.float64)
    if len(midi_onsets) < 2:
        raise ValueError("not enough MIDI onsets")
    region = (times >= midi_onsets[0] - 0.1) & (times <= midi_onsets[-1] + 0.3)
    active = smoothed[region]
    threshold = max(10.0, float(np.percentile(active, 90)) * 0.08)
    above = smoothed > threshold
    rising_indices = np.flatnonzero(above & ~np.r_[False, above[:-1]])
    candidates = times[rising_indices]

    matched_audio: list[float] = []
    next_candidate = 0
    for onset in midi_onsets:
        while next_candidate < len(candidates) and candidates[next_candidate] < onset - 0.05:
            next_candidate += 1
        possible = candidates[next_candidate:next_candidate + 3]
        if len(possible) == 0:
            break
        local = int(np.argmin(np.abs(possible - onset)))
        candidate = float(possible[local])
        if abs(candidate - onset) > 0.05:
            continue
        matched_audio.append(candidate)
        next_candidate += local + 1

    matched_count = min(len(matched_audio), len(midi_onsets))
    matched_midi = midi_onsets[:matched_count]
    matched = np.asarray(matched_audio[:matched_count], dtype=np.float64)
    latencies = matched - matched_midi
    audio_stats = timing.series_stats(matched.tolist()) if matched_count >= 2 else {}
    midi_stats = timing.series_stats(matched_midi.tolist()) if matched_count >= 2 else {}
    audio_fit = float(audio_stats.get("fit_interval_s", 0.0))
    midi_fit = float(midi_stats.get("fit_interval_s", 0.0))
    interval_delta = np.diff(matched) - np.diff(matched_midi)
    latency_span = float(np.ptp(latencies)) if matched_count else 999.0
    median_latency = float(np.median(latencies)) if matched_count else 999.0
    max_interval_delta = float(np.max(np.abs(interval_delta))) if len(interval_delta) else 999.0
    checks = {
        "matched_onsets": matched_count >= min(12, len(midi_onsets)),
        "bounded_fixed_latency": 0.0 < median_latency < 0.020,
        "latency_spread": latency_span < 0.002,
        "fitted_period_agrees": abs(audio_fit - midi_fit) < 0.000050,
        "individual_periods_agree": max_interval_delta < 0.001,
    }
    return {
        "sample_rate": rate,
        "threshold": threshold,
        "midi_onset_count": len(midi_onsets),
        "audio_onset_candidates_s": candidates.tolist(),
        "matched_audio_onsets_s": matched.tolist(),
        "matched_midi_onsets_s": matched_midi.tolist(),
        "latencies_s": latencies.tolist(),
        "median_latency_s": median_latency,
        "latency_span_s": latency_span,
        "audio_fit_interval_s": audio_fit,
        "midi_fit_interval_s": midi_fit,
        "fit_interval_delta_s": audio_fit - midi_fit,
        "max_abs_individual_interval_delta_s": max_interval_delta,
        "checks": checks,
        "verdict": "PASS" if all(checks.values()) else "FAIL",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wav", type=Path)
    parser.add_argument("midi_result", type=Path,
                        help="suite result.json or a bare arp_timing_analyze result")
    parser.add_argument("--json-out", type=Path)
    args = parser.parse_args()
    document = json.loads(args.midi_result.read_text(encoding="utf-8"))
    midi_result = document.get("result", document)
    result = analyze(args.wav, midi_result)
    print(json.dumps(result, indent=2, sort_keys=True))
    if args.json_out:
        args.json_out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    return 0 if result["verdict"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
