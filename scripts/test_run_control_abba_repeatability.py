#!/usr/bin/env python3
"""Focused tests for the balanced control repeatability gate."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_control_abba_repeatability as gate


def result(token: str, *, healthy: bool = True) -> dict[str, object]:
    return {
        "invariants": {"healthy": healthy},
        "stable_events": [token],
        "midi_sha256": token,
        "nvram_sha256": token,
        "wav_sha256": token,
        "name": token,
        "sysex_count": 1,
        "current_dump_length": 619,
    }


class ControlAbbaRepeatabilityTests(unittest.TestCase):
    def test_schedule_balances_both_process_positions(self) -> None:
        self.assertEqual(("reference", "candidate", "candidate", "reference"), gate.ABBA)
        self.assertEqual(2, gate.ABBA.count("reference"))
        self.assertEqual(2, gate.ABBA.count("candidate"))

    def test_pair_requires_audio_midi_nvram_and_events(self) -> None:
        reference = result("same")
        candidate = result("same")
        self.assertTrue(all(gate.pair_checks(reference, candidate).values()))
        candidate["wav_sha256"] = "different"
        checks = gate.pair_checks(reference, candidate)
        self.assertFalse(checks["audio_identical"])
        self.assertTrue(checks["midi_identical"])

    def test_invariant_failure_is_not_hidden_by_identical_outputs(self) -> None:
        checks = gate.pair_checks(result("same"), result("same", healthy=False))
        self.assertFalse(checks["candidate_invariants"])

    def test_cross_run_drift_fails_exact_field(self) -> None:
        runs = [result("same") for _ in range(4)]
        self.assertTrue(all(gate.stable_across_runs(runs).values()))
        runs[2]["nvram_sha256"] = "different"
        stable = gate.stable_across_runs(runs)
        self.assertFalse(stable["nvram_sha256"])
        self.assertTrue(stable["midi_sha256"])

    def test_no_runs_is_fail_closed(self) -> None:
        self.assertFalse(any(gate.stable_across_runs([]).values()))


if __name__ == "__main__":
    unittest.main()
