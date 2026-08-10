#!/usr/bin/env python3
"""Validate and promote the public ARM64 PF4 receipt into release evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import correctness_checkpoint as checkpoint
import release_preflight as preflight


ROOT = Path(__file__).resolve().parents[1]
GATE = "arm64_dynarec_interpreter_equivalence"
REQUIRED_CASES = {"boot", "note", "midfb", "cmem", "dense"}
REQUIRED_CHECKS = {
    "shipping-default",
    "forced-pf4",
    "unsafe-override",
    "note-pf4",
    "note-unsafe",
    "forced-midframe",
    "cmem-deopt",
    "dense-midi",
    "boot-mode-conflict-path-exercised",
    "note-mode-conflict-path-exercised",
}
BYTE_CHECKS = REQUIRED_CHECKS - {
    "boot-mode-conflict-path-exercised",
    "note-mode-conflict-path-exercised",
}
NATIVE_RUNS = {
    "boot.default",
    "boot.pf4",
    "boot.unsafe",
    "note.pf4",
    "note.unsafe",
    "note.midfb",
    "note.cmem_deopt",
    "dense.pf4",
}
INTERPRETER_RUNS = {"boot.interp", "note.interp", "dense.interp"}


def positive_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def validate_raw(raw: Any) -> list[str]:
    if not isinstance(raw, dict):
        return ["raw receipt root is not an object"]
    errors: list[str] = []
    if raw.get("schema") != 1:
        errors.append("raw schema must be 1")
    if raw.get("status") != "PASS":
        errors.append(f"raw status is {raw.get('status')!r}, expected 'PASS'")
    if raw.get("arch") not in {"arm64", "aarch64"}:
        errors.append(f"raw architecture is {raw.get('arch')!r}, expected native ARM64")
    if set(raw.get("cases") or []) != REQUIRED_CASES:
        errors.append("raw cases must be exactly boot,note,midfb,cmem,dense")
    if raw.get("failures") != []:
        errors.append("raw failures must be an empty list")

    binary = raw.get("binary")
    digest = binary.get("sha256") if isinstance(binary, dict) else None
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
        errors.append("raw binary SHA-256 is missing or malformed")

    checks = raw.get("checks")
    if not isinstance(checks, list):
        errors.append("raw checks is not a list")
        checks = []
    named = {
        row.get("name"): row
        for row in checks
        if isinstance(row, dict) and isinstance(row.get("name"), str)
    }
    missing_checks = sorted(REQUIRED_CHECKS - named.keys())
    if missing_checks:
        errors.append("missing checks: " + ", ".join(missing_checks))
    for name in sorted(REQUIRED_CHECKS & named.keys()):
        if named[name].get("pass") is not True:
            errors.append(f"{name}: comparison did not pass")
        if name in BYTE_CHECKS and named[name].get("first_difference") is not None:
            errors.append(f"{name}: byte comparison has a first difference")

    runs = raw.get("runs")
    if not isinstance(runs, dict):
        errors.append("raw runs is not an object")
        runs = {}
    for name in sorted(NATIVE_RUNS):
        run = runs.get(name)
        if not isinstance(run, dict):
            errors.append(f"missing native run: {name}")
            continue
        if not positive_integer(run.get("max_runs")):
            errors.append(f"{name}: no native pooled runs")
        if not positive_integer(run.get("max_compiles")):
            errors.append(f"{name}: no native pooled compiles")
        sizes = run.get("pooled_frame_sizes")
        if not isinstance(sizes, list) or not sizes:
            errors.append(f"{name}: no native pooled frame sizes")
    midframe = runs.get("note.midfb")
    if isinstance(midframe, dict) and not positive_integer(midframe.get("max_forced_midframe")):
        errors.append("note.midfb: forced-midframe path recorded no hits")
    for name in sorted(INTERPRETER_RUNS):
        run = runs.get(name)
        if not isinstance(run, dict):
            errors.append(f"missing interpreter run: {name}")
            continue
        if run.get("max_runs") != 0 or run.get("max_compiles") != 0:
            errors.append(f"{name}: interpreter oracle executed pooled code")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument(
        "--manifest", type=Path, default=ROOT / "release/v1_alpha_preflight.json"
    )
    parser.add_argument("--evidence-dir", type=Path, default=ROOT / ".release-evidence")
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    try:
        repo = args.repo.resolve()
        evidence_dir = args.evidence_dir.resolve()
        raw_path = args.raw.resolve()
        raw_path.relative_to(evidence_dir)
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        if GATE not in manifest.get("required_evidence", []):
            raise ValueError(f"{GATE} is absent from the selected manifest")
        blockers = checkpoint.source_blockers(repo, manifest, evidence_dir)
        if blockers:
            raise ValueError("; ".join(blockers))
        raw = json.loads(raw_path.read_text(encoding="utf-8"))
        errors = validate_raw(raw)
        if errors:
            raise ValueError("; ".join(errors))
        output = (
            args.output.resolve()
            if args.output
            else evidence_dir / f"{GATE}.json"
        )
        output.relative_to(evidence_dir)
        artifact = str(raw_path.relative_to(evidence_dir))
        receipt = {
            "schema": 1,
            "gate": GATE,
            "status": "PASS",
            "binding": preflight.source_binding(repo, manifest["dependencies"]),
            "artifacts": [{"path": artifact, "sha256": preflight.sha256(raw_path)}],
        }
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"ARM64 RECEIPT PROMOTION REFUSED: {exc}", file=sys.stderr)
        return 2
    print(f"PASS promoted {GATE}: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
