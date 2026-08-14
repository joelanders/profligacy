#!/usr/bin/env python3
"""Threaded-console oracle for interpreter versus CMEM-safe pooled ARM64 execution.

The direct propmin oracle cannot exercise scheduler timeslice splits caused by the
plugin's worker/ring driving.  This gate runs the same clean-backed console binary
in interpreter and pooled+CMEM-deopt modes, from identical isolated state, and
requires exact PCM for boot, note-on, and dense-MIDI scenarios.

Exact PCM is necessary but not sufficient: a paced host may preserve bytes by
catching up after missing an audio deadline.  The gate therefore also requires
zero underrun callbacks and frames after an explicit warmup interval.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import wave


PF4_RE = re.compile(
    r"\[pf4\] calls=(\d+) runs=(\d+) fallbacks=(\d+) compiles=(\d+)"
    r"(?: forced_midframe=(\d+))?"
)
SIZE_RE = re.compile(r"\[pooled-size\] pooled frame: (\d+) bytes")
SUMMARY_RE = re.compile(
    r"\[console\] wall=([0-9.]+) s\s+ratio=([0-9.]+).*ticks=(\d+)\s+underruns=(\d+)\s+peak=(\d+)"
)
UNDERRUN_DETAIL_RE = re.compile(
    r"\[console\] underrun-detail total_callbacks=(\d+) total_frames=(\d+) "
    r"post_warmup_seconds=([0-9.]+) post_warmup_callbacks=(\d+) "
    r"post_warmup_frames=(\d+) max_callback_streak=(\d+)"
)
DROPPED_RE = re.compile(r"\[console\] scheduled-midi-dropped=(\d+)")

SHIPPING_ENV = {
    "KPROP_LIE_BATTERY_OK": "1",
    "KPROP_DISABLE_AUTO_STIM_SWEEP": "1",
    "KPROP_V55_ADC_BANKSW_EXPERIMENT": "6",
    "KPROP_ADC_MUX_FROM_P2": "0",
    "KPROP_ADC_MUX_FROM_SHADOW": "1",
    "KPROP_ADC_MUX_AUTOSCAN": "0",
    "KPROP_DSP_HOST_MAP": "1,2,3",
    "KPROP_DSP2_INPUT_ROUTE": "normal",
    "KPROP_TXSM_FALLBACK_FIX": "1",
    "KPROP_DSP_SERIAL_FRAME_MODEL": "0",
}


@dataclass(frozen=True)
class Scenario:
    name: str
    seconds: int
    environment: dict[str, str]
    dense_midi: bool = False


@dataclass
class Result:
    name: str
    mode: str
    command: list[str]
    environment: dict[str, str]
    returncode: int
    wall_seconds: float
    wav_frames: int
    wav_pcm_bytes: int
    wav_pcm_sha256: str
    wav_sha256: str
    pooled_frame_sizes: list[int]
    max_runs: int
    max_fallbacks: int
    max_compiles: int
    console_ratio: float | None
    console_ticks: int | None
    console_underruns: int | None
    console_peak: int | None
    post_warmup_seconds: float | None
    post_warmup_callbacks: int | None
    post_warmup_frames: int | None
    max_callback_streak: int | None
    scheduled_midi_dropped: int | None
    log: str


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def vlq(value: int) -> bytes:
    output = [value & 0x7F]
    value >>= 7
    while value:
        output.append(0x80 | (value & 0x7F))
        value >>= 7
    return bytes(reversed(output))


def generate_dense_midi(path: Path) -> None:
    """Generate the same deterministic F6-style load as MAME's PF4 gate."""
    tpqn = 480
    ticks_per_second = tpqn * 2
    duration = 60.0
    events: list[tuple[int, bytes]] = []
    notes = [48, 60, 55, 67, 52, 64, 59, 71]

    t = 2.0
    index = 0
    while t < duration:
        note = notes[index % len(notes)]
        events.append((int(t * ticks_per_second), bytes([0x90, note, 100])))
        events.append((int((t + 0.100) * ticks_per_second), bytes([0x80, note, 64])))
        t += 0.125
        index += 1

    t = 2.0
    while t < duration:
        phase = (t % 2.0) / 2.0
        value = int(127 * (2 * phase if phase < 0.5 else 2 - 2 * phase))
        events.append((int(t * ticks_per_second), bytes([0xB0, 1, value])))
        t += 0.020

    t = 2.010
    while t < duration:
        value = max(0, min(16383, 8192 + int(4096 * math.sin(2 * math.pi * t / 3.0))))
        events.append((int(t * ticks_per_second), bytes([0xE0, value & 0x7F, value >> 7])))
        t += 0.020

    t = 2.005
    while t < duration:
        value = int(127 * ((t % 1.5) / 1.5))
        events.append((int(t * ticks_per_second), bytes([0xD0, value])))
        t += 0.040

    events.sort(key=lambda item: item[0])
    track = bytearray()
    track += vlq(0) + bytes([0xFF, 0x51, 0x03]) + struct.pack(">I", 500000)[1:]
    previous = 0
    for tick, event in events:
        track += vlq(tick - previous) + event
        previous = tick
    track += vlq(0) + bytes([0xFF, 0x2F, 0x00])
    output = b"MThd" + struct.pack(">IHHH", 6, 0, 1, tpqn)
    output += b"MTrk" + struct.pack(">I", len(track)) + track
    path.write_bytes(output)


def clean_environment() -> dict[str, str]:
    environment = os.environ.copy()
    for key in list(environment):
        if key.startswith("KPROP_") or key.startswith("PROPHOST_") or key.startswith("SDL_"):
            environment.pop(key)
    environment.update(SHIPPING_ENV)
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy",
            "PROPHOST_BLOCK": "512",
            # Match ProphecyEngine's shipping default (~43 ms at 48 kHz), so the
            # deadline gate cannot pass on buffering unavailable to the plugin.
            "PROPHOST_RING_FRAMES": "2048",
        }
    )
    return environment


def run_one(
    scenario: Scenario,
    mode: str,
    binary: Path,
    rompath: Path,
    nvram_seed: Path,
    dense_midi: Path,
    output: Path,
    metric_warmup_seconds: float,
) -> Result:
    run_dir = output / "runs" / f"{scenario.name}.{mode}"
    nvram = run_dir / "nvram"
    cfg = run_dir / "cfg"
    snapshot = run_dir / "snapshot"
    (nvram / "korgprop").mkdir(parents=True)
    cfg.mkdir()
    snapshot.mkdir()
    shutil.copy2(nvram_seed, nvram / "korgprop" / "sysram")
    wav = run_dir / "audio.wav"
    log = run_dir / "run.log"

    environment = clean_environment()
    environment.update(scenario.environment)
    environment["PROPHOST_METRIC_WARMUP"] = str(metric_warmup_seconds)
    if mode == "interp":
        environment["KPROP_DSP_PERFRAME"] = "0"
    elif mode == "cmem":
        environment["KPROP_DSP_PERFRAME"] = "4"
        environment.update({"KPROP_PF4_CMEM_DEOPT": "1", "KPROP_PF4_STATS": "1"})
    elif mode == "shipping":
        # No PF4/CMEM selector is supplied by the test: ProphecyEngine must apply
        # the shipping defaults itself. Stats provide proof that native code ran.
        environment["KPROP_PF4_STATS"] = "1"
    else:
        raise ValueError(f"unknown mode: {mode}")
    environment["PROPHOST_WAV_OUT"] = str(wav)

    command = [
        str(binary),
        "korgprop",
        "-rompath", str(rompath),
        "-nvram_directory", str(nvram),
        "-cfg_directory", str(cfg),
        "-snapshot_directory", str(snapshot),
        "-seconds_to_run", str(scenario.seconds),
        "-video", "none",
        "-sound", "none",
        "-videodriver", "dummy",
        "-nothrottle",
        "-skip_gameinfo",
    ]
    if scenario.dense_midi:
        command.extend(("-midiin", str(dense_midi)))

    started = time.monotonic()
    with log.open("wb") as sink:
        completed = subprocess.run(
            command,
            cwd=run_dir,
            env=environment,
            stdout=sink,
            stderr=subprocess.STDOUT,
            check=False,
        )
    elapsed = time.monotonic() - started
    log_text = log.read_text(encoding="utf-8", errors="replace")
    if completed.returncode != 0:
        raise RuntimeError(f"{scenario.name}.{mode} exited {completed.returncode}; see {log}")
    if not wav.is_file():
        raise RuntimeError(f"{scenario.name}.{mode} produced no WAV; see {log}")

    with wave.open(str(wav), "rb") as source:
        if source.getframerate() != 48000 or source.getnchannels() != 2 or source.getsampwidth() != 2:
            raise RuntimeError(f"{scenario.name}.{mode} produced an unexpected WAV format")
        frames = source.getnframes()
        pcm = source.readframes(frames)
    stats = [tuple(int(value or 0) for value in match.groups()) for match in PF4_RE.finditer(log_text)]
    summary = list(SUMMARY_RE.finditer(log_text))
    underrun_detail = list(UNDERRUN_DETAIL_RE.finditer(log_text))
    dropped = list(DROPPED_RE.finditer(log_text))
    final = summary[-1] if summary else None
    final_detail = underrun_detail[-1] if underrun_detail else None
    result = Result(
        name=scenario.name,
        mode=mode,
        command=command,
        environment={
            key: environment[key]
            for key in sorted(environment)
            if key.startswith("KPROP_") or key.startswith("PROPHOST_") or key.startswith("SDL_")
        },
        returncode=completed.returncode,
        wall_seconds=round(elapsed, 6),
        wav_frames=frames,
        wav_pcm_bytes=len(pcm),
        wav_pcm_sha256=hashlib.sha256(pcm).hexdigest(),
        wav_sha256=sha256_file(wav),
        pooled_frame_sizes=sorted({int(match.group(1)) for match in SIZE_RE.finditer(log_text)}),
        max_runs=max((row[1] for row in stats), default=0),
        max_fallbacks=max((row[2] for row in stats), default=0),
        max_compiles=max((row[3] for row in stats), default=0),
        console_ratio=float(final.group(2)) if final else None,
        console_ticks=int(final.group(3)) if final else None,
        console_underruns=int(final.group(4)) if final else None,
        console_peak=int(final.group(5)) if final else None,
        post_warmup_seconds=float(final_detail.group(3)) if final_detail else None,
        post_warmup_callbacks=int(final_detail.group(4)) if final_detail else None,
        post_warmup_frames=int(final_detail.group(5)) if final_detail else None,
        max_callback_streak=int(final_detail.group(6)) if final_detail else None,
        scheduled_midi_dropped=int(dropped[-1].group(1)) if dropped else None,
        log=str(log),
    )
    print(
        f"{scenario.name}.{mode}: rc=0 frames={frames} wall={elapsed:.3f}s "
        f"underruns={result.console_underruns} sha={result.wav_pcm_sha256[:12]}",
        flush=True,
    )
    return result


def post_warmup_realtime_health(results: list[Result]) -> dict[str, object]:
    """Grade audio-deadline health separately from exact offline PCM."""
    violations: list[str] = []
    by_mode: dict[str, dict[str, int | float | None | bool]] = {}
    for result in results:
        passed = (
            result.post_warmup_callbacks == 0
            and result.post_warmup_frames == 0
        )
        by_mode[result.mode] = {
            "warmup_seconds": result.post_warmup_seconds,
            "underrun_callbacks": result.post_warmup_callbacks,
            "underrun_frames": result.post_warmup_frames,
            "passed": passed,
        }
        if result.post_warmup_callbacks is None or result.post_warmup_frames is None:
            violations.append(f"{result.mode}: missing post-warmup underrun metrics")
        elif not passed:
            violations.append(
                f"{result.mode}: {result.post_warmup_callbacks} post-warmup underrun "
                f"callbacks / {result.post_warmup_frames} frames"
            )
    return {"passed": not violations, "by_mode": by_mode, "violations": violations}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console", type=Path, required=True)
    parser.add_argument("--rompath", type=Path, required=True)
    parser.add_argument("--nvram-seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--boot-seconds", type=int, default=8)
    parser.add_argument("--note-seconds", type=int, default=14)
    parser.add_argument("--dense-seconds", type=int, default=30)
    parser.add_argument("--metric-warmup-seconds", type=float, default=5.0)
    parser.add_argument(
        "--scenario", choices=("all", "boot", "note", "dense"), default="all",
        help="run all scenarios or one bounded scenario (default: all)",
    )
    args = parser.parse_args()

    if not args.console.is_file() or not os.access(args.console, os.X_OK):
        parser.error(f"console executable not found: {args.console}")
    if not args.rompath.is_dir():
        parser.error(f"ROM path not found: {args.rompath}")
    if not args.nvram_seed.is_file():
        parser.error(f"NVRAM seed not found: {args.nvram_seed}")
    if args.output.exists():
        parser.error(f"refusing stale output directory: {args.output}")
    if args.metric_warmup_seconds < 0:
        parser.error("--metric-warmup-seconds must not be negative")
    args.output.mkdir(parents=True)
    dense_midi = args.output / "dense.mid"
    generate_dense_midi(dense_midi)

    all_scenarios = [
        Scenario("boot", args.boot_seconds, {}),
        Scenario("note", args.note_seconds, {"KPROP_INJECT_NOTE": "11.0:2.0:60:100"}),
        Scenario("dense", args.dense_seconds, {}, True),
    ]
    scenarios = (
        all_scenarios if args.scenario == "all"
        else [scenario for scenario in all_scenarios if scenario.name == args.scenario]
    )
    if any(scenario.seconds <= args.metric_warmup_seconds for scenario in scenarios):
        parser.error("every scenario must run longer than --metric-warmup-seconds")
    results: dict[str, dict[str, Result]] = {}
    failures: list[str] = []
    try:
        for scenario in scenarios:
            results[scenario.name] = {}
            for mode in ("interp", "cmem", "shipping"):
                results[scenario.name][mode] = run_one(
                    scenario,
                    mode,
                    args.console.resolve(),
                    args.rompath.resolve(),
                    args.nvram_seed.resolve(),
                    dense_midi.resolve(),
                    args.output,
                    args.metric_warmup_seconds,
                )
    except RuntimeError as error:
        failures.append(str(error))

    checks: list[dict[str, object]] = []
    for scenario in scenarios:
        pair = results.get(scenario.name, {})
        if any(mode not in pair for mode in ("interp", "cmem", "shipping")):
            failures.append(f"{scenario.name}: incomplete run set")
            continue
        interp = pair["interp"]
        cmem = pair["cmem"]
        shipping = pair["shipping"]
        cmem_exact = (
            interp.wav_frames == cmem.wav_frames
            and interp.wav_pcm_bytes == cmem.wav_pcm_bytes
            and interp.wav_pcm_sha256 == cmem.wav_pcm_sha256
        )
        shipping_exact = (
            interp.wav_frames == shipping.wav_frames
            and interp.wav_pcm_bytes == shipping.wav_pcm_bytes
            and interp.wav_pcm_sha256 == shipping.wav_pcm_sha256
        )
        cmem_native = bool(cmem.pooled_frame_sizes) and cmem.max_runs > 0 and cmem.max_compiles > 0
        shipping_native = (
            bool(shipping.pooled_frame_sizes)
            and shipping.max_runs > 0
            and shipping.max_compiles > 0
        )
        shipping_uses_cmem_variant = shipping.pooled_frame_sizes == cmem.pooled_frame_sizes
        no_drops = all(
            result.scheduled_midi_dropped == 0 for result in (interp, cmem, shipping)
        )
        realtime_health = post_warmup_realtime_health([interp, cmem, shipping])
        if not cmem_exact:
            failures.append(f"{scenario.name}: threaded interpreter and CMEM-deopt PCM differ")
        if not shipping_exact:
            failures.append(f"{scenario.name}: threaded interpreter and shipping-default PCM differ")
        if not cmem_native:
            failures.append(f"{scenario.name}: CMEM mode lacks pooled compile/run evidence")
        if not shipping_native:
            failures.append(f"{scenario.name}: shipping mode lacks pooled compile/run evidence")
        if not shipping_uses_cmem_variant:
            failures.append(f"{scenario.name}: shipping mode did not compile the CMEM-deopt variant")
        if not no_drops:
            failures.append(f"{scenario.name}: scheduled MIDI bytes were dropped")
        if not realtime_health["passed"]:
            failures.extend(
                f"{scenario.name}: {violation}"
                for violation in realtime_health["violations"]
            )
        checks.append(
            {
                "scenario": scenario.name,
                "cmem_exact_pcm": cmem_exact,
                "shipping_exact_pcm": shipping_exact,
                "cmem_native_compiled_and_ran": cmem_native,
                "shipping_native_compiled_and_ran": shipping_native,
                "shipping_uses_cmem_variant": shipping_uses_cmem_variant,
                "scheduled_midi_dropped_zero": no_drops,
                "post_warmup_realtime_health": realtime_health,
                "pcm_sha256": interp.wav_pcm_sha256 if cmem_exact and shipping_exact else None,
            }
        )
        passed = (
            cmem_exact
            and shipping_exact
            and cmem_native
            and shipping_native
            and shipping_uses_cmem_variant
            and no_drops
            and realtime_health["passed"]
        )
        print(
            f"{'PASS' if passed else 'FAIL'} {scenario.name}: cmem_exact={cmem_exact} "
            f"shipping_exact={shipping_exact} native={cmem_native and shipping_native} "
            f"shipping_cmem_variant={shipping_uses_cmem_variant} no_drops={no_drops}"
            f" rt_health={realtime_health['passed']}"
        )

    receipt = {
        "schema": 1,
        "gate": "threaded_pf4_cmem_equivalence",
        "console": {"path": str(args.console.resolve()), "sha256": sha256_file(args.console)},
        "rompath": str(args.rompath.resolve()),
        "nvram_seed": {"path": str(args.nvram_seed.resolve()), "sha256": sha256_file(args.nvram_seed)},
        "dense_midi_sha256": sha256_file(dense_midi),
        "metric_warmup_seconds": args.metric_warmup_seconds,
        "scenario_filter": args.scenario,
        "checks": checks,
        "runs": {
            name: {mode: asdict(result) for mode, result in pair.items()}
            for name, pair in results.items()
        },
        "failures": failures,
        "passed": not failures,
    }
    receipt_path = args.output / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{'PASS' if not failures else 'FAIL'} threaded_pf4_cmem receipt={receipt_path}")
    for failure in failures:
        print(f"  {failure}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
