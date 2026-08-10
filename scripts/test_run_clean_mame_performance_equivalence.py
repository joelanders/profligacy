#!/usr/bin/env python3
"""Focused unit tests for the order-balanced performance gate."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_clean_mame_performance_equivalence as gate
import regrade_clean_mame_performance_receipt as regrader


def result(
    implementation: str,
    repeat: int,
    position: int,
    frames: int,
    callbacks: int | None = None,
) -> gate.RunResult:
    if callbacks is None:
        callbacks = int(frames > 0)
    return gate.RunResult(
        implementation=implementation,
        repeat=repeat,
        ordinal=(repeat - 1) * 2 + position,
        position_in_pair=position,
        command=[],
        returncode=0,
        process_wall_seconds=1.0,
        produced_seconds=60.0,
        console_wall_seconds=60.0,
        ratio=1.0,
        ticks=1,
        underrun_callbacks=callbacks,
        peak=1,
        total_underrun_frames=frames,
        post_warmup_callbacks=callbacks,
        post_warmup_frames=frames,
        max_callback_streak=callbacks,
        scheduled_midi_dropped=0,
        soak_notes=1,
        soak_program_changes=1,
        soak_ccs=1,
        pooled_frame_sizes=[],
        max_pooled_runs=0,
        max_pooled_compiles=0,
        errors=[],
        log="",
    )


def paired_rows(
    reference_frames: list[int], candidate_frames: list[int]
) -> tuple[list[gate.RunResult], list[gate.RunResult]]:
    reference: list[gate.RunResult] = []
    candidate: list[gate.RunResult] = []
    for repeat, (ref_frames, cand_frames) in enumerate(
        zip(reference_frames, candidate_frames, strict=True), start=1
    ):
        candidate_first = repeat % 2 == 1
        candidate.append(result("candidate", repeat, 1 if candidate_first else 2, cand_frames))
        reference.append(result("reference", repeat, 2 if candidate_first else 1, ref_frames))
    return reference, candidate


class PerformanceGateTests(unittest.TestCase):
    def test_schedule_balances_both_positions(self) -> None:
        schedule = gate.balanced_schedule("candidate", "reference", 6)
        self.assertEqual(3, sum(order[0] == "candidate" for _, order in schedule))
        self.assertEqual(3, sum(order[0] == "reference" for _, order in schedule))
        with self.assertRaises(ValueError):
            gate.balanced_schedule("candidate", "reference", 5)

    def test_single_scheduler_outlier_is_absolute_failure_not_regression(self) -> None:
        reference, candidate = paired_rows([0] * 6, [0, 0, 2239, 0, 0, 0])
        absolute = gate.absolute_realtime_health(candidate, 0, 0)
        comparison = gate.comparative_noninferiority(reference, candidate, 512)
        self.assertFalse(absolute["passed"])
        self.assertTrue(comparison["passed"])

    def test_outlier_on_reference_label_does_not_create_false_control_failure(self) -> None:
        control_a, control_b = paired_rows([0, 0, 0, 0, 0, 19071], [0] * 6)
        self.assertTrue(gate.comparative_noninferiority(control_a, control_b, 512)["passed"])
        self.assertTrue(gate.comparative_noninferiority(control_b, control_a, 512)["passed"])

    def test_consistent_candidate_regression_fails_comparison(self) -> None:
        reference, candidate = paired_rows([0] * 6, [1024] * 6)
        comparison = gate.comparative_noninferiority(reference, candidate, 512)
        self.assertFalse(comparison["passed"])
        self.assertEqual(6, comparison["pairs_beyond_margin"])

    def test_order_specific_candidate_regression_fails_a_stratum(self) -> None:
        reference, candidate = paired_rows([0] * 6, [1024, 0, 1024, 0, 1024, 0])
        comparison = gate.comparative_noninferiority(reference, candidate, 512)
        self.assertFalse(comparison["passed"])
        self.assertFalse(comparison["order_strata"]["first"]["passed"])

    def test_pf4_stats_are_off_unless_requested(self) -> None:
        self.assertNotIn("KPROP_PF4_STATS", gate.clean_environment(False))
        self.assertEqual("1", gate.clean_environment(True)["KPROP_PF4_STATS"])

    def test_unhealthy_reference_makes_comparison_inconclusive(self) -> None:
        grade = gate.comparative_status(
            {"passed": False},
            {"passed": False},
            None,
            True,
        )
        self.assertEqual("INCONCLUSIVE", grade["status"])
        self.assertEqual("INCONCLUSIVE", gate.overall_verdict([], True, grade["status"]))

    def test_unhealthy_candidate_is_release_fatal(self) -> None:
        self.assertEqual("FAIL", gate.overall_verdict([], False, "PASS"))

    def test_noisy_identical_control_is_inconclusive(self) -> None:
        grade = gate.comparative_status(
            {"passed": True},
            {"passed": True},
            {"control_a": {"passed": False}, "control_b": {"passed": True}},
            True,
        )
        self.assertEqual("INCONCLUSIVE", grade["status"])

    def test_offline_regrade_replaces_intermediate_pass(self) -> None:
        captured = {
            "schema": 2,
            "runs": [],
            "acceptance": {
                "absolute_realtime_health": {
                    "reference": {"passed": False, "violations": ["scheduler outlier"]},
                    "candidate": {"passed": True, "violations": []},
                },
                "comparative_noninferiority": {"passed": True},
            },
            "identical_binary_control": None,
            "failures": [],
            "passed": True,
        }
        graded = gate.regrade_receipt(captured)
        self.assertEqual("INCONCLUSIVE", graded["verdict"])
        self.assertFalse(graded["passed"])
        self.assertEqual("INCONCLUSIVE", graded["acceptance"]["comparative_status"]["status"])
        self.assertTrue(captured["passed"], "offline regrade must not mutate the raw receipt")

    def test_tracked_receipt_path_sanitization_preserves_hashes(self) -> None:
        captured = {
            "inputs": {
                "reference": {"path": "/Users/test/reference", "sha256": "refhash"},
                "candidate": {"path": "/private/tmp/candidate", "sha256": "candhash"},
                "nvram_seed": {"path": "/Users/test/sysram", "sha256": "seedhash"},
                "rompath": "/Users/test/roms",
            },
            "runs": [{
                "command": ["/private/tmp/candidate", "-rompath", "/Users/test/roms"],
                "log": "/private/tmp/output/runs/01_candidate/run.log",
            }],
            "identical_binary_control": None,
            "offline_regrade": {"source": "/Users/test/raw.json"},
        }
        sanitized = regrader.sanitized_receipt(captured, Path("/Users/test/raw.json"))
        self.assertEqual("candidate-host", sanitized["inputs"]["candidate"]["path"])
        self.assertEqual("./runs/01_candidate/run.log", sanitized["runs"][0]["log"])
        self.assertEqual("candhash", sanitized["inputs"]["candidate"]["sha256"])


if __name__ == "__main__":
    unittest.main()
