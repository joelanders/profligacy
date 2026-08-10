# Profligacy

An independent JUCE audio plug-in (VST3/AU/standalone) compatible with
user-supplied firmware for the Korg Prophecy synthesizer. Profligacy is not
affiliated with or endorsed by Korg.

**Your code lives at the top level; MAME and JUCE are dependencies underneath.**
The plugin does not fork or bury itself inside MAME — it *links the pinned public
MAME fork as a library* (MAME's frontend `main()` is just one caller of the emu
library; this repo is another) and drives its emulated devices through a narrow
host seam.

## Public alpha support boundary

The first public GitHub alpha supports native Apple Silicon on macOS 15.0 or
later. The AU, VST3, and standalone bundles are ad-hoc signed, not Developer ID
signed or Apple-notarized. Verify the published SHA-256 before using the package;
macOS may then require removing the downloaded archive's quarantine attribute as
described in `release/GITHUB_ALPHA_RELEASE_NOTES.md`.

The alpha has four important limitations:

- Korg firmware is not included. On first launch, choose a directory containing
  `korgprop/ic12_v17.bin` and `korgprop/ic22_v17.bin`, or `korgprop.zip`.
- One Profligacy synth engine may be active per host process. Additional plug-in
  objects remain inert rather than sharing or corrupting the active engine.
- DAW bounce/render must run at real-time 1x. Unrestricted faster-than-real-time
  rendering can be silent because the emulator is paced asynchronously.
- Existing bit-exact evidence ends at the DSP3 serial output. Analog line-output
  coloration has not yet been certified against the physical output stage.

```
profligacy/                       <- this repo (your code)
├── src/
│   └── console_main.cpp          <- headless host harness (the plugin minus JUCE)
├── scripts/
│   ├── build_console.sh          <- compile src/ + link MAME's static archives
│   └── run_console.sh            <- boot korgprop, capture audio (--ring for RT test)
└── extern/
    ├── mame/   (git submodule)   -> joelanders/mame-profligacy
    └── JUCE/   (git submodule)   -> juce-framework/JUCE
```

## Why the driver isn't at the top level

The **Prophecy driver** (`korgprophecy.cpp`) and the **TMS57002 arm64 dynarec** are
MAME device/driver code — they use MAME's framework and are edits to MAME's own
files — so they live inside the `extern/mame` submodule (the fork), not here.
That is the correct seam: the *emulation* is a MAME fork (and could be upstreamed
one day); the *product* is this top-level plugin that consumes it. Everything in
`src/` here is pure host code with zero MAME-framework coupling — it only links MAME.

## Build

One-time: build MAME's static archives inside the submodule (the slow foundation
build). It needs a real `python` on PATH — the asdf shim on this machine is broken
(pins an uninstalled version), so prepend a working one:

```bash
git submodule update --init            # if not already checked out
scripts/build_mame_core.sh --regen     # builds the archives AND writes the rev stamp
```

(`build_mame_core.sh` handles the python-on-PATH quirk itself and stamps the build;
CMake refuses to link archives whose stamp doesn't match the submodule HEAD.)

Then build and run the host harness from this repo:

```bash
./scripts/build_console.sh             # -> ./console_host  (links extern/mame archives)
./scripts/run_console.sh               # Rung 1: capture to WAV
./scripts/run_console.sh --ring        # Rung 2: real-time ring-backpressure self-clocking
```

At runtime, choose a ROM directory containing either
`korgprop/ic12_v17.bin` plus `korgprop/ic22_v17.bin`, or an equivalent
`korgprop.zip`. The HD44780 A00 LCD character table is a documented datasheet
reconstruction compiled into the pinned MAME fork, so no separate LCD ROM file
is required.

To build against an already-built sibling MAME tree instead of the submodule
(skips the 30-min build while iterating):

```bash
MAME_DIR=../mame-profligacy ./scripts/build_console.sh
```

## Status

Validated integration status:

- **Rung 1 (done):** link MAME archives into a non-MAME binary, own `main()`, custom
  OSD, headless boot, audio egress — bit-identical to `propmin -wavwrite`.
- **Rung 2 (done):** ring buffer + real-time-paced consumer; backpressure self-clocks
  MAME to 1.0x wall-clock, 0 underruns, still bit-exact through the ring.
- **Rung 3 (implemented):** `juce::AudioProcessor` (`src/PluginProcessor.cpp`) over
  the engine; `extern/JUCE` (8.0.14); CMake builds AU/VST3/Standalone.
  Source-equivalent integration builds have passed `auval -v aumu Pflg Prfl`,
  including in-host boot, format/render (22k–192k, blocks 64–4096), 1-channel,
  MIDI, and multi-object/teardown tests.  Final 1.0.0 validator receipts must be
  regenerated from the exact release tree as required by `release/v1_preflight.json`.
  A redistributable clean-room CI ROM now provides a bounded packaged-VST3 gate:
  it boots both host CPUs, uploads programs through the real board ports, requires
  audio to traverse DSP1 -> DSP2 -> DSP3 -> DAC, checks an LCD sentinel through
  the public state API, and records per-DSP native-JIT telemetry. See
  `scripts/CI_ROM_E2E.md`.
  This does **not** mean two audible instances are supported: v1 permits one active
  Profligacy instance per host process. A concurrent instance reports unavailable and is kept
  inert so it cannot control or mirror the active synth. Independent simultaneous
  instances require the post-v1 out-of-process bridge described below.

  The constraint comes from two ownership layers, not CPU performance: MAME's
  `mame_machine_manager::s_manager` is process-global, and the Prophecy host ABI
  currently uses process-global callbacks/rings/stores. Making every instance audible
  in-process would require de-singletonizing both layers and auditing other MAME/JIT
  globals. The intended post-v1 route is one service process per plugin instance,
  which provides crash and state isolation without maintaining a broad MAME-core fork.

### Build & validate the plugin

```bash
cmake -B build-cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake -j$(sysctl -n hw.ncpu)     # AU + VST3 + Standalone, installed to ~/Library
auval -v aumu Pflg Prfl                              # validate the AU
open "build-cmake/ProphecyPlugin_artefacts/Release/Standalone/Profligacy.app" # hear it
```

### SysEx editor

The EDIT page combines a clickable signal-flow overview with manifest-driven
Program, Changes, Arpeggiator, Global, and oscillator-model views. It catalogues
all 1,425 known SysEx rows and all seven oscillator engine types without making
unknown/reserved fields writable. Persistent text scaling is available from
`Aa`. Signal-flow blocks open focused detailed views, while repeated
parameter-name prefixes become scan-friendly nested headings with concise
control labels.
The checked-in catalogue and its provenance are documented in
`src/editor/DATA_PROVENANCE.md`.

### Next (product work, not de-risking)

Host-automation parameters for the curated controls; decode the remaining unknown
SysEx fields and packed-byte read-back; add Developer ID signing/notarization; and,
if multi-instance isolation is ever required, an out-of-process bridge.

## Wheel 2 position

The Prophecy's **Wheel 2** is a free-spinning **friction wheel** — it has no spring
and no rest detent, so wherever you leave it is where it stays. That means there is
**no single "correct" position**: the value the synth reads on Wheel 2 depends entirely
on where the physical wheel happened to be left. Because several factory patches route
Wheel 2 to level or timbre, its assumed position **materially changes the sound** (a
different Wheel 2 value has, in the past, looked like a synthesis "defect" when it was
really just the wheel sitting somewhere else).

The plugin exposes this as a persistent **Wheel 2 position** control (MIDI panel,
0–255, stored with your session/preset). The default is **128** (mid), which matches
the emulator's built-in value — leaving it there reproduces the previous behavior
exactly, byte-for-byte.

To **match a specific hardware unit or capture session**, set the wheel where that
session had it. Known reference values:

| Session        | Wheel 2 position |
| -------------- | ---------------- |
| 07-04          | ≈ mid (128)      |
| 07-10 / 07-14  | full up (255)    |
| default        | 128 (mid)        |

You can also drive Wheel 2 live from a MIDI controller: map an incoming CC to **Wheel 2**
in the same MIDI panel (CC → controller remap). A live CC write and the resting-position
control both target the same input (ADIN9); the most recent write wins.

## License

Profligacy's owned source and embedded editor assets are distributed under the
GNU Affero General Public License, version 3 only. JUCE is used under its AGPLv3
alternative. Linked MAME components are available under GPL-2.0-or-later or
their more permissive per-file licenses; GPLv3 and AGPLv3 section 13 permit the
combined distribution under the AGPL terms. See `THIRD_PARTY_NOTICES.md` and
the pinned dependency notices for the complete licensing inventory.

Korg firmware ROMs are not distributed. The user supplies their own lawfully
obtained dump. The built-in LCD character table and its provenance are described
in `THIRD_PARTY_NOTICES.md` and `src/editor/assets/README.md`.
