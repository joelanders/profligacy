#!/usr/bin/env python3
"""Fail-closed Profligacy v1 release preflight.

The preflight does not perform GUI or audio-host testing.  It verifies that
machine-produced receipts for those gates exist, passed, and were generated
from the exact source tree and dependency commits being released.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


ROOT = Path(__file__).resolve().parents[1]


def command(args: list[str], cwd: Path, *, check: bool = True) -> str:
    result = subprocess.run(
        args, cwd=cwd, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False,
    )
    if check and result.returncode:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"{' '.join(args)} failed: {detail}")
    return result.stdout.strip()


def git(repo: Path, *args: str, check: bool = True) -> str:
    return command(["git", *args], repo, check=check)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def dependency_gitlinks(repo: Path, paths: list[str]) -> dict[str, str | None]:
    links: dict[str, str | None] = {}
    for path in paths:
        row = git(repo, "ls-tree", "HEAD", "--", path)
        fields = row.split()
        links[path] = fields[2] if len(fields) >= 4 and fields[1] == "commit" else None
    return links


def source_binding(repo: Path, dependencies: dict[str, str]) -> dict[str, Any]:
    return {
        "plugin_tree": git(repo, "rev-parse", "HEAD^{tree}"),
        "dependencies": dependency_gitlinks(repo, list(dependencies)),
    }


def missing_patch_equivalents(
    repo: Path, accepted_ref: str, shipping_paths: list[str]
) -> list[str]:
    missing: list[str] = []
    # Compare each path independently.  This still fails closed on an omitted
    # accepted patch, but permits a public commit to carry the same source
    # patch plus disjoint release-only wiring in another shipping path.
    for path in shipping_paths:
        result = subprocess.run(
            [
                "git", "log", "--cherry-pick", "--right-only", "--no-merges",
                "--format=%H %s", f"HEAD...{accepted_ref}", "--", path,
            ],
            cwd=repo, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            check=False,
        )
        # A missing/private accepted ref is itself a failure, never a silent skip.
        if result.returncode:
            detail = result.stderr.strip() or "accepted source ref is unavailable"
            return [detail]
        missing.extend(
            f"{path}: {line}" for line in result.stdout.splitlines() if line.strip()
        )
    return missing


def tracked_files(repo: Path, prefixes: list[str] | None = None) -> list[Path]:
    output = git(repo, "ls-files", "-z")
    items = [item for item in output.split("\0") if item]
    if prefixes is not None:
        items = [
            item for item in items
            if any(item == prefix or item.startswith(prefix.rstrip("/") + "/")
                   for prefix in prefixes)
        ]
    return [repo / item for item in items]


def scan_forbidden_text(
    repo: Path, patterns: list[str], prefixes: list[str] | None = None
) -> list[str]:
    findings: list[str] = []
    for path in tracked_files(repo, prefixes):
        if not path.is_file() or path.stat().st_size > 5 * 1024 * 1024:
            continue
        data = path.read_bytes()
        if b"\0" in data:
            continue
        text = data.decode("utf-8", errors="replace")
        for pattern in patterns:
            if pattern in text:
                findings.append(f"{path.relative_to(repo)}: {pattern!r}")
    return findings


def fresh_root_errors(repo: Path, contract: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    head = git(repo, "rev-parse", "HEAD")
    count = git(repo, "rev-list", "--count", "HEAD")
    if count != "1":
        errors.append(f"history has {count} commits, expected one root commit")

    identity = git(repo, "show", "-s", "--format=%ae%n%ce", "HEAD").splitlines()
    expected_email = str(contract["author_email"])
    if identity != [expected_email, expected_email]:
        errors.append(
            f"author/committer email is {identity!r}, expected {expected_email!r}"
        )

    tracked = {
        item for item in git(repo, "ls-files", "-z").split("\0") if item
    }
    for forbidden in contract["forbidden_tracked_paths"]:
        if any(
            item == forbidden or item.startswith(forbidden.rstrip("/") + "/")
            for item in tracked
        ):
            errors.append(f"internal path is tracked: {forbidden}")

    refs = git(repo, "for-each-ref", "--format=%(objectname) %(refname)")
    foreign_refs = [line for line in refs.splitlines() if not line.startswith(head + " ")]
    if foreign_refs:
        errors.append("refs retain objects outside the public root: " + ", ".join(foreign_refs))

    fsck = subprocess.run(
        ["git", "fsck", "--unreachable", "--no-reflogs"],
        cwd=repo, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False,
    )
    unreachable = [
        line for line in (fsck.stdout + "\n" + fsck.stderr).splitlines()
        if line.startswith("unreachable ")
    ]
    if fsck.returncode:
        errors.append(f"git fsck failed with exit {fsck.returncode}")
    if unreachable:
        errors.append(f"repository retains {len(unreachable)} unreachable objects")
    return errors


def ctest_names(build_dir: Path) -> set[str]:
    raw = command(["ctest", "--show-only=json-v1"], build_dir)
    document = json.loads(raw)
    return {test["name"] for test in document.get("tests", [])}


def validate_receipt(
    receipt: Any,
    gate: str,
    binding: dict[str, Any],
    evidence_dir: Path,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(receipt, dict):
        return ["receipt root is not an object"]
    if receipt.get("schema") != 1:
        errors.append("schema must be 1")
    if receipt.get("gate") != gate:
        errors.append(f"gate is {receipt.get('gate')!r}, expected {gate!r}")
    if receipt.get("status") != "PASS":
        errors.append(f"status is {receipt.get('status')!r}, expected 'PASS'")
    if receipt.get("binding") != binding:
        errors.append("source/dependency binding is stale or different")

    artifacts = receipt.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        errors.append("at least one hashed artifact is required")
        return errors
    for index, artifact in enumerate(artifacts):
        if not isinstance(artifact, dict):
            errors.append(f"artifact {index} is not an object")
            continue
        relative = artifact.get("path")
        expected = artifact.get("sha256")
        if not isinstance(relative, str) or not relative:
            errors.append(f"artifact {index} has no path")
            continue
        path = (evidence_dir / relative).resolve()
        try:
            path.relative_to(evidence_dir.resolve())
        except ValueError:
            errors.append(f"artifact {index} escapes evidence directory")
            continue
        if not path.is_file():
            errors.append(f"artifact {relative!r} is missing")
        elif not isinstance(expected, str) or sha256(path) != expected:
            errors.append(f"artifact {relative!r} hash mismatch")
    return errors


def evaluate(
    repo: Path,
    manifest: dict[str, Any],
    evidence_dir: Path,
    build_dir: Path | None,
    phase: str,
) -> list[dict[str, str]]:
    checks: list[dict[str, str]] = []

    def record(name: str, errors: list[str]) -> None:
        checks.append({
            "name": name,
            "status": "PASS" if not errors else "FAIL",
            "detail": "ok" if not errors else "; ".join(errors),
        })

    status = git(repo, "status", "--porcelain", "--untracked-files=all")
    ignored_prefix = ".release-evidence/"
    dirty = [line for line in status.splitlines() if line[3:] != ".release-evidence"
             and not line[3:].startswith(ignored_prefix)]
    record("clean_worktree", dirty)

    if phase == "fresh-root":
        record("fresh_root_topology", fresh_root_errors(repo, manifest["fresh_root"]))

    dependencies = manifest["dependencies"]
    binding = source_binding(repo, dependencies)
    dependency_errors = [
        f"{path}: gitlink {binding['dependencies'].get(path)}, expected {expected}"
        for path, expected in dependencies.items()
        if binding["dependencies"].get(path) != expected
    ]
    record("exact_dependency_gitlinks", dependency_errors)

    checkout_errors: list[str] = []
    for path, expected in dependencies.items():
        directory = repo / path
        if not (directory / ".git").exists():
            checkout_errors.append(f"{path}: not initialized")
            continue
        actual = git(directory, "rev-parse", "HEAD")
        if actual != expected:
            checkout_errors.append(f"{path}: checkout {actual}, expected {expected}")
        sub_status = git(directory, "status", "--porcelain", "--untracked-files=no")
        if sub_status:
            checkout_errors.append(f"{path}: tracked checkout is dirty")
    record("exact_dependency_checkouts", checkout_errors)

    if phase == "integration":
        missing = missing_patch_equivalents(
            repo, manifest["accepted_source_ref"], manifest["shipping_paths"]
        )
        record("accepted_source_patch_equivalence", missing)

    modules = (repo / ".gitmodules").read_text(encoding="utf-8")
    private = [fragment for fragment in manifest["forbidden_submodule_url_fragments"]
               if fragment in modules]
    record("public_submodule_urls", [f"forbidden fragment {item!r}" for item in private])

    forbidden = scan_forbidden_text(
        repo, manifest["forbidden_tracked_text"], manifest["asset_scan_paths"]
    )
    record("forbidden_asset_scan", forbidden)

    if build_dir is None:
        record("mandatory_ctest_inventory", ["--build-dir was not supplied"])
    elif not build_dir.is_dir():
        record("mandatory_ctest_inventory", [f"build directory is missing: {build_dir}"])
    else:
        try:
            names = ctest_names(build_dir)
            missing_tests = sorted(set(manifest["mandatory_ctests"]) - names)
            record("mandatory_ctest_inventory", [f"missing test: {name}" for name in missing_tests])
        except (RuntimeError, json.JSONDecodeError) as exc:
            record("mandatory_ctest_inventory", [str(exc)])

    for gate in manifest["required_evidence"]:
        path = evidence_dir / f"{gate}.json"
        if not path.is_file():
            record(f"evidence:{gate}", [f"missing receipt: {path}"])
            continue
        try:
            receipt = json.loads(path.read_text(encoding="utf-8"))
            errors = validate_receipt(receipt, gate, binding, evidence_dir)
        except (OSError, json.JSONDecodeError) as exc:
            errors = [f"unreadable receipt: {exc}"]
        record(f"evidence:{gate}", errors)

    return checks


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=ROOT)
    parser.add_argument("--manifest", type=Path, default=ROOT / "release/v1_preflight.json")
    parser.add_argument("--evidence-dir", type=Path, default=ROOT / ".release-evidence")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--phase", choices=("integration", "fresh-root"), default="integration")
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args(argv)

    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        if manifest.get("schema") != 1:
            raise ValueError("manifest schema must be 1")
        checks = evaluate(
            args.repo.resolve(), manifest, args.evidence_dir.resolve(),
            args.build_dir.resolve() if args.build_dir else None, args.phase,
        )
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError) as exc:
        print(f"PREFLIGHT ERROR: {exc}", file=sys.stderr)
        return 2

    passed = all(check["status"] == "PASS" for check in checks)
    for check in checks:
        print(f"{check['status']:4}  {check['name']}: {check['detail']}")
    print(f"\nPREFLIGHT {'PASS' if passed else 'FAIL'} ({sum(c['status'] == 'PASS' for c in checks)}/{len(checks)} checks passed)")
    if args.json_output:
        args.json_output.write_text(json.dumps({"passed": passed, "checks": checks}, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
