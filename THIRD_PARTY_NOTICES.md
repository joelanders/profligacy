# Third-party notices

Profligacy is distributed under the GNU Affero General Public License,
version 3 only. The complete license text is in `LICENSE`.

The release links or packages components from the projects below. This summary
does not replace their license texts or copyright notices. Corresponding source
distributions must preserve the notices in each pinned dependency.

## JUCE

JUCE 8 is used under its GNU AGPL version 3 alternative. Its `LICENSE.md` and
the notices for JUCE's bundled dependencies are retained in the pinned
`extern/JUCE` source tree.

## MAME components

Selected MAME components are linked from the pinned `extern/mame` source tree.
MAME declares the project as a whole under GNU GPL version 2 or later; many
individual files use permissive licenses recorded in their headers. MAME's
`COPYING`, `README.md`, per-file notices, and `docs/legal` license texts must be
included in the corresponding-source distribution.

The deep-editor parameter catalogue is an independently assembled factual
interoperability dataset, not a MAME or firmware binary. Its preferred source
and provenance are included in `src/editor/`; it is licensed AGPL-3.0-only with
Profligacy's owned source.

MAME is a registered trademark of Gregory Ember. Profligacy does not use the
MAME name or logo as product branding and is not affiliated with or endorsed by
MAMEdev.

## HD44780 A00 character table

The pinned MAME fork contains a 4 KiB HD44780 A00 character-generator table
reconstructed from the glyph diagram on page 97 of Hitachi's published 1985
HD44780 datasheet. It is compiled into the Prophecy-specific LCD device; it is
not a dump of Korg firmware and users do not need to supply an LCD binary.

The table is deliberately kept in one canonical C++ definition. The editor
receives its eight visible rows per character from that same definition at
runtime rather than shipping a second font or binary copy. Verification hashes
for the 4 KiB representation are SHA-1
`65cf075a988cdcbb316b9afdd0529b374a1a65ec` and SHA-256
`2b1e60a4ec7b12f293e07cb66b272afe18b42a4583469210319292818ce4b0f6`.
The source definition records the same provenance and hashes.

## SDL

SDL2 is statically linked under the Zlib license. Its source and license notice
must accompany the corresponding-source distribution for the exact release
build.

## Noto Sans Bold

`src/editor/assets/NotoSans-Bold.ttf` is used for editor labels and is
distributed under the SIL Open Font License 1.1. It is not used to approximate
the fixed LCD characters. The complete OFL text is adjacent at
`src/editor/assets/NotoSans-OFL-1.1.txt`.

## Other linked components

The exact AU link ledger records the linked BSD-2-Clause, BSD-3-Clause, MIT,
Apache-2.0, IJG, Zlib, LZMA SDK public-domain, permission-notice, and SQLite
public-domain-style inputs. Their source notices remain in the pinned JUCE,
MAME, and SDL corresponding-source trees. Apple SDK dynamic system libraries
are recorded separately and are not embedded static objects.

Before release, regenerate the per-format link ledger and the final SBOM from
the exact source, dependency commits, and binaries being distributed.
