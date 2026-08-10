#!/usr/bin/env python3
"""Carry a PASS receipt only across machine-proven non-runtime tree changes.

This is deliberately narrower than a general allowlist. Both the old and new
Git objects must exist locally, every retained artifact must still hash exactly,
and every changed path must be one of the hard-coded reviewed paths below.
Missing history, changed JUCE, runtime source, build configuration, or an unknown
path is a hard failure.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_preflight as preflight


ROOT = Path(__file__).resolve().parents[1]

# Maximum reviewed non-runtime delta. Expanding either set is a source change
# that must receive code review and pass the adversarial tests in the companion
# test module. No glob or prefix matching is used.
PLUGIN_NON_RUNTIME_PATHS = frozenset({
    "README.md",
    "release/GITHUB_ALPHA_RELEASE_NOTES.md",
    "release/README.md",
    "release/v1_alpha_preflight.json",
    "release/v1_preflight.json",
    "scripts/test_alpha_release_policy.py",
    "scripts/test_carry_forward_receipt.py",
    "extern/mame",
})

MAME_NON_RUNTIME_PATHS = frozenset({
    "PUBLIC_ACCEPTANCE.md",
    "PUBLIC_PROVENANCE.md",
    "scripts/korgprophecy_corpus_policy.py",
    "scripts/korgprophecy_no_rom_gate.py",
    "scripts/korgprophecy_tms57002_oracle_corpus.py",
    "src/mame/skeleton/tms57002test.cpp",
    "tests/korgprophecy/README.md",
    "tests/korgprophecy/tms57002_legacy_assets_v1.json",
    "tests/korgprophecy/tms57002_oracle_gaps_v1.json",
    "tests/korgprophecy/tms57002_retired_research_tests_v1.json",
})


def run_git(repo: Path, *args: str) -> str:
    environment = os.environ.copy()
    environment["GIT_NO_LAZY_FETCH"] = "1"
    result = subprocess.run(
        ["git", *args], cwd=repo, env=environment, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False,
    )
    if result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"git {' '.join(args)} failed in {repo}: {detail}")
    return result.stdout.strip()


def require_object(repo: Path, object_name: str, kind: str) -> None:
    suffix = "^{tree}" if kind == "tree" else "^{commit}"
    run_git(repo, "cat-file", "-e", object_name + suffix)


def changed_paths(repo: Path, old: str, new: str) -> list[str]:
    return sorted(filter(None, run_git(
        repo, "diff-tree", "-r", "--no-commit-id", "--no-renames",
        "--name-only", old, new,
    ).splitlines()))


def require_reviewed(changed: list[str], allowed: frozenset[str], label: str) -> None:
    rejected = sorted(set(changed) - allowed)
    if rejected:
        raise RuntimeError(
            f"{label} contains runtime or unreviewed path(s): {', '.join(rejected)}"
        )


def clean_source(repo: Path, evidence_dir: Path) -> None:
    dirty: list[str] = []
    evidence = evidence_dir.resolve()
    for row in preflight.git(repo, "status", "--porcelain", "--untracked-files=all").splitlines():
        relative = row[3:]
        candidate = (repo / relative).resolve()
        if candidate == evidence or evidence in candidate.parents:
            continue
        dirty.append(row)
    if dirty:
        raise RuntimeError("tracked or non-evidence worktree changes exist: " + ", ".join(dirty))


def load_json(path: Path) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise RuntimeError(f"JSON root is not an object: {path}")
    return document


def rendered(document: dict[str, Any]) -> bytes:
    return (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def prepare_carry_forward(
    repo: Path,
    manifest: dict[str, Any],
    evidence_dir: Path,
    source_receipt_path: Path,
) -> tuple[dict[str, Any], dict[str, Any], str, str, bytes]:
    repo = repo.resolve()
    evidence_dir = evidence_dir.resolve()
    source_receipt_path = source_receipt_path.resolve()
    clean_source(repo, evidence_dir)

    source_bytes = source_receipt_path.read_bytes()
    source = json.loads(source_bytes.decode("utf-8"))
    if not isinstance(source, dict):
        raise RuntimeError(f"JSON root is not an object: {source_receipt_path}")
    gate = source.get("gate")
    if not isinstance(gate, str) or gate not in manifest.get("required_evidence", []):
        raise RuntimeError(f"receipt gate is not required by this manifest: {gate!r}")
    old_binding = source.get("binding")
    if not isinstance(old_binding, dict):
        raise RuntimeError("source receipt has no binding object")
    validation = preflight.validate_receipt(
        source, gate, old_binding, evidence_dir,
    )
    if validation:
        raise RuntimeError("source receipt is not a valid retained PASS: " + "; ".join(validation))

    current_binding = preflight.source_binding(repo, manifest["dependencies"])
    if current_binding == old_binding:
        raise RuntimeError("source receipt already has the current binding")

    old_plugin_tree = old_binding.get("plugin_tree")
    new_plugin_tree = current_binding["plugin_tree"]
    if not isinstance(old_plugin_tree, str):
        raise RuntimeError("source receipt plugin tree is missing")
    require_object(repo, old_plugin_tree, "tree")
    require_object(repo, new_plugin_tree, "tree")
    plugin_changes = changed_paths(repo, old_plugin_tree, new_plugin_tree)
    require_reviewed(plugin_changes, PLUGIN_NON_RUNTIME_PATHS, "Profligacy delta")

    old_dependencies = old_binding.get("dependencies")
    new_dependencies = current_binding.get("dependencies")
    if not isinstance(old_dependencies, dict) or not isinstance(new_dependencies, dict):
        raise RuntimeError("dependency binding is missing")
    if set(old_dependencies) != set(new_dependencies):
        raise RuntimeError("dependency set changed")

    dependency_changes: dict[str, list[str]] = {}
    for dependency in sorted(new_dependencies):
        old_commit = old_dependencies[dependency]
        new_commit = new_dependencies[dependency]
        if old_commit == new_commit:
            dependency_changes[dependency] = []
            continue
        if dependency != "extern/mame":
            raise RuntimeError(f"dependency commit changed and cannot be carried: {dependency}")
        dependency_repo = repo / dependency
        require_object(dependency_repo, old_commit, "commit")
        require_object(dependency_repo, new_commit, "commit")
        paths = changed_paths(dependency_repo, old_commit, new_commit)
        require_reviewed(paths, MAME_NON_RUNTIME_PATHS, "public MAME delta")
        dependency_changes[dependency] = paths

    source_receipt_hash = sha256_bytes(source_bytes)
    source_receipt_relative = (
        f"{gate}/source-receipts/{source_receipt_hash}.json"
    )
    policy_hash = preflight.sha256(Path(__file__).resolve())
    attestation = {
        "schema": 1,
        "kind": "receipt-carry-forward-attestation",
        "gate": gate,
        "source_receipt": {
            "path": source_receipt_relative,
            "sha256": source_receipt_hash,
        },
        "old_binding": old_binding,
        "new_binding": current_binding,
        "plugin_changed_paths": plugin_changes,
        "dependency_changed_paths": dependency_changes,
        "policy_script_sha256": policy_hash,
        "verdict": "PASS_NON_RUNTIME_ONLY",
    }
    attestation_relative = (
        f"{gate}/carry-forward-{str(old_plugin_tree)[:12]}-to-"
        f"{str(new_plugin_tree)[:12]}.json"
    )
    attestation_hash = sha256_bytes(rendered(attestation))
    receipt = {
        "schema": 1,
        "gate": gate,
        "status": "PASS",
        "binding": current_binding,
        "artifacts": [
            *source["artifacts"],
            {"path": source_receipt_relative, "sha256": source_receipt_hash},
            {"path": attestation_relative, "sha256": attestation_hash},
        ],
        "carried_forward_from": {
            "receipt_sha256": source_receipt_hash,
            "attestation": attestation_relative,
        },
    }
    return (
        receipt,
        attestation,
        attestation_relative,
        source_receipt_relative,
        source_bytes,
    )


def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_bytes(data)
    temporary.replace(path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--manifest", type=Path, default=ROOT / "release/v1_alpha_preflight.json")
    parser.add_argument("--evidence-dir", type=Path, default=ROOT / ".release-evidence")
    parser.add_argument("--source-receipt", type=Path, required=True)
    parser.add_argument("--promote", action="store_true", help="write attestation and replace the gate receipt")
    args = parser.parse_args(argv)

    try:
        manifest = load_json(args.manifest)
        (
            receipt,
            attestation,
            attestation_relative,
            source_receipt_relative,
            source_receipt_bytes,
        ) = prepare_carry_forward(
            args.repo, manifest, args.evidence_dir, args.source_receipt,
        )
        if args.promote:
            evidence = args.evidence_dir.resolve()
            write_atomic(evidence / source_receipt_relative, source_receipt_bytes)
            write_atomic(evidence / attestation_relative, rendered(attestation))
            errors = preflight.validate_receipt(
                receipt, receipt["gate"], receipt["binding"], evidence,
            )
            if errors:
                raise RuntimeError("prepared receipt failed validation: " + "; ".join(errors))
            write_atomic(evidence / f"{receipt['gate']}.json", rendered(receipt))
            print(f"CARRY_FORWARD status=PASS gate={receipt['gate']} promoted=1")
        else:
            print(rendered(attestation).decode("utf-8"), end="")
            print(f"CARRY_FORWARD status=PASS gate={receipt['gate']} promoted=0", file=sys.stderr)
        return 0
    except (KeyError, OSError, RuntimeError, ValueError, json.JSONDecodeError) as error:
        print(f"CARRY_FORWARD status=FAIL error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
