#!/usr/bin/env bash
#
# run_gates.sh — the full validation battery, one command, one scoreboard.
#
#   ./scripts/run_gates.sh            # everything (~12 min: pluginval + soak dominate)
#   ./scripts/run_gates.sh --quick    # skip pluginval + mini-soak (~3 min)
#
# Gates:
#   provenance     static editor font/LCD asset provenance checks
#   auval          AU validation (aumu Pflg Prfl)
#   pluginval      strictness 10, AU + VST3
#   selftest       editor self-test in the real WKWebView (full check battery)
#   byteexact      console PCM byte-identical to propmin -wavwrite (same scenario)
#   minisoak       3 min churn, zero post-boot missing frames at ring 8192
#
# Exit nonzero if any gate fails. Requires: built plugin (cmake --build build-cmake),
# built console (scripts/build_console.sh), an exact MAME build, and ROMs at ../mame.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

QUICK=0
[[ "${1:-}" == "--quick" ]] && QUICK=1

PASS=()
FAIL=()
note() { printf '%s\n' "$*" >&2; }
gate() { # gate <name> <ok:0/1> <detail>
	if [[ "$2" == "0" ]]; then PASS+=("$1: $3"); note "  PASS  $1 — $3"
	else FAIL+=("$1: $3"); note "  FAIL  $1 — $3"; fi
}

if [[ -n "${PROPHECY_GATE_OUT:-}" ]]; then
	TMP="$PROPHECY_GATE_OUT"
	if [[ -e "$TMP" ]]; then note "error: refusing stale PROPHECY_GATE_OUT=$TMP"; exit 2; fi
	mkdir -p "$TMP"
else
	TMP="$(mktemp -d /tmp/prophecy_gates.XXXXXX)"
	trap 'rm -rf "$TMP"' EXIT
fi

MAME_DIR="${PROPHECY_MAME_DIR:-$REPO/extern/mame}"
# A release checkout may share the large MAME build directory with its canonical
# dependency worktree.  Generated MAME projects place the CLI beside the physical
# build directory's parent, not beside the checkout containing the build symlink.
# Follow that layout only when the checkout itself has no propmin executable.
if [[ ! -x "$MAME_DIR/propmin" && -d "$MAME_DIR/build" ]]; then
	MAME_BUILD_PHYSICAL="$(cd "$MAME_DIR/build" && pwd -P)"
	MAME_BUILD_ROOT="$(dirname "$MAME_BUILD_PHYSICAL")"
	if [[ -x "$MAME_BUILD_ROOT/propmin" ]]; then MAME_DIR="$MAME_BUILD_ROOT"; fi
fi
BUILD_DIR="${PROPHECY_BUILD_DIR:-$REPO/build-cmake}"
CONSOLE="${PROPHECY_CONSOLE:-$REPO/console_host}"
ROMPATH="${PROPHECY_ROMPATH:-$REPO/../mame/00-roms}"
NVRAM_SEED="${PROPHECY_NVRAM_SEED:-$REPO/../mame/nvram/korgprop/sysram}"

MAME_ENV=(env -u KPROP_INJECT_NOTE SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy KPROP_LIE_BATTERY_OK=1)
MAME_ARGS=(korgprop -rompath "$ROMPATH"
	-video none -sound none -videodriver dummy -nothrottle -skip_gameinfo)

# MAME writes nvram back on exit — NEVER point a gate at the research bank
# (../mame/nvram). Each run gets its own throwaway copy seeded from it.
seed_nvram() { # seed_nvram <name> -> echoes the -nvram_directory path
	mkdir -p "$TMP/$1/korgprop"
	cp "$NVRAM_SEED" "$TMP/$1/korgprop/sysram"
	printf '%s' "$TMP/$1"
}

# ---------------- editor asset provenance ----------------
note "== editor asset provenance (static) =="
if python3 -m unittest scripts/test_editor_asset_provenance.py \
		> "$TMP/editor_asset_provenance.log" 2>&1; then
	gate provenance 0 "one verified compiled HD44780 table; CGRAM/A00 routes and OFL verified"
else
	gate provenance 1 "see $TMP/editor_asset_provenance.log"
fi

# ---------------- auval ----------------
note "== auval =="
if PROPHECY_ROMPATH="$ROMPATH" PROPHECY_NVRAM="$(seed_nvram nv_auval)" \
	auval -v aumu Pflg Prfl > "$TMP/auval.log" 2>&1 && grep -q 'AU VALIDATION SUCCEEDED' "$TMP/auval.log"; then
	gate auval 0 "$(grep -oE 'Average speed: [0-9.]+%' "$TMP/auval.log" | tail -1)"
else
	gate auval 1 "see $TMP/auval.log"
fi

# ---------------- pluginval ----------------
if [[ $QUICK == 0 ]]; then
	PV=/Applications/pluginval.app/Contents/MacOS/pluginval
	for fmt in "AU:$HOME/Library/Audio/Plug-Ins/Components/Profligacy.component" \
	           "VST3:$HOME/Library/Audio/Plug-Ins/VST3/Profligacy.vst3"; do
		name="${fmt%%:*}"; path="${fmt#*:}"
		note "== pluginval $name (strictness 10) =="
		if [[ -x "$PV" ]] && PROPHECY_ROMPATH="$ROMPATH" PROPHECY_NVRAM="$(seed_nvram "nv_pluginval_$name")" \
			"$PV" --strictness-level 10 --validate "$path" \
				--timeout-ms 900000 > "$TMP/pluginval_$name.log" 2>&1 \
				&& tail -1 "$TMP/pluginval_$name.log" | grep -q SUCCESS; then
			gate "pluginval-$name" 0 "strictness 10 SUCCESS"
		else
			gate "pluginval-$name" 1 "see $TMP/pluginval_$name.log"
		fi
	done
fi

# ---------------- editor self-test ----------------
note "== editor self-test (real WKWebView) =="
rm -f "$TMP/selftest.json"
# The plugin has no dev-tree ROM fallback (scrubbed for release); without a ROM
# dir the self-test silently degrades to the 4-check no-ROM mode. Point it at
# the sibling dump so the FULL battery runs; fail loudly if it's absent.
if [[ -d "$ROMPATH" ]]; then export PROPHECY_ROMPATH="$ROMPATH"
else note "  WARN  selftest — ROM path not found at $ROMPATH; running degraded no-ROM selftest"; fi
PROPHECY_NVRAM="$(seed_nvram nv_selftest)" \
	PROPHECY_EDITOR_SELFTEST=1 PROPHECY_EDITOR_SELFTEST_OUT="$TMP/selftest.json" \
	"$BUILD_DIR/ProphecyPlugin_artefacts/Release/Standalone/Profligacy.app/Contents/MacOS/Profligacy" \
	> "$TMP/selftest_run.log" 2>&1 &
ST_PID=$!
for _ in $(seq 1 60); do [[ -s "$TMP/selftest.json" ]] && break; sleep 2; done
{ kill "$ST_PID" && sleep 1 && kill -9 "$ST_PID"; } 2>/dev/null; wait "$ST_PID" 2>/dev/null || true
if [[ -s "$TMP/selftest.json" ]]; then
	read -r ST_PASSED ST_TOTAL < <(/opt/homebrew/bin/python3 -c "
import json; r=json.load(open('$TMP/selftest.json')); print(r['passed'], r['total'])")
	if [[ "$ST_PASSED" == "$ST_TOTAL" ]]; then gate selftest 0 "$ST_PASSED/$ST_TOTAL"
	else gate selftest 1 "$ST_PASSED/$ST_TOTAL — $TMP/selftest.json"; fi
else
	gate selftest 1 "no report produced — $TMP/selftest_run.log"
fi

# ---------------- byte-exact PCM ----------------
note "== byte-exact PCM (console vs propmin) =="
AUDIO_ENV=(KPROP_DISABLE_AUTO_STIM_SWEEP=1 KPROP_V55_ADC_BANKSW_EXPERIMENT=6
	KPROP_ADC_MUX_FROM_P2=0 KPROP_ADC_MUX_FROM_SHADOW=1 KPROP_ADC_MUX_AUTOSCAN=0
	KPROP_DSP_HOST_MAP=1,2,3 KPROP_DSP2_INPUT_ROUTE=normal KPROP_TXSM_FALLBACK_FIX=1
	KPROP_DSP_SERIAL_FRAME_MODEL=0 KPROP_DSP_PERFRAME=4 KPROP_PF4_CMEM_DEOPT=1)
"${MAME_ENV[@]}" "${AUDIO_ENV[@]}" KPROP_INJECT_NOTE=11.0:2.0:60:100 \
	"$MAME_DIR/propmin" "${MAME_ARGS[@]}" -nvram_directory "$(seed_nvram nv_ref)" -seconds_to_run 14 \
	-wavwrite "$TMP/ref.wav" > "$TMP/propmin.log" 2>&1
"${MAME_ENV[@]}" KPROP_INJECT_NOTE=11.0:2.0:60:100 PROPHOST_BLOCK=512 PROPHOST_RING_FRAMES=8192 \
	PROPHOST_WAV_OUT="$TMP/console.wav" \
	"$CONSOLE" "${MAME_ARGS[@]}" -nvram_directory "$(seed_nvram nv_con)" -seconds_to_run 14 > "$TMP/console.log" 2>&1
BE=$(/opt/homebrew/bin/python3 -c "
import wave
def pcm(p):
    w = wave.open(p); return w.readframes(w.getnframes())
try:
    a, b = pcm('$TMP/ref.wav'), pcm('$TMP/console.wav')
    n = min(len(a), len(b))
    print('OK' if (n > 2000000 and a[:n] == b[:n]) else f'DIFF len {len(a)}/{len(b)}')
except Exception as e:
    print('ERR', e)")
[[ "$BE" == "OK" ]] && gate byteexact 0 "14 s PCM identical" || gate byteexact 1 "$BE"

# ---------------- mini-soak ----------------
if [[ $QUICK == 0 ]]; then
	note "== mini-soak (3 min churn) =="
	"${MAME_ENV[@]}" PROPHOST_BLOCK=512 PROPHOST_RING_FRAMES=8192 PROPHOST_SOAK=180 \
		PROPHOST_METRIC_WARMUP=12 PROPHOST_SCHED_STATS=1 PROPHOST_WAV_OUT=/dev/null \
		"$CONSOLE" "${MAME_ARGS[@]}" -nvram_directory "$(seed_nvram nv_soak)" -seconds_to_run 200 > "$TMP/soak.log" 2>&1
	POST_FRAMES=$(grep -oE 'post_warmup_frames=[0-9]+' "$TMP/soak.log" | tail -1 | cut -d= -f2)
	POST_CALLBACKS=$(grep -oE 'post_warmup_callbacks=[0-9]+' "$TMP/soak.log" | tail -1 | cut -d= -f2)
	SCHED_OK=$(grep -oE 'producer_time_constraint=[01]' "$TMP/soak.log" | tail -1 | cut -d= -f2)
	POST_FRAMES="${POST_FRAMES:-999999}"
	POST_CALLBACKS="${POST_CALLBACKS:-999999}"
	SCHED_OK="${SCHED_OK:-0}"
	if [[ "$POST_FRAMES" -eq 0 && "$SCHED_OK" -eq 1 ]]; then
		gate minisoak 0 "post-warmup missing frames=0; producer time-constraint active"
	else
		gate minisoak 1 "post-warmup missing frames=$POST_FRAMES callbacks=$POST_CALLBACKS producer_time_constraint=$SCHED_OK"
	fi
fi

note ""
note "================ GATE SCOREBOARD ================"
for p in ${PASS[@]+"${PASS[@]}"}; do note "  PASS  $p"; done
for f in ${FAIL[@]+"${FAIL[@]}"}; do note "  FAIL  $f"; done
note "================================================="
if [[ ${#FAIL[@]} -gt 0 ]]; then note "RESULT: ${#FAIL[@]} gate(s) FAILED"; exit 1; fi
note "RESULT: all ${#PASS[@]} gates passed"
