#!/usr/bin/env python3
"""Run the release control/faceplate comparison in balanced A/B/B/A order."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import run_clean_mame_control_equivalence as control


ABBA = ("reference", "candidate", "candidate", "reference")
STABLE_FIELDS = (
    "stable_events", "midi_sha256", "nvram_sha256", "wav_sha256",
    "name", "sysex_count", "current_dump_length",
)


def pair_checks(reference: dict[str, object], candidate: dict[str, object]) -> dict[str, bool]:
    return {
        "reference_invariants": all(reference["invariants"].values()),
        "candidate_invariants": all(candidate["invariants"].values()),
        "stable_events_identical": reference["stable_events"] == candidate["stable_events"],
        "midi_identical": reference["midi_sha256"] == candidate["midi_sha256"],
        "nvram_identical": reference["nvram_sha256"] == candidate["nvram_sha256"],
        "audio_identical": reference["wav_sha256"] == candidate["wav_sha256"],
        "program_name_identical": reference["name"] == candidate["name"],
        "sysex_shape_identical": (
            reference["sysex_count"] == candidate["sysex_count"]
            and reference["current_dump_length"] == candidate["current_dump_length"]
        ),
    }


def stable_across_runs(runs: list[dict[str, object]]) -> dict[str, bool]:
    if not runs:
        return {field: False for field in STABLE_FIELDS}
    first = runs[0]
    return {field: all(run[field] == first[field] for run in runs[1:])
            for field in STABLE_FIELDS}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-console", type=control.existing_file, required=True)
    parser.add_argument("--candidate-console", type=control.existing_file, required=True)
    parser.add_argument("--rompath", type=control.existing_dir, required=True)
    parser.add_argument("--nvram-seed", type=control.existing_file, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    output = args.out.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)
    binaries = {
        "reference": args.reference_console,
        "candidate": args.candidate_console,
    }
    runs: dict[str, list[dict[str, object]]] = {"reference": [], "candidate": []}
    pairs: list[dict[str, object]] = []

    try:
        for index, first in enumerate(ABBA, start=1):
            second = "candidate" if first == "reference" else "reference"
            pair_dir = output / f"pair-{index:02d}-{first}-first"
            pair_dir.mkdir()
            results: dict[str, dict[str, object]] = {}
            for implementation in (first, second):
                result = control.run_host(
                    binaries[implementation], implementation, pair_dir,
                    args.rompath, args.nvram_seed,
                )
                results[implementation] = result
                runs[implementation].append(result)
            checks = pair_checks(results["reference"], results["candidate"])
            pairs.append({
                "index": index,
                "order": [first, second],
                "passed": all(checks.values()),
                "checks": checks,
                "reference": results["reference"],
                "candidate": results["candidate"],
            })
    except (OSError, RuntimeError) as error:
        print(f"ERROR control_abba_repeatability {error}")
        return 2

    repeatability = {name: stable_across_runs(records) for name, records in runs.items()}
    passed = all(pair["passed"] for pair in pairs) and all(
        all(checks.values()) for checks in repeatability.values()
    )
    receipt = {
        "schema": 1,
        "passed": passed,
        "schedule": list(ABBA),
        "inputs": {
            "reference_console": {
                "path": str(args.reference_console),
                "sha256": control.sha256(args.reference_console),
            },
            "candidate_console": {
                "path": str(args.candidate_console),
                "sha256": control.sha256(args.candidate_console),
            },
            "nvram_seed": {
                "path": str(args.nvram_seed),
                "sha256": control.sha256(args.nvram_seed),
            },
            "rompath": str(args.rompath),
        },
        "pairs": pairs,
        "repeatability": repeatability,
    }
    receipt_path = output / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
    for pair in pairs:
        print(f"{'PASS' if pair['passed'] else 'FAIL'} pair-{pair['index']} "
              f"order={'-then-'.join(pair['order'])}")
    for implementation, checks in repeatability.items():
        for field, value in checks.items():
            print(f"{'PASS' if value else 'FAIL'} repeat-{implementation}-{field}")
    print(f"receipt: {receipt_path}")
    print("CONTROL_ABBA_PASS" if passed else "CONTROL_ABBA_FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    raise SystemExit(main())
