#!/usr/bin/env python3
"""Apply final PASS/FAIL/INCONCLUSIVE grading to a captured v2 receipt."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_clean_mame_performance_equivalence as gate


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sanitized_receipt(receipt: dict[str, object], source: Path) -> dict[str, object]:
    """Replace machine-local paths while preserving hashes, commands, and metrics."""
    sanitized = copy.deepcopy(receipt)
    inputs = sanitized["inputs"]
    replacements = {
        inputs["reference"]["path"]: "reference-host",
        inputs["candidate"]["path"]: "candidate-host",
        inputs["nvram_seed"]["path"]: "settled-nvram-seed",
        inputs["rompath"]: "user-rom-directory",
        str(source.resolve()): "raw_run_receipt.json",
    }
    rows = list(sanitized.get("runs", []))
    control = sanitized.get("identical_binary_control")
    if control is not None:
        rows.extend(control.get("runs", []))
    logs = [row["log"] for row in rows if row.get("log")]
    if logs:
        run_root = str(Path(logs[0]).parents[2])
        replacements[run_root] = "."
    ordered_replacements = sorted(replacements.items(), key=lambda item: len(item[0]), reverse=True)

    def replace(value: object) -> object:
        if isinstance(value, dict):
            return {key: replace(item) for key, item in value.items()}
        if isinstance(value, list):
            return [replace(item) for item in value]
        if isinstance(value, str):
            for local, stable in ordered_replacements:
                if value == local:
                    return stable
                if value.startswith(local + "/"):
                    return stable + value[len(local):]
        return value

    sanitized = replace(sanitized)

    local_paths: list[str] = []

    def find_local(value: object) -> None:
        if isinstance(value, dict):
            for item in value.values():
                find_local(item)
        elif isinstance(value, list):
            for item in value:
                find_local(item)
        elif isinstance(value, str) and (value.startswith("/Users/") or value.startswith("/private/tmp/")):
            local_paths.append(value)

    find_local(sanitized)
    if local_paths:
        raise ValueError(f"unmapped machine-local receipt paths: {sorted(set(local_paths))}")
    return sanitized


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--sanitize-paths", action="store_true",
        help="replace machine-local paths with stable evidence labels",
    )
    args = parser.parse_args()
    if not args.input.is_file():
        parser.error(f"input receipt not found: {args.input}")
    if args.output.exists():
        parser.error(f"refusing existing output: {args.output}")

    receipt = json.loads(args.input.read_text(encoding="utf-8"))
    if receipt.get("schema") != 2:
        parser.error("offline regrade requires a schema-2 performance receipt")
    if "absolute_realtime_health" not in receipt.get("acceptance", {}):
        parser.error("receipt lacks v2 absolute real-time health data")
    graded = gate.regrade_receipt(receipt)
    graded["offline_regrade"] = {
        "source": str(args.input.resolve()),
        "source_sha256": sha256(args.input),
        "policy": "three-state candidate-health and evidence-validity grading",
    }
    if args.sanitize_paths:
        graded = sanitized_receipt(graded, args.input)
        graded["offline_regrade"]["paths_sanitized"] = True
    args.output.write_text(json.dumps(graded, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"{graded['verdict']} regraded_receipt={args.output}")
    for reason in graded["acceptance"]["comparative_status"]["reasons"]:
        print(f"  inconclusive: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
