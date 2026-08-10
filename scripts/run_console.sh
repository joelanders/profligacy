#!/usr/bin/env bash
#
# run_console.sh - boot korgprop through ./console_host and capture audio.
#
#   ./scripts/run_console.sh          # Rung 1: accumulate + WAV
#   ./scripts/run_console.sh --ring   # Rung 2: ring buffer + real-time-paced consumer
#
# ROMs/NVRAM live in a sibling ../mame tree using the layout expected by MAME.
#
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

[[ -x ./console_host ]] || { echo "error: ./console_host not built. Run ./scripts/build_console.sh" >&2; exit 1; }

ROMPATH="${ROMPATH:-$REPO/../mame/00-roms}"
NVRAM_DIR="${NVRAM_DIR:-$REPO/../mame/nvram}"
SECONDS_RUN="${SECONDS_RUN:-14}"

if [[ "${1:-}" == "--ring" ]]; then
	export PROPHOST_BLOCK="${PROPHOST_BLOCK:-512}"
	export PROPHOST_RING_FRAMES="${PROPHOST_RING_FRAMES:-8192}"
	export PROPHOST_WAV_OUT="${PROPHOST_WAV_OUT:-console_ring_out.wav}"
	shift
else
	export PROPHOST_WAV_OUT="${PROPHOST_WAV_OUT:-console_out.wav}"
fi

export KPROP_INJECT_NOTE="${KPROP_INJECT_NOTE:-11.0:2.0:60:100}"
export KPROP_LIE_BATTERY_OK="${KPROP_LIE_BATTERY_OK:-1}"
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy

set -x
exec ./console_host korgprop \
	-rompath "$ROMPATH" \
	-nvram_directory "$NVRAM_DIR" \
	-video none -sound none -nothrottle \
	-skip_gameinfo \
	-seconds_to_run "$SECONDS_RUN" \
	"$@"
