# Profligacy 1.0.0 alpha 8

Profligacy is an independent AU, VST3, and standalone software instrument
compatible with user-supplied Korg Prophecy firmware. It is not affiliated with
or endorsed by Korg.

## Changes in alpha 8

- Reduced internal audio buffering and report the processing delay to the DAW
  for latency compensation.
- Keep MIDI and audio aligned across sample rates and changing buffer sizes,
  including long sessions.
- Support faster-than-real-time offline rendering and VST3 prefetch.
- Finish firmware startup and preset restoration before playing the first note,
  avoiding the previous long delay after some restore sequences.
- Save the current sound when DAW processing has stopped, and cancel delayed
  editor actions when a project or preset restores another sound.

## Supported systems

- Native Apple Silicon (`arm64`)
- macOS 15.0 or later
- AU, VST3, and standalone formats
- Experimental Windows x64 preview: VST3 and standalone

The macOS public alpha is ad-hoc signed. It is not Developer ID signed or
notarized by Apple. The Windows preview is unsigned and may trigger SmartScreen.
Verify the release asset's published SHA-256 before installing either package.

## Firmware

Korg firmware is not distributed with Profligacy. On first launch, use the
built-in folder picker to select a directory containing either:

- `korgprop/ic12_v17.bin` and `korgprop/ic22_v17.bin`; or
- `korgprop.zip`.

The persistent default location is
`~/Library/Application Support/Profligacy/roms` on macOS and
`%APPDATA%\Profligacy\roms` on Windows. These are per-user locations; no `C:\`
path is hardcoded. The LCD character table is a license-clean datasheet
reconstruction compiled into the public MAME dependency; no separate LCD ROM
is required.

## Installation

Copy the supplied bundles to the conventional per-user locations:

- AU: `~/Library/Audio/Plug-Ins/Components/Profligacy.component`
- VST3: `~/Library/Audio/Plug-Ins/VST3/Profligacy.vst3`
- Standalone: a location of your choice, such as `/Applications`

If macOS blocks the checksum-verified download because it is unnotarized, remove
quarantine only from the exact extracted bundle you intend to use, for example:

```sh
xattr -dr com.apple.quarantine /path/to/Profligacy.app
xattr -dr com.apple.quarantine /path/to/Profligacy.component
xattr -dr com.apple.quarantine /path/to/Profligacy.vst3
```

Do not apply recursive `xattr` commands to a broad directory such as
`/Applications` or `~/Library`.

On Windows, copy `Profligacy.vst3` to your VST3 directory and place
`Profligacy.exe` wherever you keep standalone instruments. The Windows build is
an experimental preview and does not include an installer or code signature.

## Known alpha limitations

- Only one synth engine can be active in a host process. Additional Profligacy
  instances stay inert.
- Let editor changes finish applying before saving a DAW project. Pending
  editor gestures are not part of the saved program.
- Host automation for the deep SysEx editor is not yet exposed.
- Some unknown or packed SysEx fields are intentionally read-only rather than
  guessed.
- DSP output is hardware-corpus-tested, but physical analog output-stage
  coloration is not yet certified.
- Intel Macs and macOS releases older than 15.0 are unsupported by this alpha.
- Windows x64 is supplied as an unsigned experimental preview; macOS arm64 is
  the primary supported alpha target.
- The native TMS57002 JIT is the default on both Windows x64 and macOS arm64.
  For troubleshooting, set `PROPHECY_DSP_ENGINE=interpreter` before launch.

## Verification scope

The public source includes a 554-case hardware-derived TMS57002 replay corpus,
self-contained focused tests, ARM64 interpreter/dynarec equivalence gates,
Windows native-JIT telemetry gates, deterministic headless editor/DAW stress
tests with exact board-link grading, host validation, editor tests, and
publication/licensing audits. Historical
underdeclared captures and unavailable research diagnostics are preserved
outside the release denominator; they are not represented as current product
failures.

## Licensing and source

Profligacy's owned source is AGPL-3.0-only. The release includes corresponding
source, pinned JUCE source, the public MAME source/patch recipe, third-party
notices, and checksums. Korg firmware and donor NVRAM are never included.
