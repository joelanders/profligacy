#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Fail closed on the public Profligacy identity and legacy-data migration."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ProductIdentityTests(unittest.TestCase):
    def test_plugin_metadata_is_profligacy(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        for declaration in (
            "project(profligacy_plugin VERSION 1.0.0",
            'PRODUCT_NAME "Profligacy"',
            'COMPANY_NAME "Profligacy"',
            'BUNDLE_ID "net.joelanders.profligacy"',
            "PLUGIN_MANUFACTURER_CODE Prfl",
            "PLUGIN_CODE Pflg",
        ):
            self.assertIn(declaration, cmake)
        processor = (ROOT / "src/PluginProcessor.h").read_text(encoding="utf-8")
        self.assertIn('getName() const override { return "Profligacy"; }', processor)

    def test_editor_and_docs_use_independent_product_brand(self) -> None:
        editor = (ROOT / "src/editor/index.html").read_text(encoding="utf-8")
        self.assertIn("<title>Profligacy</title>", editor)
        # The native window title already carries the product name; the editor's
        # upper-left duplicate was deliberately removed to free working space.
        self.assertNotIn('<div class="brand">Profligacy ', editor)
        self.assertIn('id="chip_program"', editor)
        self.assertIn("Profligacy emulates the Korg Prophecy and requires user-supplied firmware.", editor)
        self.assertIn('id="rompick"', editor)
        readme = (ROOT / "README.md").read_text(encoding="utf-8")
        self.assertTrue(readme.startswith("# Profligacy\n"))
        self.assertIn("not affiliated with or endorsed by Korg", " ".join(readme.split()))
        self.assertNotIn("notes/", readme)

    def test_editor_has_no_screen_sized_height_ceiling(self) -> None:
        processor = (ROOT / "src/PluginProcessor.cpp").read_text(encoding="utf-8")
        self.assertIn("setResizeLimits(740, 360, 16384, 16384);", processor)
        self.assertNotIn("setResizeLimits(740, 360, 2960, 1440);", processor)

    def test_public_mame_dependency_has_no_private_tracking_branch(self) -> None:
        modules = (ROOT / ".gitmodules").read_text(encoding="utf-8")
        self.assertIn("https://github.com/joelanders/mame-profligacy.git", modules)
        self.assertNotIn("git@", modules)
        self.assertNotIn("mame-private", modules)
        self.assertNotIn("branch =", modules)

    def test_legacy_settings_and_nvram_are_migrated(self) -> None:
        locator = (ROOT / "src/rom_locator.h").read_text(encoding="utf-8")
        self.assertIn('o.applicationName     = "Profligacy";', locator)
        self.assertIn("legacySettingsOptions()", locator)
        self.assertIn("legacyAppSupportDir()", locator)
        self.assertIn("legacy.existsAsFile() ? legacy : sibling", locator)
        editor = (ROOT / "src/editor/index.html").read_text(encoding="utf-8")
        for key in (
            "profligacy.editor.appearance.v2",
            "profligacy.panelLayout.v5",
            "profligacy.panelMaterial.v5",
        ):
            self.assertIn(key, editor)
        for legacy_key in (
            "prophecy.editor.appearance.v2",
            "prophecy.panelLayout.v5",
            "prophecy.panelMaterial.v5",
        ):
            self.assertIn(legacy_key, editor)

    def test_release_tools_have_no_obsolete_bundle_identity(self) -> None:
        paths = (ROOT / "CMakeLists.txt", ROOT / "README.md", ROOT / "scripts/run_gates.sh")
        text = "\n".join(path.read_text(encoding="utf-8") for path in paths)
        for obsolete in (
            "com.joelanders.prophecy",
            "Prp1 Jlnd",
            "Prophecy.component",
            "Prophecy.vst3",
            "Standalone/Prophecy.app",
        ):
            self.assertNotIn(obsolete, text)
        ledger = (ROOT / "scripts/generate_au_link_license_ledger.py").read_text(encoding="utf-8")
        self.assertIn('archive_name == "libProfligacy_SharedCode.a"', ledger)

    def test_public_tree_has_no_private_worktree_documentation(self) -> None:
        files = [
            ROOT / "README.md",
            ROOT / "scripts/build_console.sh",
            ROOT / "scripts/build_mame_core.sh",
            ROOT / "scripts/run_console.sh",
        ]
        text = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in files)
        self.assertNotIn("mame-fuck", text)
        self.assertNotIn("mame-private", text)
        self.assertNotIn("extern/mame CLAUDE.md", text)


if __name__ == "__main__":
    unittest.main()
