# Profligacy

Profligacy is an independent JUCE audio plug-in — AU, VST3, and standalone — compatible with user-supplied firmware for the Korg Prophecy synthesizer.

It is not affiliated with or endorsed by Korg.

Profligacy uses a pinned MAME fork to emulate the Prophecy hardware and exposes that emulated synthesizer through a conventional audio plug-in interface.

## Current target

The primary public-alpha target is:

* native Apple Silicon (`arm64`);
* macOS 15.0 or later;
* AU, VST3, and standalone formats.

An experimental Windows x64 build also exists, but macOS arm64 is the primary supported alpha platform.

The current Profligacy implementation targets **Korg Prophecy firmware v1.7**. The underlying MAME driver contains definitions for other Prophecy firmware revisions, but Profligacy currently boots and validates the v1.7 machine specifically.

Korg firmware is not distributed with Profligacy. You must supply your own lawfully obtained firmware dump.

## Firmware

Profligacy uses MAME's standard ROM-set layout.

For the current v1.7 target, provide either:

```text
<ROMPATH>/
└── korgprop/
    ├── ic12_v17.bin
    └── ic22_v17.bin
```

or:

```text
<ROMPATH>/
└── korgprop.zip
```

where `korgprop.zip` contains the two firmware images (note that if using a zip, the filename of the firmware images must match the above required format).

`IC12` and `IC22` are the physical ROM designators used by the Prophecy hardware:

* `IC12` contains the main H8/3003 CPU firmware;
* `IC22` contains the V55 sub-CPU firmware.

The filenames and `korgprop` set name are MAME conventions. MAME also validates the ROM contents, so renaming an arbitrary firmware image is not sufficient.

The HD44780 A00 LCD character table is reconstructed from the published datasheet and compiled into the pinned MAME fork. No separate LCD ROM is required.

## Build and run

### 1. Initialise the dependencies

After cloning the repository:

```bash
git submodule update --init
```

The important dependencies live under:

```text
extern/
├── mame/
└── JUCE/
```

### 2. Build the pinned static SDL2 prerequisite

The macOS build requires a static SDL2 archive at
`${SDL_PREFIX}/lib/libSDL2.a`. The release workflow builds real SDL2 2.32.10
from source; a current Homebrew `sdl2-compat` installation may provide only
`libSDL2.dylib` and is not a substitute.

Build the same pinned dependency once outside the repository:

```bash
SDL_VERSION=2.32.10
SDL_PREFIX="$HOME/.local/SDL2-$SDL_VERSION"
SDL_SOURCE_ROOT="$HOME/.local/src"
SDL_BUILD_ROOT="$HOME/.local/build/SDL2-$SDL_VERSION"
SDL_ARCHIVE="/tmp/SDL2-$SDL_VERSION.tar.gz"

mkdir -p "$SDL_SOURCE_ROOT"
curl --fail --location --retry 3 --output "$SDL_ARCHIVE" \
  "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
echo '5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165  '"$SDL_ARCHIVE" | shasum -a 256 -c -
tar -xzf "$SDL_ARCHIVE" -C "$SDL_SOURCE_ROOT"
cmake -S "$SDL_SOURCE_ROOT/SDL2-$SDL_VERSION" -B "$SDL_BUILD_ROOT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DSDL_SHARED=OFF \
  -DSDL_STATIC=ON \
  -DSDL_TEST=OFF
cmake --build "$SDL_BUILD_ROOT" --parallel "$(sysctl -n hw.ncpu)" --target install
test -f "$SDL_PREFIX/lib/libSDL2.a"
```

Keep `SDL_PREFIX` set for the following MAME and plug-in build steps.

### 3. Build the MAME foundation

MAME is built separately into static archives and then linked into Profligacy.

This is the slow foundation build and normally only needs to be repeated when the pinned MAME source changes:

```bash
SDL_INSTALL_ROOT="$SDL_PREFIX" scripts/build_mame_core.sh
```

The script also stamps the resulting MAME archives with the exact MAME revision from which they were built.

Profligacy checks this stamp and refuses to link stale archives from a different `extern/mame` revision.

### 4. Build the console host

The console host is a small headless harness for running the emulated Prophecy without JUCE:

```bash
./scripts/build_console.sh
```

This produces:

```text
./console_host
```

and also builds the lifecycle test host used for engine teardown/reconstruction testing.

### 5. Run the console host

`run_console.sh` boots the v1.7 Prophecy machine headlessly, injects a test note, captures the resulting audio, and exits.

Set `ROMPATH` to the directory containing your `korgprop/` directory or `korgprop.zip`:

```bash
ROMPATH=/path/to/my/roms ./scripts/run_console.sh
```

For example:

```text
/Users/me/ProphecyROMs/
└── korgprop/
    ├── ic12_v17.bin
    └── ic22_v17.bin
```

would be run with:

```bash
ROMPATH=/Users/me/ProphecyROMs ./scripts/run_console.sh
```

The normal harness writes:

```text
console_out.wav
```

The script currently defaults `ROMPATH` to a sibling `../mame/00-roms` directory when no override is supplied. That is a developer convenience rather than a required repository layout; setting `ROMPATH` explicitly is clearer.

There are two harness modes:

```bash
ROMPATH=/path/to/roms ./scripts/run_console.sh
```

Rung 1: straightforward capture to WAV.

```bash
ROMPATH=/path/to/roms ./scripts/run_console.sh --ring
```

Rung 2: exercises the real-time ring-buffer/backpressure path and writes:

```text
console_ring_out.wav
```

### 6. Build the plug-in

Use the same static SDL2 installation when configuring CMake:

```bash
cmake -B build-cmake -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL_PREFIX="$SDL_PREFIX"
```

Then build:

```bash
cmake --build build-cmake -j$(sysctl -n hw.ncpu)
```

This builds the AU, VST3, and standalone applications.

On macOS, validate the Audio Unit with:

```bash
auval -v aumu Pflg Prfl
```

and launch the standalone application with:

```bash
open "build-cmake/ProphecyPlugin_artefacts/Release/Standalone/Profligacy.app"
```

## Using the plug-in

On first launch, Profligacy looks for the v1.7 ROM set and, if necessary, presents a folder picker.

The selected directory should contain either:

```text
korgprop/
├── ic12_v17.bin
└── ic22_v17.bin
```

or:

```text
korgprop.zip
```

The selected path is persisted for subsequent launches.

The conventional per-user ROM location is:

```text
~/Library/Application Support/Profligacy/roms
```

on macOS.

A `PROPHECY_ROMPATH` environment variable can also be used to specify the plug-in ROM path explicitly.

### One active engine per process

The current implementation supports **one active Profligacy synthesizer engine per host process**.

Additional plug-in objects can exist, but remain inert rather than sharing or corrupting the active emulated machine.

Independent simultaneous synth instances are therefore not currently supported.

### Real-time bounce

DAW bounce/render currently needs to run at real-time `1x`.

Unrestricted faster-than-real-time rendering can produce silence because the emulator is paced asynchronously through the audio ring.

### DSP execution engine

The native TMS57002 execution engine is the default on supported 64-bit builds, including the experimental Windows x64 build.

For troubleshooting, force the portable interpreter before launching the plug-in or standalone application:

```bash
PROPHECY_DSP_ENGINE=interpreter
```

The lower-level `KPROP_DSP_PERFRAME` developer variable remains available and takes precedence when explicitly set.

## Alpha release notes

The macOS alpha bundles are ad-hoc signed rather than Developer ID signed and are not Apple-notarized.

Verify the published SHA-256 of a release package before using it. macOS may require removal of the downloaded bundle's quarantine attribute; see the release notes for the exact procedure.

The current alpha also has the following important limitations:

* one active synthesizer engine per host process;
* DAW bounce/render must currently run at real-time `1x`;
* host automation for the full deep SysEx editor is not yet exposed;
* some unknown or packed SysEx fields remain intentionally read-only;
* Intel Macs and macOS releases older than 15.0 are outside the current public-alpha target;
* Windows x64 is experimental rather than the primary supported platform.

Existing bit-exact evidence reaches the DSP3 serial output. Physical analogue line-output coloration has not yet been certified against the hardware output stage.

---

# Developer and architecture notes

The sections below describe the internal architecture, validation work, build implementation, and current research status. They are not required simply to build and run Profligacy.

## Repository structure

The product source lives at the top level. MAME and JUCE are dependencies underneath it:

```text
profligacy/
├── src/
│   ├── PluginProcessor.cpp
│   ├── prophecy_engine.cpp
│   └── console_main.cpp
├── scripts/
│   ├── build_mame_core.sh
│   ├── build_console.sh
│   └── run_console.sh
└── extern/
    ├── mame/
    └── JUCE/
```

`console_main.cpp` is the headless host harness: effectively the engine integration without the JUCE plug-in shell.

## Why the Prophecy driver lives inside MAME

The Prophecy driver (`korgprophecy.cpp`) and the TMS57002 native execution work are MAME device/driver code.

They use MAME's internal framework and modify MAME-owned components, so they live inside the `extern/mame` submodule rather than the Profligacy product source.

That is the intended architectural boundary:

```text
MAME fork
    ↓
Prophecy hardware emulation
    ↓
narrow host seam
    ↓
Profligacy engine
    ↓
JUCE plug-in / standalone
```

The emulation work could potentially be upstreamed to MAME independently of Profligacy.

The top-level `src/` product code keeps the MAME-specific dependency concentrated in `prophecy_engine.cpp`; the host-facing interface itself remains MAME-free.

## MAME archive freshness

MAME is deliberately treated as a separately built foundation dependency.

`scripts/build_mame_core.sh` builds the required static archives and writes:

```text
.prophecy_build_rev
```

alongside them.

The stamp contains the MAME Git revision used for that build.

At CMake configure time and again during the build, Profligacy compares the stamp against the currently checked-out `extern/mame` revision.

This prevents a particularly dangerous failure mode:

```text
update extern/mame
        ↓
forget to rebuild MAME
        ↓
link old static archives
        ↓
apparently successful but stale product build
```

A dirty MAME build is treated specially and should not be used for a release package.

## Python and MAME build generation

The MAME build requires Python.

`build_mame_core.sh` locates a working `python3`, passes the resulting executable to MAME as `PYTHON_EXECUTABLE`, and currently also creates a temporary `python` alias on `PATH` for compatibility with MAME build paths that may probe that command name.

This was originally added around a local version-manager environment where an `asdf` Python shim pointed at an uninstalled interpreter. Users should not need to modify their own `PATH` manually.

The helper also accepts:

```bash
scripts/build_mame_core.sh --regen
```

which maps to MAME's `REGENIE` option and forces regeneration of the Genie-generated project files.

For the currently pinned MAME makefile this flag is redundant in practice: the wrapper supplies `REGENIE=0` when the flag is absent, while MAME tests whether `REGENIE` is defined rather than whether its value is `1`. Consequently the documented build does not require `--regen`.

`REGENIE` itself refers only to regenerating MAME's generated build/project files; it is separate from compilation of the C++ sources.

## Building against another MAME tree

For emulator development or A/B comparison work, Profligacy can use an already-built MAME worktree instead of the checked-out submodule.

For example:

```bash
MAME_DIR=../mame-profligacy ./scripts/build_console.sh
```

This avoids repeating the roughly 30-minute foundation build while iterating on a separate MAME tree.

Independent comparison builds can also use separate host build/output paths.

## Firmware revisions in the MAME driver

The MAME fork contains definitions for three Prophecy firmware sets:

```text
korgpro101  → firmware v1.01
korgprop     → firmware v1.7
korgpro20    → firmware v2.0
```

The current Profligacy product, however, explicitly boots:

```text
korgprop
```

and its ROM picker, NVRAM layout, test corpus, and product validation are built around that v1.7 target.

Adding another firmware revision to the product therefore involves more than allowing another filename. It requires validating the editor protocol, state handling, audio behaviour, and other firmware-facing assumptions against that revision.

## Project status

The integration work was developed in progressively stronger validation stages.

### Rung 1 — complete

Link MAME's archives into a non-MAME executable with:

* a Profligacy-owned `main()`;
* custom OSD integration;
* headless Prophecy boot;
* audio egress.

The resulting capture is bit-identical to the corresponding `propmin -wavwrite` path.

### Rung 2 — complete

Add:

* bounded audio ring;
* real-time-paced consumer;
* backpressure-based self-clocking.

The emulator remains at `1.0x` wall-clock pace with zero underruns in the validated path while remaining bit-exact through the ring.

### Rung 3 — implemented

The engine is wrapped in a real `juce::AudioProcessor`, producing AU, VST3, and standalone builds.

Source-equivalent integration builds have passed:

* `auval -v aumu Pflg Prfl`;
* in-host boot;
* format/render tests from 22 kHz through 192 kHz;
* host blocks from 64 through 4096 samples;
* mono output;
* MIDI input;
* multi-object handling;
* teardown/reconstruction testing.

Final release validator receipts must still be generated from the exact release tree required by `release/v1_preflight.json`.

A redistributable clean-room CI ROM also provides a bounded packaged-VST3 gate. It:

* boots both host CPUs;
* uploads programs through the real board ports;
* requires audio to traverse DSP1 → DSP2 → DSP3 → DAC;
* checks an LCD sentinel through the public state API;
* records native-JIT telemetry for each DSP.

This does **not** imply support for multiple simultaneously audible instances.

## Why only one active instance?

The current limitation is architectural rather than primarily a CPU-performance problem.

Two ownership layers are process-global:

1. MAME's `mame_machine_manager::s_manager`;
2. Prophecy host ABI callbacks, rings, and stores.

Supporting independent in-process instances would require de-singletonising both layers and auditing additional MAME/JIT global state.

The intended post-v1 design is instead one service process per plug-in instance.

That would provide instance isolation and crash isolation without requiring Profligacy to maintain a broad de-singletonised MAME fork.

## SysEx editor

The EDIT page combines a clickable signal-flow view with manifest-driven detailed editing.

The catalogue contains all 1,425 currently known SysEx rows and covers all seven oscillator-engine types.

The editor intentionally does not guess unknown or reserved fields.

Persistent text scaling is available through `Aa`.

Signal-flow blocks open focused detailed views, while repeated parameter-name prefixes are presented as nested headings with shorter scan-friendly control names.

The editor catalogue and its provenance are documented in:

```text
src/editor/DATA_PROVENANCE.md
```

The catalogue is based on:

* parameter names and grouping from publicly distributed Korg MIDI/parameter material and editor screenshots;
* measured byte offsets;
* transport behaviour;
* changed-byte previews;
* automated firmware SysEx sweeps;
* explicit confidence metadata.

It contains no Korg firmware.

## Current validation boundary

The public source includes hardware-derived and self-contained validation covering the TMS57002 implementation, interpreter/native equivalence, host integration, editor behaviour, release packaging, and licensing.

Existing exact-audio evidence reaches the DSP3 serial output.

The current U2/DAC/output model does not claim certified recreation of the Prophecy's physical analogue line-output coloration.

That distinction is intentional: digital emulation accuracy and analogue output-stage modelling are treated as separate claims.

## Next

Remaining product work includes:

* host-automation parameters for the curated controls;
* decoding additional unknown SysEx fields;
* packed-byte read-back;
* Developer ID signing and notarisation;
* optional post-v1 out-of-process multi-instance support.

These are product-development tasks rather than blockers for the existing single-engine architecture.

## Wheel 2 position

The Prophecy's Wheel 2 is a free-spinning friction wheel.

Unlike a spring-loaded modulation wheel, it has no centre return or rest detent. Its value is simply wherever the physical wheel was last left.

There is consequently no universal "correct" Wheel 2 value.

Several factory patches route Wheel 2 to level or timbral parameters, so its position can materially affect the sound. During hardware comparison work, an unexpected Wheel 2 position has previously looked like a synthesis discrepancy when the actual difference was simply the physical controller state.

Profligacy exposes Wheel 2 as a persistent control in the MIDI panel.

Its default is:

```text
128
```

which matches the emulator's previous built-in midpoint behaviour.

Known reference capture positions are:

| Session       |               Wheel 2 position |
| ------------- | -----------------------------: |
| 07-04         | approximately midpoint (`128`) |
| 07-10 / 07-14 |                full up (`255`) |
| default       |                          `128` |

To reproduce a particular hardware capture, set Wheel 2 to the position used by that session.

Wheel 2 can also be driven from an external MIDI controller by mapping an incoming CC to the Wheel 2 target.

The persistent position and live CC path both ultimately write the same Prophecy input (`ADIN9`); the most recent write wins.

## License

Profligacy's owned source and embedded editor assets are distributed under the GNU Affero General Public License, version 3 only.

JUCE is used under its AGPLv3 alternative.

Linked MAME components are available under GPL-2.0-or-later or their more permissive per-file licences. GPLv3 and AGPLv3 section 13 permit the combined distribution under the AGPL terms.

See:

```text
THIRD_PARTY_NOTICES.md
```

for the complete third-party licensing inventory.

Korg firmware ROMs are not distributed with Profligacy. Users must supply their own lawfully obtained dumps.

The built-in LCD character table and its provenance are documented in `THIRD_PARTY_NOTICES.md` and `src/editor/assets/README.md`.
