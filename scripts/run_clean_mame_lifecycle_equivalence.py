#!/usr/bin/env python3
"""Prove repeated in-process ProphecyEngine teardown/restart for two MAME builds."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
from typing import Any


CYCLE_RE = re.compile(
    r"^\[lifecycle\] cycle=(\d+) started=(\d+) produced=(\d+) lcd=(\d+) "
    r"finished=(\d+) stop_ms=(\d+) wall_ms=(\d+) (PASS|FAIL)$",
    re.MULTILINE,
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def percentile_95(values: list[int]) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = max(0, (95 * len(ordered) + 99) // 100 - 1)
    return float(ordered[index])


def run_case(
    label: str,
    binary: Path,
    rompath: Path,
    seed: Path,
    output: Path,
    cycles: int,
    target_frames: int,
    timeout_ms: int,
    require_pooled: bool,
) -> dict[str, Any]:
    case = output / label
    nvram = case / "nvram"
    cfg = case / "cfg"
    snap = case / "snap"
    shutil.copytree(seed, nvram)
    cfg.mkdir(parents=True)
    snap.mkdir(parents=True)

    command = [
        str(binary),
        "korgprop",
        "-rompath", str(rompath),
        "-video", "none",
        "-sound", "none",
        "-videodriver", "dummy",
        "-nothrottle",
        "-skip_gameinfo",
        "-nvram_directory", str(nvram),
        "-cfg_directory", str(cfg),
        "-snapshot_directory", str(snap),
    ]
    env = os.environ.copy()
    env.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy",
            "PROPHOST_LIFECYCLE_CYCLES": str(cycles),
            "PROPHOST_LIFECYCLE_FRAMES": str(target_frames),
            "PROPHOST_LIFECYCLE_TIMEOUT_MS": str(timeout_ms),
        }
    )
    completed = subprocess.run(
        command,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=cycles * (timeout_ms / 1000.0 + 5.0),
        check=False,
    )
    log = completed.stdout
    (case / "run.log").write_text(log, encoding="utf-8")

    rows = []
    for match in CYCLE_RE.finditer(log):
        rows.append(
            {
                "cycle": int(match.group(1)),
                "started": int(match.group(2)),
                "produced": int(match.group(3)),
                "lcd": int(match.group(4)),
                "finished": int(match.group(5)),
                "stop_ms": int(match.group(6)),
                "wall_ms": int(match.group(7)),
                "verdict": match.group(8),
            }
        )

    errors: list[str] = []
    if completed.returncode != 0:
        errors.append(f"process returned {completed.returncode}")
    if len(rows) != cycles:
        errors.append(f"expected {cycles} cycle rows, found {len(rows)}")
    if f"PASS lifecycle_restart cycles={cycles} target_frames={target_frames}" not in log:
        errors.append("missing final lifecycle PASS marker")
    for expected, row in enumerate(rows, start=1):
        if row["cycle"] != expected:
            errors.append(f"cycle sequence mismatch at row {expected}: {row['cycle']}")
        if row["verdict"] != "PASS":
            errors.append(f"cycle {expected} verdict={row['verdict']}")
        if not (row["started"] and row["lcd"] and row["finished"]):
            errors.append(f"cycle {expected} did not start, publish LCD, and finish")
        if row["produced"] < target_frames:
            errors.append(f"cycle {expected} produced only {row['produced']} frames")
        if row["stop_ms"] >= 3000:
            errors.append(f"cycle {expected} teardown took {row['stop_ms']} ms")
    pooled_frames = log.count("[pooled-size] pooled frame:")
    if require_pooled and pooled_frames < cycles:
        errors.append(
            f"candidate emitted only {pooled_frames} pooled native frame records across {cycles} boots"
        )

    stop_values = [row["stop_ms"] for row in rows]
    wall_values = [row["wall_ms"] for row in rows]
    result = {
        "label": label,
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "command": command,
        "returncode": completed.returncode,
        "cycles": rows,
        "pooled_frame_records": pooled_frames,
        "summary": {
            "stop_ms_median": statistics.median(stop_values) if stop_values else None,
            "stop_ms_p95": percentile_95(stop_values),
            "wall_ms_median": statistics.median(wall_values) if wall_values else None,
            "wall_ms_p95": percentile_95(wall_values),
        },
        "errors": errors,
        "passed": not errors,
    }
    status = "PASS" if result["passed"] else "FAIL"
    print(
        f"{status} {label}: cycles={len(rows)}/{cycles} "
        f"stop_median={result['summary']['stop_ms_median']}ms "
        f"wall_median={result['summary']['wall_ms_median']}ms "
        f"pooled_records={pooled_frames}"
    )
    for error in errors:
        print(f"  {error}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--rompath", type=Path, required=True)
    parser.add_argument("--nvram-seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument("--target-frames", type=int, default=576000)
    parser.add_argument("--timeout-ms", type=int, default=30000)
    args = parser.parse_args()

    for binary in (args.reference, args.candidate):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"executable not found: {binary}")
    if not args.rompath.is_dir():
        parser.error(f"ROM path not found: {args.rompath}")
    if not args.nvram_seed.is_dir():
        parser.error(f"NVRAM seed not found: {args.nvram_seed}")
    if args.cycles < 2:
        parser.error("--cycles must be at least 2 (this is a restart gate)")
    if args.target_frames <= 0 or args.timeout_ms <= 0:
        parser.error("frame and timeout values must be positive")
    if args.output.exists():
        parser.error(f"refusing stale output directory: {args.output}")
    args.output.mkdir(parents=True)

    reference = run_case(
        "reference", args.reference.resolve(), args.rompath.resolve(),
        args.nvram_seed.resolve(), args.output, args.cycles, args.target_frames,
        args.timeout_ms, False,
    )
    candidate = run_case(
        "candidate", args.candidate.resolve(), args.rompath.resolve(),
        args.nvram_seed.resolve(), args.output, args.cycles, args.target_frames,
        args.timeout_ms, True,
    )
    receipt = {
        "schema": 1,
        "gate": "clean_mame_same_process_lifecycle_equivalence",
        "cycles": args.cycles,
        "target_frames": args.target_frames,
        "timeout_ms": args.timeout_ms,
        "rompath": str(args.rompath.resolve()),
        "nvram_seed": str(args.nvram_seed.resolve()),
        "reference": reference,
        "candidate": candidate,
        "passed": reference["passed"] and candidate["passed"],
    }
    (args.output / "receipt.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if receipt["passed"]:
        print(f"PASS lifecycle_equivalence receipt={args.output / 'receipt.json'}")
        return 0
    print(f"FAIL lifecycle_equivalence receipt={args.output / 'receipt.json'}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
