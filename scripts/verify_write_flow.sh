#!/usr/bin/env bash
#
# verify_write_flow.sh — prove the preset write path end-to-end, on a THROWAWAY bank.
#
# Drives exactly what ProphecyAudioProcessor::writePatch() does, over the console
# harness: rename the edit buffer (Program Name Char = g1 p1..p3), unprotect
# (global p170 = 0), WRITE (panel row 0 bit 0), ENTER (row 1 bit 6) — then reads the
# name back out of the resulting sysram file.
#
# The firmware refuses a protected write ("*WRITE ERROR<ProgramMemory is protected>"),
# which is why the unprotect step is mandatory. Never points at the research nvram.
set -uo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

NV="$(mktemp -d /tmp/prophecy_write.XXXXXX)"
trap 'rm -rf "$NV"' EXIT
mkdir -p "$NV/korgprop"
cp ../mame/nvram/korgprop/sysram "$NV/korgprop/sysram"

# 'Z','A','P' into name chars 1..3 @12.0-12.2s, unprotect @12.5s, WRITE+ENTER @13.0/13.5s
env -u KPROP_INJECT_NOTE SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy KPROP_LIE_BATTERY_OK=1 \
	PROPHOST_BLOCK=512 PROPHOST_RING_FRAMES=8192 PROPHOST_WAV_OUT="$NV/out.wav" \
	PROPHOST_TEST_MIDI='f0 42 30 41 41 01 01 00 5a 00 f7@12.0 ; f0 42 30 41 41 01 02 00 41 00 f7@12.1 ; f0 42 30 41 41 01 03 00 50 00 f7@12.2 ; f0 42 30 41 41 00 2a 01 00 00 f7@12.5' \
	PROPHOST_PANEL_AT='13:0,0;1,6' PROPHOST_LCD_AT=14.2 \
	./console_host korgprop -rompath ../mame/00-roms -nvram_directory "$NV" \
	-video none -sound none -nothrottle -skip_gameinfo -seconds_to_run 18 \
	> "$NV/run.log" 2>&1
grep -E 'LCD@' "$NV/run.log" >&2 || true

/opt/homebrew/bin/python3 - "$NV/korgprop/sysram" <<'PY'
import sys
BASE = 0x20A10
name = open(sys.argv[1], 'rb').read()[BASE:BASE + 16].decode('latin1')
print(f"A00 name after write: {name!r}")
ok = name.startswith('ZAP')
print("PASS: write flow reached sysram" if ok else "FAIL: name not written")
sys.exit(0 if ok else 1)
PY
