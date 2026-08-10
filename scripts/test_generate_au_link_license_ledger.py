#!/usr/bin/env python3
"""Focused tests for AU link/license ledger parsing and policy flags."""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import generate_au_link_license_ledger as ledger


class LinkLedgerTests(unittest.TestCase):
    def test_link_map_keeps_real_objects_and_separates_synthetic(self) -> None:
        fixture = """# Path: AU/Profligacy
# Arch: arm64
# Object files:
[  0] linker synthesized
[  6] /tmp/direct.o
[  7] /tmp/libfoo.a[2](member.o)
# Sections:
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.map"
            path.write_text(fixture, encoding="utf-8")
            arch, objects, synthetic = ledger.parse_link_map(path)
        self.assertEqual("arm64", arch)
        self.assertEqual([(6, "/tmp/direct.o"), (7, "/tmp/libfoo.a[2](member.o)")], objects)
        self.assertEqual([(0, "linker synthesized")], synthetic)

    def test_license_aliases_and_requested_review_flags(self) -> None:
        self.assertEqual("GPL-2.0-or-later", ledger.normalize_license("GPL-2.0+"))
        self.assertEqual(
            "AGPL-3.0-only OR LicenseRef-JUCE-Commercial",
            ledger.normalize_license("AGPLv3/Commercial"),
        )
        self.assertEqual([], ledger.review_flags("GPL-2.0-or-later"))
        self.assertEqual(["gpl-2.0-only"], ledger.review_flags("GPL-2.0-only"))
        self.assertEqual(["lgpl"], ledger.review_flags("LGPL-2.1-or-later"))
        self.assertEqual(["unknown-license"], ledger.review_flags("UNKNOWN"))
        self.assertEqual(
            ["proprietary-alternative"],
            ledger.review_flags("AGPL-3.0-only OR LicenseRef-JUCE-Commercial"),
        )
        self.assertEqual("AGPL-3.0-only", ledger.JUCE_SELECTED_LICENSE)
        self.assertEqual(
            "AGPL-3.0-only OR LicenseRef-JUCE-Commercial",
            ledger.JUCE_DUAL_LICENSE,
        )
        self.assertEqual("GPL-2.0-or-later", ledger.MAME_WHOLE_WORK_LICENSE)

    def test_juce_dual_header_resolves_to_selected_agpl_alternative(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            plugin = root / "plugin"
            juce = plugin / "extern/JUCE"
            mame = root / "mame"
            source = juce / "modules/example/example.cpp"
            header = source.with_suffix(".h")
            header.parent.mkdir(parents=True)
            header.write_text(
                "// license: AGPLv3/Commercial\n",
                encoding="utf-8",
            )
            expression, evidence, method = ledger.license_evidence(
                source, plugin, juce, mame, root / "SDL-LICENSE.txt"
            )
        self.assertEqual("AGPL-3.0-only", expression)
        self.assertEqual(header, evidence)
        self.assertEqual("sibling-header-AGPL-alternative-selected", method)

    def test_makefile_release_object_order_preserves_archive_ordinal(self) -> None:
        fixture = """ifeq ($(config),release64)
  OBJDIR = ../../osx_clang/obj/x64/Release/foo
  override TARGET = $(TARGETDIR)/libfoo.a
  OBJECTS := \\
\t$(OBJDIR)/first.o \\
\t$(OBJDIR)/second.o
"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            makefile = root / "build/projects/foo.make"
            makefile.parent.mkdir(parents=True)
            makefile.write_text(fixture, encoding="utf-8")
            target, objects = ledger.parse_release_archive_objects(makefile, root)
        self.assertEqual("libfoo.a", target)
        self.assertEqual(["first.o", "second.o"], [path.name for path in objects])

    def test_darwin_archive_ordinal_resolves_exact_mame_member(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            first = root / "obj/first.o"
            second = root / "obj/second.o"
            source = root / "src/second.cpp"
            second.parent.mkdir(parents=True)
            source.parent.mkdir(parents=True)
            source.write_text("// license:BSD-3-Clause\n", encoding="utf-8")
            second.with_suffix(".d").write_text(
                f"{second}: {source}\n",
                encoding="utf-8",
            )
            sources, method = ledger.resolve_archive_member(
                "libfoo.a",
                3,
                "second.o",
                {},
                {"libfoo.a": [first, second]},
                root,
            )
        self.assertEqual([source], sources)
        self.assertEqual("MAME-generated-makefile-ordinal", method)


if __name__ == "__main__":
    unittest.main()
