#!/usr/bin/env python3
"""Focused tests for fail-closed ARM64 dynarec receipt promotion."""

from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import promote_arm64_dynarec_receipt as promoter


def valid_raw() -> dict:
    checks = [
        {"name": name, "pass": True, **(
            {"first_difference": None} if name in promoter.BYTE_CHECKS else {}
        )}
        for name in promoter.REQUIRED_CHECKS
    ]
    runs = {
        name: {
            "max_runs": 1,
            "max_compiles": 1,
            "pooled_frame_sizes": [64],
            "max_forced_midframe": 1 if name == "note.midfb" else 0,
        }
        for name in promoter.NATIVE_RUNS
    }
    runs.update({
        name: {"max_runs": 0, "max_compiles": 0, "pooled_frame_sizes": []}
        for name in promoter.INTERPRETER_RUNS
    })
    return {
        "schema": 1,
        "status": "PASS",
        "arch": "arm64",
        "binary": {"sha256": "a" * 64},
        "cases": sorted(promoter.REQUIRED_CASES),
        "checks": checks,
        "runs": runs,
        "failures": [],
    }


class PromoteArm64DynarecReceiptTests(unittest.TestCase):
    def test_complete_native_exact_receipt_passes(self) -> None:
        self.assertEqual([], promoter.validate_raw(valid_raw()))

    def test_x86_receipt_is_rejected(self) -> None:
        raw = valid_raw()
        raw["arch"] = "x86_64"
        self.assertTrue(promoter.validate_raw(raw))

    def test_fallback_only_native_run_is_rejected(self) -> None:
        raw = valid_raw()
        raw["runs"]["note.pf4"]["max_runs"] = 0
        self.assertTrue(promoter.validate_raw(raw))

    def test_partial_scenario_matrix_is_rejected(self) -> None:
        raw = valid_raw()
        raw["cases"].remove("dense")
        self.assertTrue(promoter.validate_raw(raw))

    def test_byte_difference_is_rejected_even_if_pass_flag_is_wrongly_green(self) -> None:
        raw = valid_raw()
        row = next(item for item in raw["checks"] if item["name"] == "note-pf4")
        row["first_difference"] = 123
        self.assertTrue(promoter.validate_raw(raw))

    def test_interpreter_false_green_is_rejected(self) -> None:
        raw = valid_raw()
        raw["runs"]["boot.interp"]["max_compiles"] = 1
        self.assertTrue(promoter.validate_raw(raw))

    def test_failed_raw_receipt_is_rejected(self) -> None:
        raw = copy.deepcopy(valid_raw())
        raw["status"] = "FAIL"
        raw["failures"] = ["example"]
        self.assertTrue(promoter.validate_raw(raw))


if __name__ == "__main__":
    unittest.main()
