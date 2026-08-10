#!/usr/bin/env python3

import unittest

import run_arp_timing_suite as timing


class ExternalClockFixtureTest(unittest.TestCase):
    def test_external_clock_uses_public_scheduled_midi_seam(self) -> None:
        case = timing.cases_for("external")[0]
        events = timing.scheduled_fixture(case).split(" ; ")
        clocks = [event for event in events if event.startswith("f8@")]

        self.assertEqual(193, len(clocks))
        self.assertEqual("f8@11.500000", clocks[0])
        self.assertEqual("f8@15.500000", clocks[-1])

    def test_internal_clock_fixture_has_no_external_ticks(self) -> None:
        case = timing.cases_for("subdivision")[0]
        events = timing.scheduled_fixture(case).split(" ; ")

        self.assertFalse(any(event.startswith("f8@") for event in events))


class KeySyncClockMetricsTest(unittest.TestCase):
    def test_one_nearby_short_reset_is_removed_from_steady_timing(self) -> None:
        case = timing.Case(name="keysync", key_sync=True, driver_note_start=12.0)
        metrics = timing.key_sync_clock_metrics(
            case, {"clock_times_s": [11.992, 12.000, 12.001, 12.009, 12.017]})

        self.assertTrue(metrics["reset_valid"])
        self.assertEqual(1, metrics["reset_count"])
        self.assertAlmostEqual(0.001, metrics["reset_interval_s"])
        self.assertAlmostEqual(0.008, metrics["stable_median_s"])
        self.assertLess(metrics["stable_max_jitter_s"], 0.000100)

    def test_multiple_short_intervals_do_not_qualify_as_one_reset(self) -> None:
        case = timing.Case(name="keysync", key_sync=True, driver_note_start=12.0)
        metrics = timing.key_sync_clock_metrics(
            case, {"clock_times_s": [11.992, 12.000, 12.001, 12.002, 12.010]})

        self.assertFalse(metrics["reset_valid"])
        self.assertEqual(2, metrics["reset_count"])

    def test_short_interval_far_from_note_on_is_not_a_key_sync_reset(self) -> None:
        case = timing.Case(name="keysync", key_sync=True, driver_note_start=12.0)
        metrics = timing.key_sync_clock_metrics(
            case, {"clock_times_s": [12.080, 12.088, 12.089, 12.097, 12.105]})

        self.assertFalse(metrics["reset_valid"])
        self.assertEqual(1, metrics["reset_count"])


class KeySyncCrossCaseTest(unittest.TestCase):
    def test_free_running_phase_comparison_wraps_by_one_step_period(self) -> None:
        cases = timing.cases_for("keysync")
        period = 0.096192
        records = [
            {"result": {"onset_times_s": [12.033635],
                        "note_on": {"fit_interval_s": period}}},
            {"result": {"onset_times_s": [12.129824],
                        "note_on": {"fit_interval_s": period}}},
            {"result": {"onset_times_s": [12.002600],
                        "note_on": {"fit_interval_s": period}}},
            {"result": {"onset_times_s": [12.042650],
                        "note_on": {"fit_interval_s": period}}},
        ]

        passed, failed = timing.cross_case_checks("keysync", cases, records)

        self.assertEqual([], failed)
        self.assertEqual(3, len(passed))


if __name__ == "__main__":
    unittest.main()
