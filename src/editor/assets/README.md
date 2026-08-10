# Editor display-asset provenance

## Fixed LCD characters

The LCD uses the HD44780 A00 character table compiled into
`extern/mame/src/devices/video/hd44780.cpp`. The table was reconstructed from
the glyph diagram on page 97 of Hitachi's published 1985 HD44780 datasheet.
There is no separate `.bin`, JavaScript glyph table, or downloadable user
dependency.

- Canonical 4 KiB SHA-1: `65cf075a988cdcbb316b9afdd0529b374a1a65ec`
- Canonical 4 KiB SHA-256:
  `2b1e60a4ec7b12f293e07cb66b272afe18b42a4583469210319292818ce4b0f6`
- Editor projection (eight visible rows for each of 256 characters) SHA-256:
  `03dfee0c369ae14ae20f490b54a387602bde923408eab94f7d7a7ac9def2aa5a`

The Prophecy-specific emulated LCD reads the canonical table directly. The
editor asks the native resource bridge for a generated 2 KiB projection of the
same bytes. A static release test checks all three hashes, the resource path,
and the CGRAM-versus-fixed-character routing.

## Noto Sans Bold

`NotoSans-Bold.ttf` is the font embedded for editor labels. It is not the LCD
character source.

- SHA-256: `6c9841ae63e266b77ee79820d62095a244d2e76d638b8a45ba3cb2c23f3e1932`
- Locally verified source: `scripts/font/NotoSans-Bold.ttf` in the pinned public
  MAME tree at commit `d7779c3e697a2047d1e57d730beca4770d78bf94`
- Verification: the two files are byte-for-byte identical (`cmp`) and have the
  same SHA-256 digest.
- License: SIL Open Font License 1.1, reproduced in
  `NotoSans-OFL-1.1.txt`. The source tree distributes that license adjacent to
  the font, and its adjacent Noto README identifies Google Noto as the origin.

The exact earlier Google source package or upstream commit is not established
by the local evidence. This file deliberately records only what can be verified
without fetching external material.
