#!/usr/bin/env python3
"""Adversarial tests for fail-closed receipt carry-forward."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import carry_forward_receipt as carry


def run(cwd: Path, *args: str) -> str:
    return subprocess.run(
        list(args), cwd=cwd, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=True,
    ).stdout.strip()


def configure(repo: Path) -> None:
    run(repo, "git", "init", "-q")
    run(repo, "git", "config", "user.name", "Test")
    run(repo, "git", "config", "user.email", "test@example.invalid")


class CarryForwardTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.dependency = self.root / "plugin" / "extern" / "mame"
        self.dependency.mkdir(parents=True)
        configure(self.dependency)
        (self.dependency / "PUBLIC_ACCEPTANCE.md").write_text("old\n")
        (self.dependency / "src" / "devices").mkdir(parents=True)
        (self.dependency / "src" / "devices" / "runtime.cpp").write_text("runtime\n")
        run(self.dependency, "git", "add", ".")
        run(self.dependency, "git", "commit", "-qm", "base")
        self.old_mame = run(self.dependency, "git", "rev-parse", "HEAD")

        self.repo = self.root / "plugin"
        configure(self.repo)
        (self.repo / "README.md").write_text("old\n")
        run(self.repo, "git", "add", "README.md", "extern/mame")
        run(self.repo, "git", "commit", "-qm", "old root")
        self.old_tree = run(self.repo, "git", "rev-parse", "HEAD^{tree}")

        self.evidence = self.repo / ".release-evidence"
        self.evidence.mkdir()
        artifact = self.evidence / "gate" / "raw.log"
        artifact.parent.mkdir()
        artifact.write_text("measured\n")
        self.receipt = self.evidence / "gate.json"
        self.receipt.write_text(json.dumps({
            "schema": 1,
            "gate": "gate",
            "status": "PASS",
            "binding": {
                "plugin_tree": self.old_tree,
                "dependencies": {"extern/mame": self.old_mame},
            },
            "artifacts": [{
                "path": "gate/raw.log",
                "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(),
            }],
        }))

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def advance_non_runtime(self) -> dict:
        (self.dependency / "PUBLIC_ACCEPTANCE.md").write_text("new\n")
        run(self.dependency, "git", "commit", "-qam", "docs")
        new_mame = run(self.dependency, "git", "rev-parse", "HEAD")
        (self.repo / "README.md").write_text("new\n")
        run(self.repo, "git", "add", "README.md", "extern/mame")
        run(self.repo, "git", "commit", "-qm", "public wording")
        return {
            "required_evidence": ["gate"],
            "dependencies": {"extern/mame": new_mame},
        }

    def test_exact_non_runtime_delta_is_accepted(self) -> None:
        manifest = self.advance_non_runtime()
        receipt, attestation, _, _, _ = carry.prepare_carry_forward(
            self.repo, manifest, self.evidence, self.receipt,
        )
        self.assertEqual("PASS", receipt["status"])
        self.assertEqual(["README.md", "extern/mame"], attestation["plugin_changed_paths"])
        self.assertEqual(
            ["PUBLIC_ACCEPTANCE.md"],
            attestation["dependency_changed_paths"]["extern/mame"],
        )

    def test_plugin_runtime_change_is_rejected(self) -> None:
        manifest = self.advance_non_runtime()
        (self.repo / "src").mkdir()
        (self.repo / "src" / "runtime.cpp").write_text("changed\n")
        run(self.repo, "git", "add", "src/runtime.cpp")
        run(self.repo, "git", "commit", "-qm", "runtime")
        with self.assertRaisesRegex(RuntimeError, "src/runtime.cpp"):
            carry.prepare_carry_forward(self.repo, manifest, self.evidence, self.receipt)

    def test_mame_runtime_change_is_rejected(self) -> None:
        manifest = self.advance_non_runtime()
        runtime = self.dependency / "src" / "devices" / "runtime.cpp"
        runtime.write_text("changed\n")
        run(self.dependency, "git", "commit", "-qam", "runtime")
        manifest["dependencies"]["extern/mame"] = run(
            self.dependency, "git", "rev-parse", "HEAD",
        )
        run(self.repo, "git", "add", "extern/mame")
        run(self.repo, "git", "commit", "-qm", "runtime pin")
        with self.assertRaisesRegex(RuntimeError, "src/devices/runtime.cpp"):
            carry.prepare_carry_forward(self.repo, manifest, self.evidence, self.receipt)

    def test_missing_old_tree_is_rejected(self) -> None:
        manifest = self.advance_non_runtime()
        document = json.loads(self.receipt.read_text())
        document["binding"]["plugin_tree"] = "f" * 40
        self.receipt.write_text(json.dumps(document))
        with self.assertRaisesRegex(RuntimeError, "cat-file"):
            carry.prepare_carry_forward(self.repo, manifest, self.evidence, self.receipt)

    def test_changed_retained_artifact_is_rejected(self) -> None:
        manifest = self.advance_non_runtime()
        (self.evidence / "gate" / "raw.log").write_text("tampered\n")
        with self.assertRaisesRegex(RuntimeError, "hash mismatch"):
            carry.prepare_carry_forward(self.repo, manifest, self.evidence, self.receipt)

    def test_policy_is_exact_not_prefix_based(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "README.md.backup"):
            carry.require_reviewed(
                ["README.md.backup"], carry.PLUGIN_NON_RUNTIME_PATHS, "plugin",
            )

    def test_policy_cannot_carry_change_to_itself(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "scripts/carry_forward_receipt.py"):
            carry.require_reviewed(
                ["scripts/carry_forward_receipt.py"],
                carry.PLUGIN_NON_RUNTIME_PATHS,
                "plugin",
            )

    def test_promotion_archives_source_before_replacing_live_receipt(self) -> None:
        manifest = self.advance_non_runtime()
        manifest_path = self.evidence / "manifest.json"
        manifest_path.write_text(json.dumps(manifest))
        source_bytes = self.receipt.read_bytes()
        source_hash = hashlib.sha256(source_bytes).hexdigest()

        result = carry.main([
            "--repo", str(self.repo),
            "--manifest", str(manifest_path),
            "--evidence-dir", str(self.evidence),
            "--source-receipt", str(self.receipt),
            "--promote",
        ])

        self.assertEqual(0, result)
        promoted = json.loads(self.receipt.read_text())
        attestation_relative = promoted["carried_forward_from"]["attestation"]
        attestation = json.loads(
            (self.evidence / attestation_relative).read_text()
        )
        archived_source = self.evidence / attestation["source_receipt"]["path"]
        self.assertEqual(source_bytes, archived_source.read_bytes())
        self.assertEqual(source_hash, attestation["source_receipt"]["sha256"])


if __name__ == "__main__":
    unittest.main()
