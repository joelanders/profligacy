# Profligacy 1.0.0 alpha 6

Profligacy is an independent AU, VST3, and standalone software instrument
compatible with user-supplied Korg Prophecy firmware. It is not affiliated with
or endorsed by Korg.

## Changes in alpha 6

- Added a guarded delivery invariant for the Prophecy's internal V55-to-H8
  serial link. A completed V55 transmit byte remains pending until H8 SCI0
  accepts it; after a stable idle, error-free delivery deficit, the missing byte
  is delivered through the normal H8 receive-data and interrupt path. This
  prevents the observed control freeze in which the final two bytes of a
  four-byte command disappeared while audio continued.
- Added a deterministic two-frame fault gate for that failure. With the guard
  disabled it reproduces the unresponsive control path with two bytes pending
  and no SCI errors; with the guard enabled exactly two deliveries are repaired
  and the editor remains responsive. Normal 44.1, 48, and 96 kHz stress runs
  complete with zero repairs and zero delivery-queue overflows.
- The lower-level cause of the rare callback/event disappearance is not yet
  identified. Alpha 6 therefore treats this as a narrowly gated stability
  safeguard rather than claiming the scheduler mechanism itself is fixed.
- Retains alpha 5's paced editor SysEx path and board-link stress coverage,
  alpha 4's MIDI-stability fixes, alpha 3's native TMS57002 JIT improvements,
  and alpha 2's WebView2 editor fix.

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
- DAW bounce/render must use real-time 1x. Unrestricted offline rendering can be
  silent.
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
