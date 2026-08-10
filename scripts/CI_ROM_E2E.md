# Clean-room packaged-product E2E gate

`build_ci_probe_roms.py` generates two small, redistributable firmware images
from source. They contain no Korg firmware. The V55 writes `CI V55+H8 OK` to the
LCD; the H8 exercises SCI0 and uploads three tiny TMS57002 programs through the
real board ports. DSP1 generates a constant, DSP2 forwards it, and DSP3 forwards
it to the DAC. Nonzero final audio therefore depends on all three DSPs executing.

Run the gate against the packaged VST3, not an in-tree processor target:

```bash
python3 -B scripts/run_ci_rom_e2e.py \
  --artifact-host build/ProphecyArtifactHost_artefacts/Release/ProphecyArtifactHost \
  --plugin build/ProphecyPlugin_artefacts/Release/VST3/Profligacy.vst3 \
  --work /tmp/profligacy-ci-rom-e2e \
  --seconds 6
```

On Windows, use the corresponding `.exe` host and VST3 bundle paths. The runner
sets isolated ROM/NVRAM directories and real-time pacing, then retains a WAV,
host log, host receipt, wrapped VST3 state capture, and combined JSON receipt.
Success requires:

- exact LCD sentinel recovery through the packaged plug-in's VST3 state API;
- finite, nonzero final audio;
- unambiguous DSP1, DSP2, and DSP3 telemetry, each with native frame compiles and
  executions.

The marker is exposed only when `PROFLIGACY_CI_EXPOSE_LCD_STATE` is set. Normal
plug-in state bytes are unchanged. The synthetic state capture contains no user
firmware or private patch data.

This is an execution-path sentinel, not exhaustive instruction verification.
The MAME fork separately retains a factory-reachable JIT coverage receipt. Its
current normalized manifest has 97 instruction forms and 94 legal tuples,
categorized as native, intentional fallback, or untested; CI fails if an untested
case appears or the native-policy predicate changes without review.

## CI policy

The pure ROM-generator and telemetry-parser tests run in the fast macOS/Windows
product-shell workflow. Full MAME compilation and this real packaged-product gate
belong in an explicitly dispatched release-candidate build (and once per release
commit), not every product pull request. Reuse exact, revision-stamped MAME archive
artifacts when those are available; never silently substitute archives from a
different MAME commit.
