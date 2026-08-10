#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).with_name("run_ci_rom_e2e.py")
SPEC = importlib.util.spec_from_file_location("run_ci_rom_e2e", SCRIPT)
assert SPEC and SPEC.loader
e2e = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(e2e)


class RuntimeTelemetryTest(unittest.TestCase):
    def test_latest_per_dsp_sample_wins(self) -> None:
        log = """
KPROP_JIT_RUNTIME tag=:dsp1 calls=20000 runs=10 fallbacks=1 compiles=1 forced_midframe=0
KPROP_JIT_RUNTIME tag=:dsp2 calls=20000 runs=11 fallbacks=2 compiles=1 forced_midframe=0
KPROP_JIT_RUNTIME tag=:dsp3 calls=20000 runs=12 fallbacks=3 compiles=1 forced_midframe=0
KPROP_JIT_RUNTIME tag=:dsp1 calls=40000 runs=30 fallbacks=1 compiles=1 forced_midframe=0
"""
        stats = e2e.parse_jit_runtime(log)
        self.assertEqual(30, stats[":dsp1"]["runs"])
        self.assertEqual((":dsp2", stats[":dsp2"]), e2e.find_dsp(stats, 2))

    def test_missing_or_ambiguous_dsp_is_rejected(self) -> None:
        self.assertIsNone(e2e.find_dsp({}, 1))
        stats = {":dsp1": {}, ":other:dsp1": {}}
        self.assertIsNone(e2e.find_dsp(stats, 1))


if __name__ == "__main__":
    unittest.main()
