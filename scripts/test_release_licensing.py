#!/usr/bin/env python3
"""Static release-policy checks for the selected AGPL distribution path."""

from __future__ import annotations

import csv
import hashlib
from pathlib import Path
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_au_link_license_ledger as ledger


class ReleaseLicensingTests(unittest.TestCase):
    def test_canonical_agpl_text_and_public_license_summary(self) -> None:
        license_path = ROOT / "LICENSE"
        self.assertEqual(
            "0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0",
            hashlib.sha256(license_path.read_bytes()).hexdigest(),
        )
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertIn("GNU Affero General Public License, version 3 only", readme)

    def test_owned_link_inputs_declare_agpl(self) -> None:
        paths = (
            "src/PluginProcessor.cpp",
            "src/PluginProcessor.h",
            "src/console_main.cpp",
            "src/lifecycle_main.cpp",
            "src/prophecy_engine.cpp",
            "src/prophecy_engine.h",
            "src/rom_locator.h",
            "src/editor/index.html",
            "src/editor/deep_editor_manifest.js",
            "scripts/multi_instance_contract_test.cpp",
        )
        for relative in paths:
            head = (ROOT / relative).read_text(encoding="utf-8")[:256]
            self.assertIn("SPDX-License-Identifier: AGPL-3.0-only", head, relative)

    def test_deep_editor_catalogue_has_reproducible_public_source(self) -> None:
        source = ROOT / "src/editor/deep_editor_manifest.tsv"
        self.assertEqual(
            "69522429fea7dd7fe06568052bae106aa3d9f85570136bf22e2a51ca0697af62",
            hashlib.sha256(source.read_bytes()).hexdigest(),
        )
        provenance = (ROOT / "src/editor/DATA_PROVENANCE.md").read_text(encoding="utf-8")
        self.assertIn("factual interoperability catalogue", provenance)
        self.assertIn("AGPL-3.0-only", provenance)

        with source.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        self.assertEqual(1425, len(rows))
        self.assertEqual(39, sum(bool(row["bit_width"]) for row in rows))
        self.assertEqual(43, sum("bit_sweep" in row["confidence"] for row in rows))
        fixed = next(
            row for row in rows
            if row["kind"] == "program" and row["group"] == "1" and row["param"] == "19"
        )
        self.assertEqual(("0", "0", "SOLO", "", ""), (
            fixed["min"], fixed["max"], fixed["enum"],
            fixed.get("bit_shift") or "", fixed.get("bit_width") or ""
        ))
        self.assertIn("bit ownership is not observable", fixed["notes"])

    def test_ledger_selects_compatible_license_alternatives(self) -> None:
        self.assertEqual("AGPL-3.0-only", ledger.PROJECT_LICENSE)
        self.assertEqual("AGPL-3.0-only", ledger.JUCE_SELECTED_LICENSE)
        self.assertEqual("GPL-2.0-or-later", ledger.MAME_WHOLE_WORK_LICENSE)
        self.assertEqual([], ledger.review_flags(ledger.PROJECT_LICENSE))
        self.assertEqual([], ledger.review_flags(ledger.JUCE_SELECTED_LICENSE))
        self.assertEqual([], ledger.review_flags(ledger.MAME_WHOLE_WORK_LICENSE))

    def test_required_notice_files_are_present(self) -> None:
        notices = (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8")
        for name in ("JUCE", "MAME components", "HD44780 A00 character table", "SDL", "Noto Sans Bold"):
            self.assertIn(name, notices)
        self.assertTrue((ROOT / "src/editor/assets/NotoSans-OFL-1.1.txt").is_file())


if __name__ == "__main__":
    unittest.main()
