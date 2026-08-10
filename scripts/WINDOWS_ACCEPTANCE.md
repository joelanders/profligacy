# Windows packaged-artifact acceptance

These gates test copied shipping artifacts rather than an in-tree plug-in target. Use a
new `RunId` for every build; the runner refuses to overwrite an existing evidence set.

First clean-rebuild native MAME, then force a clean product relink:

```powershell
C:\BuildTools\MSBuild\Current\Bin\MSBuild.exe `
  extern\mame\build\projects\windows\mamepropmin\vs2022-clang\propmin.vcxproj `
  /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m:8

C:\msys64\ucrt64\bin\cmake.exe --build build-windows --config Release `
  --target ProphecyPlugin_VST3 ProphecyPlugin_Standalone ProphecyArtifactHost `
  --clean-first -j 8
```

MAME names the generated ClangCL project directory `vs2022-clang`, while its
libraries and objects are emitted below `extern\mame\build\vs2022`.

Run the no-ROM API/host gates plus pluginval. Pass a separately built official
Steinberg validator when available:

```powershell
.\scripts\run_windows_artifact_acceptance.ps1 `
  -RunId win-x64-<source>-<mame>-01 `
  -SteinbergValidator C:\tools\vst3-validator\validator.exe
```

For the product audio gate, point at a private MAME-layout ROM directory and an
isolated writable NVRAM directory:

```powershell
.\scripts\run_windows_artifact_acceptance.ps1 `
  -RunId win-x64-<source>-<mame>-02 `
  -SteinbergValidator C:\tools\vst3-validator\validator.exe `
  -RomRoot C:\Temp\private-roms `
  -NvramRoot C:\Temp\private-nvram
```

The ROM run is paced in real time because Profligacy's emulator producer is
asynchronous; an unpaced host can outrun it and record misleading silence. This script
never deletes the supplied private directories. The caller must remove and verify them
after collecting sanitized receipts.

For routine packaged-binary E2E, no private ROM is needed. Run the clean-room
three-DSP sentinel after the build above:

```powershell
python -B scripts\run_ci_rom_e2e.py `
  --artifact-host build-windows\ProphecyArtifactHost_artefacts\Release\ProphecyArtifactHost.exe `
  --plugin build-windows\ProphecyPlugin_artefacts\Release\VST3\Profligacy.vst3 `
  --work C:\profligacy-acceptance\synthetic-e2e `
  --seconds 6
```

This verifies packaged loading, both host CPUs, the DSP1 -> DSP2 -> DSP3 audio
chain, DAC output, and per-DSP native JIT execution. Keep the private-ROM run for
release evidence involving real firmware; it tests a different claim.

REAPER is an additional host test, not a replacement for pluginval or Steinberg's
validator. Install a pinned portable copy, then use the staged run from above:

```powershell
.\scripts\install_windows_reaper.ps1
.\scripts\run_windows_reaper_acceptance.ps1 `
  -Run C:\profligacy-acceptance\win-x64-<source>-<mame>-02 `
  -RomRoot C:\Temp\private-roms `
  -NvramRoot C:\Temp\private-nvram-reaper
```

The REAPER gate is SSH/Session-0 safe: it handles the first-run no-audio-device prompt,
scans and instantiates the staged VST3, creates a MIDI project, forces a 1x render,
checks the WAV for finite nonzero audio, restores plug-in state, and creates a second
instance. No RDP client is needed.

A visual editor gate is different. Session 0 can create the JUCE/browser window but
Windows returns black pixels when it is captured there. Screenshot and pointer/keyboard
testing therefore require an interactive desktop session and should be kept separate
from routine build-server acceptance.
