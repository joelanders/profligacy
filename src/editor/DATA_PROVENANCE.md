# Deep editor catalogue provenance

`deep_editor_manifest.tsv` is the preferred source for the generated
`deep_editor_manifest.js` catalogue embedded in Profligacy. The compilation is
licensed under AGPL-3.0-only with the rest of Profligacy's owned source.

The 1,425-row snapshot was created by the Profligacy project from two kinds of
evidence:

- parameter names and user-facing grouping inventoried from Korg's publicly
  distributed Prophecy MIDI/parameter documentation and editor screenshots;
- byte offsets, transport status, changed-byte previews, and confidence fields
  measured by automated firmware SysEx sweeps.

It is a factual interoperability catalogue. It contains no firmware, ROM image,
manual page, screenshot, or executable code copied from Korg. Korg and Prophecy
are trademarks of their respective owner; see `../../THIRD_PARTY_NOTICES.md` for
the compatibility and non-affiliation notice.

The source snapshot comes from MAME research revision
`e528a3eaed89f1a94baf31e4a55be667da7e54ee`. Its packed fields include the
results of a 219-probe program sweep and a 32-probe Brass/Reed sweep, both with
zero probe errors. The durable research receipts have SHA-256 digests
`7475f00a13a2e126d5ad9219c72b1a453191c40f6d1bc0775f91ed1637aad21c` and
`9338912bf77b79e0cbca9ce038958cd3b3adb103d7a678422f0a953479d8c5d8`.

Trailing empty TSV fields were normalized to avoid trailing whitespace; the
normalized source SHA-256 was
`69522429fea7dd7fe06568052bae106aa3d9f85570136bf22e2a51ca0697af62`.

Corrections since that snapshot (19 rows), checked against the firmware's
parameter descriptor table and the 128 factory programs:

- PE Nob2 Value (636) is at offset 526 (between PE Nob1 at 525 and Nob3 at
  527, and in its own sweep list). The snapshot had 172, the first OSC1 type
  byte; the sweep's other offsets for it are now side effects.
- The 18 EG / LFO select rows (enums `EG1|...|A.EG`, `LFO1|...|LFO4`) are
  stored and transmitted as mod-source numbers (EG1-4, P.EG, A.EG = 1-6;
  LFO1-4 = 7-10), not as 0-based enum indices; noted on each row. The editor
  converts at the boundary.

The corrected source SHA-256 is
`da35c83a869c5f870378b5b16518e2700c9fa6f5e428136c2843af08edda89ed`.
Run `python3 scripts/gen_deep_editor_manifest.py` from the repository root to
regenerate the JavaScript asset.
