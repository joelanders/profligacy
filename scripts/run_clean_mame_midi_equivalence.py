#!/usr/bin/env python3
"""Run identical timestamped MIDI fixtures through reference and clean MAME.

This is an equivalence gate, not a golden generator. Both hosts must pass the
existing firmware timing checks, and their complete timestamped MIDI OUT JSONL
streams plus analyzed result objects must be identical. Any mismatch stays red.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
import run_arp_timing_suite as arp  # noqa: E402


SUITES = ("subdivision", "speed", "block", "external", "pattern", "controls",
          "keysync", "drift", "stress", "audio")


def existing_file(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"file not found: {path}")
    return path


def existing_dir(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_dir():
        raise argparse.ArgumentTypeError(f"directory not found: {path}")
    return path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def first_jsonl_mismatch(left: Path, right: Path) -> dict[str, object] | None:
    left_lines = left.read_text(encoding="utf-8").splitlines()
    right_lines = right.read_text(encoding="utf-8").splitlines()
    common = min(len(left_lines), len(right_lines))
    mismatch = next((index for index in range(common)
                     if left_lines[index] != right_lines[index]), None)
    if mismatch is None and len(left_lines) == len(right_lines):
        return None
    if mismatch is None:
        mismatch = common
    return {
        "index": mismatch,
        "reference_count": len(left_lines),
        "candidate_count": len(right_lines),
        "reference": json.loads(left_lines[mismatch]) if mismatch < len(left_lines) else None,
        "candidate": json.loads(right_lines[mismatch]) if mismatch < len(right_lines) else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-console", type=existing_file, required=True)
    parser.add_argument("--candidate-console", type=existing_file, required=True)
    parser.add_argument("--rompath", type=existing_dir, required=True)
    parser.add_argument("--nvram-seed", type=existing_file, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--suite", choices=SUITES, default="subdivision")
    parser.add_argument("--case", help="run one generated case by exact name")
    args = parser.parse_args()

    output = args.out.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)
    reference_root = output / "reference"
    candidate_root = output / "candidate"
    reference_root.mkdir()
    candidate_root.mkdir()

    cases = arp.cases_for(args.suite)
    if args.case:
        cases = [case for case in cases if case.name == args.case]
        if not cases:
            parser.error(f"unknown case {args.case!r} for {args.suite}")

    reference_records: list[dict[str, object]] = []
    candidate_records: list[dict[str, object]] = []
    comparisons: list[dict[str, object]] = []
    failures: list[str] = []

    for index, case in enumerate(cases, 1):
        print(f"[{index}/{len(cases)}] {case.name}: reference", flush=True)
        try:
            reference = arp.run_case(case, reference_root, args.rompath,
                                     args.nvram_seed, args.reference_console)
            print(f"[{index}/{len(cases)}] {case.name}: candidate", flush=True)
            candidate = arp.run_case(case, candidate_root, args.rompath,
                                     args.nvram_seed, args.candidate_console)
        except (OSError, RuntimeError, ValueError) as error:
            print(f"ERROR {case.name}: {error}", file=sys.stderr)
            return 2

        reference_records.append(reference)
        candidate_records.append(candidate)
        reference_midi = reference_root / case.name / "midi.jsonl"
        candidate_midi = candidate_root / case.name / "midi.jsonl"
        midi_mismatch = first_jsonl_mismatch(reference_midi, candidate_midi)
        result_identical = reference["result"] == candidate["result"]
        reference_passed = reference["verdict"] == "PASS"
        candidate_passed = candidate["verdict"] == "PASS"
        passed = (reference_passed and candidate_passed and
                  midi_mismatch is None and result_identical)
        if not passed:
            failures.append(case.name)
        comparisons.append({
            "case": case.name,
            "passed": passed,
            "reference_verdict": reference["verdict"],
            "candidate_verdict": candidate["verdict"],
            "midi_identical": midi_mismatch is None,
            "midi_first_mismatch": midi_mismatch,
            "result_identical": result_identical,
            "midi_sha256": {
                "reference": sha256(reference_midi),
                "candidate": sha256(candidate_midi),
            },
        })
        print(("  PASS" if passed else "  FAIL") +
              f" midi_identical={midi_mismatch is None} result_identical={result_identical}")

    ref_cross_passed, ref_cross_failed = arp.cross_case_checks(
        args.suite, cases, reference_records)
    cand_cross_passed, cand_cross_failed = arp.cross_case_checks(
        args.suite, cases, candidate_records)
    cross_identical = (ref_cross_passed == cand_cross_passed and
                       ref_cross_failed == cand_cross_failed)
    if ref_cross_failed or cand_cross_failed or not cross_identical:
        failures.append("cross_case")

    receipt = {
        "schema": 1,
        "suite": args.suite,
        "case_filter": args.case,
        "passed": not failures,
        "failures": failures,
        "inputs": {
            "reference_console": {"path": str(args.reference_console),
                                  "sha256": sha256(args.reference_console)},
            "candidate_console": {"path": str(args.candidate_console),
                                  "sha256": sha256(args.candidate_console)},
            "nvram_seed": {"path": str(args.nvram_seed),
                           "sha256": sha256(args.nvram_seed)},
            "rompath": str(args.rompath),
        },
        "comparisons": comparisons,
        "cross_case": {
            "reference_passed": ref_cross_passed,
            "reference_failed": ref_cross_failed,
            "candidate_passed": cand_cross_passed,
            "candidate_failed": cand_cross_failed,
            "identical": cross_identical,
        },
    }
    receipt_path = output / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
    print(f"receipt: {receipt_path}")
    print("MIDI_EQUIVALENCE_PASS" if not failures else "MIDI_EQUIVALENCE_FAIL")
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
