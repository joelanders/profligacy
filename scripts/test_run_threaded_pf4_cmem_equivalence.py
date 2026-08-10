#!/usr/bin/env python3
"""Focused unit tests for threaded PF4 real-time-health grading."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_threaded_pf4_cmem_equivalence as gate


def result(mode: str, callbacks: int | None, frames: int | None) -> gate.Result:
    return gate.Result(
        name="boot",
        mode=mode,
        command=[],
        environment={},
        returncode=0,
        wall_seconds=1.0,
        wav_frames=1,
        wav_pcm_bytes=4,
        wav_pcm_sha256="pcm",
        wav_sha256="wav",
        pooled_frame_sizes=[],
        max_runs=0,
        max_fallbacks=0,
        max_compiles=0,
        console_ratio=1.0,
        console_ticks=1,
        console_underruns=callbacks,
        console_peak=0,
        post_warmup_seconds=5.0,
        post_warmup_callbacks=callbacks,
        post_warmup_frames=frames,
        max_callback_streak=callbacks,
        scheduled_midi_dropped=0,
        log="run.log",
    )


class RealtimeHealthTests(unittest.TestCase):
    def test_zero_post_warmup_underruns_pass(self) -> None:
        health = gate.post_warmup_realtime_health(
            [result(mode, 0, 0) for mode in ("interp", "cmem", "shipping")]
        )
        self.assertTrue(health["passed"])
        self.assertEqual([], health["violations"])

    def test_exact_pcm_cannot_hide_audio_deadline_miss(self) -> None:
        health = gate.post_warmup_realtime_health(
            [result("interp", 0, 0), result("cmem", 2, 768), result("shipping", 0, 0)]
        )
        self.assertFalse(health["passed"])
        self.assertIn("cmem: 2 post-warmup underrun callbacks / 768 frames", health["violations"])

    def test_missing_metrics_fail_closed(self) -> None:
        health = gate.post_warmup_realtime_health([result("shipping", None, None)])
        self.assertFalse(health["passed"])
        self.assertEqual(
            ["shipping: missing post-warmup underrun metrics"], health["violations"]
        )


if __name__ == "__main__":
    unittest.main()
