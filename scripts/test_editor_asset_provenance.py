#!/usr/bin/env python3
"""Static, non-GUI checks for editor font and LCD rendering provenance."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
MAME_ROOT = Path(os.environ.get("MAME_DIR", ROOT / "extern/mame"))
HTML = ROOT / "src/editor/index.html"
ASSETS = ROOT / "src/editor/assets"
MAME_LCD_CPP = MAME_ROOT / "src/devices/video/hd44780.cpp"
MAME_LCD_HEADER = MAME_ROOT / "src/devices/video/hd44780.h"
MAME_PROPHECY = MAME_ROOT / "src/mame/korg/korgprophecy.cpp"
PLUGIN_PROCESSOR = ROOT / "src/PluginProcessor.cpp"
PROPHECY_ENGINE = ROOT / "src/prophecy_engine.cpp"
ROM_LOCATOR = ROOT / "src/rom_locator.h"
NOTO_SHA256 = "6c9841ae63e266b77ee79820d62095a244d2e76d638b8a45ba3cb2c23f3e1932"
CGROM_SHA1 = "65cf075a988cdcbb316b9afdd0529b374a1a65ec"
CGROM_SHA256 = "2b1e60a4ec7b12f293e07cb66b272afe18b42a4583469210319292818ce4b0f6"
GLYPH_ROWS_SHA256 = "03dfee0c369ae14ae20f490b54a387602bde923408eab94f7d7a7ac9def2aa5a"


class EditorAssetProvenanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.html = HTML.read_text(encoding="utf-8")
        cls.mame_cpp = MAME_LCD_CPP.read_text(encoding="utf-8")
        match = re.search(
            r"s_hd44780_a00_reconstructed_cgrom\[0x1000\]\s*=\s*\{(.*?)\};",
            cls.mame_cpp,
            re.DOTALL,
        )
        if match is None:
            raise AssertionError("compiled HD44780 A00 table not found")
        cls.cgrom = bytes(int(token, 16) for token in re.findall(r"0x([0-9a-fA-F]{2})", match.group(1)))
        cls.glyph_rows = b"".join(cls.cgrom[ch * 16 : ch * 16 + 8] for ch in range(256))

    def test_one_canonical_compiled_table_not_an_html_copy(self) -> None:
        self.assertNotIn("const GLYPHS", self.html)
        self.assertNotIn("full HD44780 A00 ROM", self.html)
        self.assertEqual(4096, len(self.cgrom))

    def test_compiled_table_matches_documented_mame_reconstruction(self) -> None:
        self.assertEqual(CGROM_SHA1, hashlib.sha1(self.cgrom).hexdigest())
        self.assertEqual(CGROM_SHA256, hashlib.sha256(self.cgrom).hexdigest())
        self.assertIn(CGROM_SHA1, self.mame_cpp)
        self.assertIn(CGROM_SHA256, self.mame_cpp)
        self.assertEqual(GLYPH_ROWS_SHA256, hashlib.sha256(self.glyph_rows).hexdigest())
        self.assertEqual(bytes((14, 17, 17, 17, 31, 17, 17, 0)), self.glyph_rows[0x41 * 8 : 0x42 * 8])

    def test_prophecy_uses_explicit_no_external_rom_device(self) -> None:
        header = MAME_LCD_HEADER.read_text(encoding="utf-8")
        driver = MAME_PROPHECY.read_text(encoding="utf-8")
        self.assertIn("class hd44780_a00_reconstructed_device", header)
        self.assertIn("HD44780_A00_RECONSTRUCTED(config, m_lcdc", driver)
        self.assertNotIn("HD44780(config, m_lcdc", driver)

    def test_only_live_cgram_codes_are_pixel_exact(self) -> None:
        self.assertIn("ch >= 0x00 && ch <= 0x0f ? (ch & 7) : -1", self.html)
        self.assertIn("const cgramSlot = raw ? lcdCgramSlot(ch) : -1", self.html)
        self.assertIn("lcdRaw.cgram[cgramSlot*8+gr]", self.html)

    def test_hd44780_cgram_aliases_are_live_checked(self) -> None:
        self.assertIn("ok('HD44780 CGRAM aliases'", self.html)
        self.assertIn("[0x00,0x07,0x08,0x0f,0x10].map(lcdCgramSlot)", self.html)
        self.assertEqual(1, self.html.count("function lcdCgramSlot(ch)"))
        self.assertLess(
            self.html.index("function lcdCgramSlot(ch)"),
            self.html.index("async function runSelfTest()"),
        )

    def test_editor_gets_fixed_glyphs_from_compiled_table(self) -> None:
        processor = PLUGIN_PROCESSOR.read_text(encoding="utf-8")
        engine = PROPHECY_ENGINE.read_text(encoding="utf-8")
        self.assertIn("/assets/hd44780-a00-glyphs.bin", self.html)
        self.assertIn("if (b.byteLength !== 256*8)", self.html)
        self.assertIn("const g = ch*8", self.html)
        self.assertIn("const bits = lcdA00Glyphs[g+gr]", self.html)
        self.assertIn("ok('HD44780 A00 glyph route'", self.html)
        self.assertIn('url == "/assets/hd44780-a00-glyphs.bin"', processor)
        self.assertIn("ProphecyEngine::lcdA00GlyphRows", processor)
        self.assertIn("hd44780_a00_reconstructed_cgrom()", engine)

    def test_user_rom_validation_requires_only_korg_firmware(self) -> None:
        locator = ROM_LOCATOR.read_text(encoding="utf-8")
        self.assertIn('getChildFile("ic12_v17.bin")', locator)
        self.assertIn('getChildFile("ic22_v17.bin")', locator)
        self.assertIn('getChildFile("korgprop.zip")', locator)
        self.assertNotIn("hd44780_a00", locator)
        self.assertNotIn("hd44780.zip", locator)

    def test_bundled_noto_hash_and_license(self) -> None:
        digest = hashlib.sha256((ASSETS / "NotoSans-Bold.ttf").read_bytes()).hexdigest()
        self.assertEqual(NOTO_SHA256, digest)
        license_text = (ASSETS / "NotoSans-OFL-1.1.txt").read_text(encoding="utf-8")
        self.assertIn("SIL OPEN FONT LICENSE Version 1.1", license_text)
        provenance = (ASSETS / "README.md").read_text(encoding="utf-8")
        self.assertIn(NOTO_SHA256, provenance)


if __name__ == "__main__":
    unittest.main()
