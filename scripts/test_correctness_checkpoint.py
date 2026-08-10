#!/usr/bin/env python3
"""Focused tests for the resumable correctness checkpoint."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import correctness_checkpoint as checkpoint


class CorrectnessCheckpointTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.evidence = Path(self.temporary.name)
        self.artifact = self.evidence / "gate" / "raw.json"
        self.artifact.parent.mkdir()
        self.artifact.write_text("{}\n", encoding="utf-8")
        self.binding = {
            "plugin_tree": "a" * 40,
            "dependencies": {"extern/mame": "b" * 40},
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_receipt(self, *, status: str = "PASS", binding=None) -> Path:
        receipt = self.evidence / "gate.json"
        receipt.write_text(
            json.dumps({
                "schema": 1,
                "gate": "gate",
                "status": status,
                "binding": self.binding if binding is None else binding,
                "artifacts": [{
                    "path": "gate/raw.json",
                    "sha256": hashlib.sha256(self.artifact.read_bytes()).hexdigest(),
                }],
            }),
            encoding="utf-8",
        )
        return receipt

    def test_missing_receipt_is_resumable_work(self) -> None:
        state, _ = checkpoint.classify_receipt(
            self.evidence / "missing.json", "gate", self.binding, self.evidence
        )
        self.assertEqual("MISSING", state)

    def test_exact_pass_is_reused(self) -> None:
        state, _ = checkpoint.classify_receipt(
            self.write_receipt(), "gate", self.binding, self.evidence
        )
        self.assertEqual("PASS", state)

    def test_changed_binding_is_stale_not_reused(self) -> None:
        stale = {**self.binding, "plugin_tree": "c" * 40}
        state, _ = checkpoint.classify_receipt(
            self.write_receipt(binding=stale), "gate", self.binding, self.evidence
        )
        self.assertEqual("STALE", state)

    def test_failed_gate_is_not_reused(self) -> None:
        state, _ = checkpoint.classify_receipt(
            self.write_receipt(status="FAIL"), "gate", self.binding, self.evidence
        )
        self.assertEqual("FAIL", state)

    def test_changed_artifact_is_integrity_failure(self) -> None:
        receipt = self.write_receipt()
        self.artifact.write_text("changed\n", encoding="utf-8")
        state, _ = checkpoint.classify_receipt(
            receipt, "gate", self.binding, self.evidence
        )
        self.assertEqual("FAIL", state)


if __name__ == "__main__":
    unittest.main()
