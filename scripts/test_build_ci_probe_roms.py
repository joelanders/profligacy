#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("build_ci_probe_roms.py")
SPEC = importlib.util.spec_from_file_location("build_ci_probe_roms", SCRIPT)
assert SPEC and SPEC.loader
roms = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(roms)


class ProbeRomTest(unittest.TestCase):
    def test_sizes_vectors_and_sentinel_are_stable(self) -> None:
        v55 = roms.emit_v55()
        h8 = roms.emit_h8()
        self.assertEqual(0x80000, len(v55))
        self.assertEqual(0x20000, len(h8))
        self.assertEqual(bytes.fromhex("EA 00 00 00 80"), v55[0x7FFF0:0x7FFF5])
        self.assertEqual(bytes.fromhex("00 00 00 01"), h8[:4])
        self.assertEqual(
            "93da86ea0cb515798d9010e2bbcab63e32ada975c7a3c4105cbd25bf2591fd8a",
            hashlib.sha256(v55).hexdigest(),
        )
        self.assertEqual(
            "774ee241fb679ac2da18d55cc7ce2dcb73b9a6d5867b7bd66e507ccf35418adc",
            hashlib.sha256(h8).hexdigest(),
        )
        lcd_bytes = bytes(
            v55[index + 4]
            for index in range(len(v55) - 4)
            if v55[index:index + 4] == bytes.fromhex("C6 06 07 FF")
        )
        self.assertIn(b"CI V55+H8 OK", lcd_bytes)

    def test_build_emits_only_two_firmware_files(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            receipt = roms.build(root)
            files = sorted(path.relative_to(root).as_posix() for path in root.rglob("*") if path.is_file())
        self.assertEqual(["korgprop/ic12_v17.bin", "korgprop/ic22_v17.bin"], files)
        self.assertTrue(receipt["redistributable"])


if __name__ == "__main__":
    unittest.main()
