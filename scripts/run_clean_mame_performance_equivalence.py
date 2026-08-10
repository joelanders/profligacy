#!/usr/bin/env python3
"""Order-balanced real-time health and implementation non-inferiority gate.

This deliberately reports two different questions:

* did each implementation meet an absolute post-warmup real-time budget; and
* is the candidate consistently worse than the reference after balancing order?

An optional identical-binary control checks that the comparison would not assign a
regression to two copies of the same executable under the current machine load.
"""

from __future__ import annotations

import argparse
import copy
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time


SUMMARY_RE = re.compile(
    r"\[console\] wall=([0-9.]+) s\s+ratio=([0-9.]+).*ticks=(\d+)\s+underruns=(\d+)\s+peak=(\d+)"
)
PRODUCED_RE = re.compile(r"\[console\] block=(\d+) frames\s+produced=([0-9.]+) s emulated")
DETAIL_RE = re.compile(
    r"\[console\] underrun-detail total_callbacks=(\d+) total_frames=(\d+) "
    r"post_warmup_seconds=([0-9.]+) post_warmup_callbacks=(\d+) "
    r"post_warmup_frames=(\d+) max_callback_streak=(\d+)"
)
DROPPED_RE = re.compile(r"\[console\] scheduled-midi-dropped=(\d+)")
SOAK_RE = re.compile(r"\[soak\] t=([0-9.]+)s underruns=(\d+) notes=(\d+) pcs=(\d+) ccs=(\d+)")
PF4_RE = re.compile(
    r"\[pf4\] calls=(\d+) runs=(\d+) fallbacks=(\d+) compiles=(\d+)"
    r"(?: forced_midframe=(\d+))?"
)
SIZE_RE = re.compile(r"\[pooled-size\] pooled frame: (\d+) bytes")


@dataclass
class RunResult:
    implementation: str
    repeat: int
    ordinal: int
    position_in_pair: int
    command: list[str]
    returncode: int
    process_wall_seconds: float
    produced_seconds: float | None
    console_wall_seconds: float | None
    ratio: float | None
    ticks: int | None
    underrun_callbacks: int | None
    peak: int | None
    total_underrun_frames: int | None
    post_warmup_callbacks: int | None
    post_warmup_frames: int | None
    max_callback_streak: int | None
    scheduled_midi_dropped: int | None
    soak_notes: int | None
    soak_program_changes: int | None
    soak_ccs: int | None
    pooled_frame_sizes: list[int]
    max_pooled_runs: int
    max_pooled_compiles: int
    errors: list[str]
    log: str


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def p95(values: list[int]) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, (95 * len(ordered) + 99) // 100 - 1)
    return float(ordered[index])


def clean_environment(collect_native_stats: bool) -> dict[str, str]:
    environment = os.environ.copy()
    for key in list(environment):
        if key.startswith("KPROP_") or key.startswith("PROPHOST_") or key.startswith("SDL_"):
            environment.pop(key)
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy",
            "PROPHOST_WAV_OUT": "/dev/null",
        }
    )
    if collect_native_stats:
        environment["KPROP_PF4_STATS"] = "1"
    return environment


def run_one(
    implementation: str,
    repeat: int,
    ordinal: int,
    position_in_pair: int,
    binary: Path,
    rompath: Path,
    seed: Path,
    output: Path,
    seconds: int,
    block: int,
    ring_frames: int,
    warmup_seconds: float,
    require_native: bool,
    collect_native_stats: bool,
) -> RunResult:
    run_dir = output / "runs" / f"{ordinal:02d}_{implementation}_r{repeat}"
    nvram = run_dir / "nvram"
    cfg = run_dir / "cfg"
    snapshot = run_dir / "snapshot"
    (nvram / "korgprop").mkdir(parents=True)
    cfg.mkdir()
    snapshot.mkdir()
    shutil.copy2(seed, nvram / "korgprop" / "sysram")
    log = run_dir / "run.log"

    command = [
        str(binary),
        "korgprop",
        "-rompath", str(rompath),
        "-nvram_directory", str(nvram),
        "-cfg_directory", str(cfg),
        "-snapshot_directory", str(snapshot),
        "-seconds_to_run", str(seconds),
        "-video", "none",
        "-sound", "none",
        "-videodriver", "dummy",
        "-nothrottle",
        "-skip_gameinfo",
    ]
    environment = clean_environment(collect_native_stats)
    environment.update(
        {
            "PROPHOST_BLOCK": str(block),
            "PROPHOST_RING_FRAMES": str(ring_frames),
            "PROPHOST_SOAK": str(seconds),
            "PROPHOST_METRIC_WARMUP": str(warmup_seconds),
        }
    )
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
    process_wall = time.monotonic() - started
    text = log.read_text(encoding="utf-8", errors="replace")
    summaries = list(SUMMARY_RE.finditer(text))
    produced_rows = list(PRODUCED_RE.finditer(text))
    details = list(DETAIL_RE.finditer(text))
    dropped_rows = list(DROPPED_RE.finditer(text))
    soak_rows = list(SOAK_RE.finditer(text))
    stats = [tuple(int(value or 0) for value in match.groups()) for match in PF4_RE.finditer(text)]
    summary = summaries[-1] if summaries else None
    produced = produced_rows[-1] if produced_rows else None
    detail = details[-1] if details else None
    dropped = dropped_rows[-1] if dropped_rows else None
    soak = soak_rows[-1] if soak_rows else None
    sizes = sorted({int(match.group(1)) for match in SIZE_RE.finditer(text)})

    errors: list[str] = []
    if completed.returncode != 0:
        errors.append(f"process returned {completed.returncode}")
    if not summary or not produced or not detail or not dropped:
        errors.append("missing structured console performance output")
    produced_seconds = float(produced.group(2)) if produced else None
    ratio = float(summary.group(2)) if summary else None
    peak = int(summary.group(5)) if summary else None
    scheduled_drops = int(dropped.group(1)) if dropped else None
    if produced_seconds is not None and produced_seconds < seconds - 0.01:
        errors.append(f"produced only {produced_seconds:.3f}s, expected {seconds}s")
    if ratio is not None and not 0.95 <= ratio <= 1.10:
        errors.append(f"host pacing ratio {ratio:.3f} outside 0.95..1.10")
    if peak is not None and peak <= 0:
        errors.append("soak produced no nonzero PCM")
    if scheduled_drops is not None and scheduled_drops != 0:
        errors.append(f"scheduled MIDI dropped {scheduled_drops} bytes")
    max_runs = max((row[1] for row in stats), default=0)
    max_compiles = max((row[3] for row in stats), default=0)
    if require_native and (not sizes or max_runs <= 0 or max_compiles <= 0):
        errors.append("candidate lacks pooled native compile/run evidence")

    result = RunResult(
        implementation=implementation,
        repeat=repeat,
        ordinal=ordinal,
        position_in_pair=position_in_pair,
        command=command,
        returncode=completed.returncode,
        process_wall_seconds=round(process_wall, 6),
        produced_seconds=produced_seconds,
        console_wall_seconds=float(summary.group(1)) if summary else None,
        ratio=ratio,
        ticks=int(summary.group(3)) if summary else None,
        underrun_callbacks=int(summary.group(4)) if summary else None,
        peak=peak,
        total_underrun_frames=int(detail.group(2)) if detail else None,
        post_warmup_callbacks=int(detail.group(4)) if detail else None,
        post_warmup_frames=int(detail.group(5)) if detail else None,
        max_callback_streak=int(detail.group(6)) if detail else None,
        scheduled_midi_dropped=scheduled_drops,
        soak_notes=int(soak.group(3)) if soak else None,
        soak_program_changes=int(soak.group(4)) if soak else None,
        soak_ccs=int(soak.group(5)) if soak else None,
        pooled_frame_sizes=sizes,
        max_pooled_runs=max_runs,
        max_pooled_compiles=max_compiles,
        errors=errors,
        log=str(log),
    )
    print(
        f"{ordinal:02d} {implementation} r{repeat}: "
        f"post_frames={result.post_warmup_frames} callbacks={result.post_warmup_callbacks} "
        f"ratio={result.ratio} native_runs={result.max_pooled_runs} "
        f"{'VALID' if not errors else 'INVALID'}",
        flush=True,
    )
    return result


def summarize(rows: list[RunResult]) -> dict[str, object]:
    frames = [row.post_warmup_frames or 0 for row in rows]
    callbacks = [row.post_warmup_callbacks or 0 for row in rows]
    ratios = [row.ratio or 0.0 for row in rows]
    return {
        "runs": len(rows),
        "post_warmup_frames_total": sum(frames),
        "post_warmup_frames_median": statistics.median(frames),
        "post_warmup_frames_p95": p95(frames),
        "post_warmup_frames_max": max(frames, default=0),
        "zero_drop_runs": sum(value == 0 for value in frames),
        "post_warmup_callbacks_total": sum(callbacks),
        "post_warmup_callbacks_median": statistics.median(callbacks),
        "post_warmup_callbacks_p95": p95(callbacks),
        "ratio_median": statistics.median(ratios),
        "ratio_p95": p95([int(value * 1_000_000) for value in ratios]) / 1_000_000.0,
    }


def balanced_schedule(left: str, right: str, repeats: int) -> list[tuple[int, tuple[str, str]]]:
    """Give both labels equal exposure to first and second position."""
    if repeats < 4 or repeats % 2:
        raise ValueError("balanced comparison requires an even repeat count of at least 4")
    return [
        (repeat, (left, right) if repeat % 2 else (right, left))
        for repeat in range(1, repeats + 1)
    ]


def absolute_realtime_health(
    rows: list[RunResult], max_frames_per_run: int, max_callbacks_per_run: int
) -> dict[str, object]:
    """Grade real-time health without comparing one executable to another."""
    violations: list[str] = []
    for row in rows:
        frames = row.post_warmup_frames
        callbacks = row.post_warmup_callbacks
        if frames is None or callbacks is None:
            violations.append(f"repeat {row.repeat}: missing underrun metrics")
            continue
        if frames > max_frames_per_run:
            violations.append(
                f"repeat {row.repeat}: {frames} post-warmup frames exceed "
                f"absolute limit {max_frames_per_run}"
            )
        if callbacks > max_callbacks_per_run:
            violations.append(
                f"repeat {row.repeat}: {callbacks} post-warmup callbacks exceed "
                f"absolute limit {max_callbacks_per_run}"
            )
    return {
        "max_frames_per_run": max_frames_per_run,
        "max_callbacks_per_run": max_callbacks_per_run,
        "passed": not violations,
        "violations": violations,
    }


def comparative_noninferiority(
    reference: list[RunResult], candidate: list[RunResult], margin_frames: int
) -> dict[str, object]:
    """Robust, order-stratified comparison of paired missing-frame counts.

    Aggregate totals and maxima are intentionally not used: a single scheduler
    outlier can land on either label, as the identical-clean control demonstrated.
    The absolute health result still preserves and fails that outlier.  Comparative
    regression attribution instead requires a median/order-stratum signal or a
    majority of pairs beyond the declared margin.
    """
    reference_by_repeat = {row.repeat: row for row in reference}
    candidate_by_repeat = {row.repeat: row for row in candidate}
    repeats = sorted(set(reference_by_repeat) & set(candidate_by_repeat))
    paired_differences = [
        (candidate_by_repeat[repeat].post_warmup_frames or 0)
        - (reference_by_repeat[repeat].post_warmup_frames or 0)
        for repeat in repeats
    ]
    position_rows: dict[str, dict[str, list[int]]] = {
        "first": {"reference": [], "candidate": []},
        "second": {"reference": [], "candidate": []},
    }
    for name, rows in (("reference", reference), ("candidate", candidate)):
        for row in rows:
            position = "first" if row.position_in_pair == 1 else "second"
            position_rows[position][name].append(row.post_warmup_frames or 0)

    strata: dict[str, dict[str, object]] = {}
    strata_pass = True
    for position, values in position_rows.items():
        reference_median = statistics.median(values["reference"])
        candidate_median = statistics.median(values["candidate"])
        difference = candidate_median - reference_median
        passed = difference <= margin_frames
        strata_pass = strata_pass and passed
        strata[position] = {
            "reference_median_frames": reference_median,
            "candidate_median_frames": candidate_median,
            "candidate_minus_reference_median_frames": difference,
            "passed": passed,
        }

    paired_median = statistics.median(paired_differences)
    worse_pairs = sum(value > margin_frames for value in paired_differences)
    majority_limit = len(paired_differences) // 2
    passed = (
        paired_median <= margin_frames
        and worse_pairs <= majority_limit
        and strata_pass
    )
    return {
        "method": "paired median plus equal-order strata and beyond-margin majority",
        "margin_frames_per_run": margin_frames,
        "paired_candidate_minus_reference_frames": paired_differences,
        "paired_difference_median_frames": paired_median,
        "pairs_beyond_margin": worse_pairs,
        "maximum_pairs_beyond_margin": majority_limit,
        "order_strata": strata,
        "passed": passed,
    }


def comparative_status(
    raw_comparison: dict[str, object],
    reference_health: dict[str, object],
    control_health: dict[str, dict[str, object]] | None,
    control_discriminating: bool,
) -> dict[str, object]:
    """Return PASS/FAIL/INCONCLUSIVE without blaming a noisy baseline or control."""
    inconclusive_reasons: list[str] = []
    if not reference_health["passed"]:
        inconclusive_reasons.append("reference failed absolute real-time health")
    if control_health is not None:
        unhealthy_controls = [name for name, health in control_health.items() if not health["passed"]]
        if unhealthy_controls:
            inconclusive_reasons.append(
                "identical-binary control failed absolute real-time health: "
                + ", ".join(unhealthy_controls)
            )
    if not control_discriminating:
        inconclusive_reasons.append("identical-binary comparison was label/order-sensitive")

    if inconclusive_reasons:
        status = "INCONCLUSIVE"
    else:
        status = "PASS" if raw_comparison["passed"] else "FAIL"
    return {"status": status, "reasons": inconclusive_reasons}


def overall_verdict(
    structural_failures: list[str], candidate_health_passed: bool, comparison_status: str
) -> str:
    """Candidate health/regression failures are fatal; noisy evidence is inconclusive."""
    if structural_failures or not candidate_health_passed or comparison_status == "FAIL":
        return "FAIL"
    if comparison_status == "INCONCLUSIVE":
        return "INCONCLUSIVE"
    return "PASS"


def regrade_receipt(receipt: dict[str, object]) -> dict[str, object]:
    """Apply final three-state grading to a captured schema-2 receipt offline."""
    graded = copy.deepcopy(receipt)
    acceptance = graded["acceptance"]
    absolute = acceptance["absolute_realtime_health"]
    comparison = acceptance["comparative_noninferiority"]
    control = graded.get("identical_binary_control")
    control_health = control["absolute_realtime_health"] if control is not None else None
    control_discriminating = control is None or bool(control["comparison_discriminating"])
    grade = comparative_status(
        comparison,
        absolute["reference"],
        control_health,
        control_discriminating,
    )

    all_rows = list(graded.get("runs", []))
    if control is not None:
        all_rows.extend(control.get("runs", []))
    structural_failures = [
        f"{row['implementation']} repeat {row['repeat']}: {error}"
        for row in all_rows
        for error in row.get("errors", [])
    ]
    failures = list(structural_failures)
    failures.extend(
        f"candidate absolute health: {item}"
        for item in absolute["candidate"]["violations"]
    )
    if grade["status"] == "FAIL":
        failures.append("candidate shows order-balanced comparative non-inferiority failure")
    verdict = overall_verdict(
        structural_failures,
        bool(absolute["candidate"]["passed"]),
        str(grade["status"]),
    )
    acceptance["comparative_status"] = grade
    acceptance["candidate_release_health_passed"] = absolute["candidate"]["passed"]
    acceptance["comparison_discriminating"] = control_discriminating
    graded["failures"] = failures
    graded["verdict"] = verdict
    graded["passed"] = verdict == "PASS"
    return graded


def run_series(
    labels: tuple[str, str],
    binaries: dict[str, Path],
    repeats: int,
    ordinal_start: int,
    output: Path,
    args: argparse.Namespace,
    require_native_label: str | None,
) -> tuple[list[RunResult], int]:
    rows: list[RunResult] = []
    ordinal = ordinal_start
    for repeat, order in balanced_schedule(labels[0], labels[1], repeats):
        for position_in_pair, implementation in enumerate(order, start=1):
            ordinal += 1
            rows.append(
                run_one(
                    implementation,
                    repeat,
                    ordinal,
                    position_in_pair,
                    binaries[implementation],
                    args.rompath.resolve(),
                    args.nvram_seed.resolve(),
                    output,
                    args.seconds,
                    args.block,
                    args.ring_frames,
                    args.warmup_seconds,
                    implementation == require_native_label,
                    args.collect_native_stats,
                )
            )
    return rows, ordinal


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--rompath", type=Path, required=True)
    parser.add_argument("--nvram-seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--repeats", type=int, default=6,
        help="even primary pair count; equalizes first/second run order (default: 6)",
    )
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--block", type=int, default=512)
    parser.add_argument("--ring-frames", type=int, default=2048)
    parser.add_argument("--warmup-seconds", type=float, default=12.0)
    parser.add_argument(
        "--identical-control", choices=("reference", "candidate", "none"), default="reference",
        help="run an A/A scheduling control with this binary (default: reference)",
    )
    parser.add_argument(
        "--control-repeats", type=int, default=4,
        help="even identical-binary control pair count (default: 4)",
    )
    parser.add_argument(
        "--absolute-max-frames-per-run", type=int, default=0,
        help="absolute post-warmup missing-frame budget for each run (default: zero)",
    )
    parser.add_argument(
        "--absolute-max-callbacks-per-run", type=int, default=0,
        help="absolute post-warmup short-callback budget for each run (default: zero)",
    )
    parser.add_argument(
        "--comparison-margin-frames", type=int,
        help="per-run non-inferiority margin (default: one --block)",
    )
    parser.add_argument(
        "--collect-native-stats", action="store_true",
        help="enable perturbing PF4 diagnostic output; off for normal timing runs",
    )
    parser.add_argument(
        "--require-candidate-native-evidence", action="store_true",
        help="require PF4 native compile/run evidence (implies --collect-native-stats)",
    )
    args = parser.parse_args()

    for binary in (args.reference, args.candidate):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"executable not found: {binary}")
    if not args.rompath.is_dir():
        parser.error(f"ROM path not found: {args.rompath}")
    if not args.nvram_seed.is_file():
        parser.error(f"NVRAM seed not found: {args.nvram_seed}")
    if args.output.exists():
        parser.error(f"refusing stale output directory: {args.output}")
    try:
        balanced_schedule("reference", "candidate", args.repeats)
        if args.identical_control != "none":
            balanced_schedule("control_a", "control_b", args.control_repeats)
    except ValueError as error:
        parser.error(str(error))
    if args.seconds <= args.warmup_seconds + 15:
        parser.error("run must contain at least 15 post-warmup seconds")
    if args.absolute_max_frames_per_run < 0 or args.absolute_max_callbacks_per_run < 0:
        parser.error("absolute real-time budgets must be non-negative")
    if args.comparison_margin_frames is not None and args.comparison_margin_frames < 0:
        parser.error("--comparison-margin-frames must be non-negative")
    if args.require_candidate_native_evidence:
        args.collect_native_stats = True
    args.output.mkdir(parents=True)

    binaries = {"reference": args.reference.resolve(), "candidate": args.candidate.resolve()}
    rows, ordinal = run_series(
        ("candidate", "reference"),
        binaries,
        args.repeats,
        0,
        args.output,
        args,
        "candidate" if args.require_candidate_native_evidence else None,
    )

    groups = {
        implementation: [row for row in rows if row.implementation == implementation]
        for implementation in ("reference", "candidate")
    }
    summaries = {name: summarize(group) for name, group in groups.items()}
    structural_failures = [
        f"{row.implementation} repeat {row.repeat}: {error}"
        for row in rows
        for error in row.errors
    ]
    absolute = {
        name: absolute_realtime_health(
            group, args.absolute_max_frames_per_run, args.absolute_max_callbacks_per_run
        )
        for name, group in groups.items()
    }
    margin = args.comparison_margin_frames
    if margin is None:
        margin = args.block
    comparison = comparative_noninferiority(groups["reference"], groups["candidate"], margin)

    control: dict[str, object] | None = None
    if args.identical_control != "none":
        control_binary = binaries[args.identical_control]
        control_binaries = {"control_a": control_binary, "control_b": control_binary}
        control_rows, ordinal = run_series(
            ("control_a", "control_b"),
            control_binaries,
            args.control_repeats,
            ordinal,
            args.output,
            args,
            None,
        )
        rows.extend(control_rows)
        control_groups = {
            label: [row for row in control_rows if row.implementation == label]
            for label in ("control_a", "control_b")
        }
        structural_failures.extend(
            f"{row.implementation} repeat {row.repeat}: {error}"
            for row in control_rows
            for error in row.errors
        )
        control_comparison = comparative_noninferiority(
            control_groups["control_a"], control_groups["control_b"], margin
        )
        control_reverse_comparison = comparative_noninferiority(
            control_groups["control_b"], control_groups["control_a"], margin
        )
        comparison_discriminating = bool(
            control_comparison["passed"] and control_reverse_comparison["passed"]
        )
        control = {
            "source": args.identical_control,
            "binary_sha256": sha256(control_binary),
            "runs": [asdict(row) for row in control_rows],
            "summary": {name: summarize(group) for name, group in control_groups.items()},
            "absolute_realtime_health": {
                name: absolute_realtime_health(
                    group,
                    args.absolute_max_frames_per_run,
                    args.absolute_max_callbacks_per_run,
                )
                for name, group in control_groups.items()
            },
            "control_b_noninferior_to_a": control_comparison,
            "control_a_noninferior_to_b": control_reverse_comparison,
            "comparison_discriminating": comparison_discriminating,
        }

    control_health = control["absolute_realtime_health"] if control is not None else None
    control_discriminating = control is None or bool(control["comparison_discriminating"])
    comparison_grade = comparative_status(
        comparison,
        absolute["reference"],
        control_health,
        control_discriminating,
    )
    failures = list(structural_failures)
    failures.extend(
        f"candidate absolute health: {item}" for item in absolute["candidate"]["violations"]
    )
    if comparison_grade["status"] == "FAIL":
        failures.append("candidate shows order-balanced comparative non-inferiority failure")
    verdict = overall_verdict(
        structural_failures,
        bool(absolute["candidate"]["passed"]),
        str(comparison_grade["status"]),
    )

    receipt = {
        "schema": 2,
        "gate": "clean_mame_realtime_health_and_performance_noninferiority",
        "inputs": {
            "reference": {"path": str(binaries["reference"]), "sha256": sha256(binaries["reference"])},
            "candidate": {"path": str(binaries["candidate"]), "sha256": sha256(binaries["candidate"])},
            "rompath": str(args.rompath.resolve()),
            "nvram_seed": {"path": str(args.nvram_seed.resolve()), "sha256": sha256(args.nvram_seed)},
            "repeats": args.repeats,
            "seconds": args.seconds,
            "block": args.block,
            "ring_frames": args.ring_frames,
            "warmup_seconds": args.warmup_seconds,
            "collect_native_stats": args.collect_native_stats,
            "identical_control": args.identical_control,
            "control_repeats": args.control_repeats if args.identical_control != "none" else 0,
        },
        "runs": [asdict(row) for row in rows if row.implementation in groups],
        "summary": summaries,
        "acceptance": {
            "absolute_realtime_health": absolute,
            "comparative_noninferiority": comparison,
            "comparative_status": comparison_grade,
            "candidate_release_health_passed": absolute["candidate"]["passed"],
            "comparison_discriminating": control_discriminating,
        },
        "identical_binary_control": control,
        "failures": failures,
        "verdict": verdict,
        "passed": verdict == "PASS",
    }
    receipt_path = args.output / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"summary": summaries, "acceptance": receipt["acceptance"]}, indent=2))
    print(f"{verdict} realtime_and_noninferiority receipt={receipt_path}")
    for failure in failures:
        print(f"  {failure}")
    for reason in comparison_grade["reasons"]:
        print(f"  inconclusive: {reason}")
    return {"PASS": 0, "FAIL": 1, "INCONCLUSIVE": 2}[verdict]


if __name__ == "__main__":
    sys.exit(main())
