#!/usr/bin/env python3
"""Focused unit tests for the fail-closed release preflight."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_preflight as preflight


def run(*args: str, cwd: Path) -> str:
    return subprocess.run(
        list(args), cwd=cwd, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=True,
    ).stdout.strip()


class ReleasePreflightTests(unittest.TestCase):
    def test_receipt_requires_pass_exact_binding_and_real_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary)
            artifact = evidence / "gate.log"
            artifact.write_text("measured\n", encoding="utf-8")
            binding = {
                "plugin_tree": "b" * 40,
                "dependencies": {"extern/mame": "c" * 40},
            }
            receipt = {
                "schema": 1,
                "gate": "control",
                "status": "PASS",
                "binding": binding,
                "artifacts": [{
                    "path": "gate.log",
                    "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
                }],
            }
            self.assertEqual([], preflight.validate_receipt(receipt, "control", binding, evidence))

            stale = dict(receipt)
            stale["binding"] = {**binding, "plugin_tree": "d" * 40}
            self.assertTrue(preflight.validate_receipt(stale, "control", binding, evidence))

            failed = dict(receipt)
            failed["status"] = "INCONCLUSIVE"
            self.assertTrue(preflight.validate_receipt(failed, "control", binding, evidence))

            artifact.write_text("changed\n", encoding="utf-8")
            self.assertTrue(preflight.validate_receipt(receipt, "control", binding, evidence))

    def test_receipt_rejects_artifact_path_escape(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "evidence"
            evidence.mkdir()
            outside = Path(temporary) / "outside.log"
            outside.write_text("x", encoding="utf-8")
            binding = {"plugin_tree": "t", "dependencies": {}}
            receipt = {
                "schema": 1, "gate": "x", "status": "PASS", "binding": binding,
                "artifacts": [{"path": "../outside.log", "sha256": preflight.sha256(outside)}],
            }
            errors = preflight.validate_receipt(receipt, "x", binding, evidence)
            self.assertTrue(any("escapes" in error for error in errors))

    def test_patch_equivalence_detects_omission_and_accepts_cherry_pick(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            run("git", "init", "-q", cwd=repo)
            run("git", "config", "user.name", "Test", cwd=repo)
            run("git", "config", "user.email", "test@example.invalid", cwd=repo)
            source = repo / "src"
            source.mkdir()
            file = source / "shipping.cpp"
            file.write_text("one\n", encoding="utf-8")
            run("git", "add", ".", cwd=repo)
            run("git", "commit", "-qm", "base", cwd=repo)
            base = run("git", "rev-parse", "HEAD", cwd=repo)

            file.write_text("one\ntwo\n", encoding="utf-8")
            run("git", "commit", "-qam", "accepted fix", cwd=repo)
            accepted = run("git", "rev-parse", "HEAD", cwd=repo)
            run("git", "checkout", "-q", "-b", "candidate", base, cwd=repo)
            self.assertTrue(preflight.missing_patch_equivalents(repo, accepted, ["src"]))

            file.write_text("one\ntwo\n", encoding="utf-8")
            run("git", "commit", "-qam", "same patch, different commit", cwd=repo)
            self.assertEqual([], preflight.missing_patch_equivalents(repo, accepted, ["src"]))

    def test_patch_equivalence_allows_disjoint_release_wiring(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            run("git", "init", "-q", cwd=repo)
            run("git", "config", "user.name", "Test", cwd=repo)
            run("git", "config", "user.email", "test@example.invalid", cwd=repo)
            source = repo / "src"
            source.mkdir()
            file = source / "shipping.cpp"
            cmake = repo / "CMakeLists.txt"
            file.write_text("one\n", encoding="utf-8")
            cmake.write_text("base\n", encoding="utf-8")
            run("git", "add", ".", cwd=repo)
            run("git", "commit", "-qm", "base", cwd=repo)
            base = run("git", "rev-parse", "HEAD", cwd=repo)

            file.write_text("one\ntwo\n", encoding="utf-8")
            run("git", "commit", "-qam", "accepted behavior", cwd=repo)
            accepted = run("git", "rev-parse", "HEAD", cwd=repo)
            run("git", "checkout", "-q", "-b", "candidate", base, cwd=repo)

            file.write_text("one\ntwo\n", encoding="utf-8")
            cmake.write_text("base\nrelease test\n", encoding="utf-8")
            run("git", "commit", "-qam", "behavior plus release wiring", cwd=repo)
            self.assertEqual(
                [], preflight.missing_patch_equivalents(
                    repo, accepted, ["src", "CMakeLists.txt"]
                ),
            )

    def test_forbidden_text_scan_is_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            run("git", "init", "-q", cwd=repo)
            (repo / "source.js").write_text("const GLYPHS = []\n", encoding="utf-8")
            run("git", "add", ".", cwd=repo)
            findings = preflight.scan_forbidden_text(repo, ["const GLYPHS"])
            self.assertEqual(["source.js: 'const GLYPHS'"], findings)

    def test_fresh_root_rejects_history_internal_paths_and_wrong_identity(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            run("git", "init", "-q", cwd=repo)
            run("git", "config", "user.name", "Joe Landers", cwd=repo)
            run("git", "config", "user.email", "joe@joelanders.net", cwd=repo)
            (repo / "README.md").write_text("public\n", encoding="utf-8")
            run("git", "add", ".", cwd=repo)
            run("git", "commit", "-qm", "root", cwd=repo)
            contract = {
                "author_email": "joe@joelanders.net",
                "forbidden_tracked_paths": ["HANDOVER.md", "notes"],
            }
            self.assertEqual([], preflight.fresh_root_errors(repo, contract))

            (repo / "HANDOVER.md").write_text("private\n", encoding="utf-8")
            run("git", "add", ".", cwd=repo)
            run("git", "commit", "-qm", "second", cwd=repo)
            errors = preflight.fresh_root_errors(repo, contract)
            self.assertTrue(any("2 commits" in error for error in errors))
            self.assertTrue(any("HANDOVER.md" in error for error in errors))

    def test_ctest_inventory_parser_does_not_treat_no_tests_as_success(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / "CTestTestfile.cmake").write_text("", encoding="utf-8")
            self.assertEqual(set(), preflight.ctest_names(build))


if __name__ == "__main__":
    unittest.main()
