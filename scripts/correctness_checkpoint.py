#!/usr/bin/env python3
"""Report resumable, exact-source correctness evidence for the release candidate.

This command never runs a gate and never promotes evidence.  It answers the
question that matters before an expensive run: does an existing PASS receipt
still bind to this committed plugin tree, these dependency gitlinks, and its
retained artifacts?  A dirty tree or mismatched dependency checkout blocks the
answer instead of risking evidence for uncommitted inputs.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_preflight as preflight


ROOT = Path(__file__).resolve().parents[1]

CORRECTNESS_GATES = (
    "dsp_hardware_corpus_554",
    "arm64_dynarec_interpreter_equivalence",
    "multi_instance_contract",
    "plugin_rt_safety",
    "control_abba_repeatability",
    "pcm_matrix",
    "midi_matrix",
    "engine_lifecycle",
    "clean_mame_performance",
)


def classify_receipt(
    path: Path,
    gate: str,
    binding: dict[str, Any],
    evidence_dir: Path,
) -> tuple[str, str]:
    if not path.is_file():
        return "MISSING", f"missing receipt: {path}"
    try:
        receipt = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return "FAIL", f"unreadable receipt: {exc}"

    errors = preflight.validate_receipt(receipt, gate, binding, evidence_dir)
    if not errors:
        return "PASS", "exact binding and retained artifacts"
    if errors == ["source/dependency binding is stale or different"]:
        return "STALE", errors[0]
    return "FAIL", "; ".join(errors)


def source_blockers(
    repo: Path,
    manifest: dict[str, Any],
    evidence_dir: Path,
) -> list[str]:
    status = preflight.git(repo, "status", "--porcelain", "--untracked-files=all")
    ignored = evidence_dir.resolve()
    dirty: list[str] = []
    for row in status.splitlines():
        relative = row[3:]
        candidate = (repo / relative).resolve()
        if candidate == ignored or ignored in candidate.parents:
            continue
        dirty.append(row)
    if dirty:
        return ["tracked or non-evidence worktree changes exist: " + ", ".join(dirty)]

    blockers: list[str] = []
    binding = preflight.source_binding(repo, manifest["dependencies"])
    for path, expected in manifest["dependencies"].items():
        gitlink = binding["dependencies"].get(path)
        if gitlink != expected:
            blockers.append(f"{path}: gitlink {gitlink}, expected {expected}")
            continue
        checkout = repo / path
        if not (checkout / ".git").exists():
            blockers.append(f"{path}: checkout is not initialized")
            continue
        actual = preflight.git(checkout, "rev-parse", "HEAD")
        if actual != expected:
            blockers.append(f"{path}: checkout {actual}, expected {expected}")
        if preflight.git(checkout, "status", "--porcelain", "--untracked-files=no"):
            blockers.append(f"{path}: tracked checkout is dirty")
    return blockers


def evaluate(
    repo: Path,
    manifest: dict[str, Any],
    evidence_dir: Path,
) -> list[dict[str, str]]:
    required = set(manifest["required_evidence"])
    absent = [gate for gate in CORRECTNESS_GATES if gate not in required]
    if absent:
        raise ValueError(
            "correctness gate(s) absent from release manifest: " + ", ".join(absent)
        )
    binding = preflight.source_binding(repo, manifest["dependencies"])
    rows: list[dict[str, str]] = []
    for gate in CORRECTNESS_GATES:
        state, detail = classify_receipt(
            evidence_dir / f"{gate}.json", gate, binding, evidence_dir
        )
        rows.append({"gate": gate, "status": state, "detail": detail})
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument(
        "--manifest", type=Path, default=ROOT / "release/v1_alpha_preflight.json"
    )
    parser.add_argument("--evidence-dir", type=Path, default=ROOT / ".release-evidence")
    parser.add_argument("--json-output", type=Path)
    parser.add_argument(
        "--next", action="store_true",
        help="print only the first non-PASS gate after validation",
    )
    args = parser.parse_args(argv)

    try:
        repo = args.repo.resolve()
        evidence_dir = args.evidence_dir.resolve()
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        if manifest.get("schema") != 1:
            raise ValueError("manifest schema must be 1")
        blockers = source_blockers(repo, manifest, evidence_dir)
        if blockers:
            for blocker in blockers:
                print(f"BLOCKED  {blocker}", file=sys.stderr)
            return 2
        rows = evaluate(repo, manifest, evidence_dir)
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"CORRECTNESS CHECKPOINT ERROR: {exc}", file=sys.stderr)
        return 2

    pending = [row for row in rows if row["status"] != "PASS"]
    shown = pending[:1] if args.next else rows
    for row in shown:
        print(f"{row['status']:7}  {row['gate']}: {row['detail']}")
    passed = len(rows) - len(pending)
    print(f"\nCORRECTNESS {passed}/{len(rows)} exact receipts PASS")
    if args.json_output:
        args.json_output.write_text(
            json.dumps({"passed": not pending, "checks": rows}, indent=2) + "\n",
            encoding="utf-8",
        )
    return 0 if not pending else 1


if __name__ == "__main__":
    raise SystemExit(main())
