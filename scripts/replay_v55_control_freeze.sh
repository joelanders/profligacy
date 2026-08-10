#!/usr/bin/env bash
# Replay the UI control history captured from the persistent 97854 freeze.
# The console sorts the resulting scheduled MIDI stream by emulated timestamp.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run_dir="${1:-$(mktemp -d /private/tmp/profligacy-v55-replay.XXXXXX)}"
run_seconds="${2:-92}"
rom_path="${PROFLIGACY_REPLAY_ROMPATH:-$repo_dir/roms}"
mkdir -p "$run_dir/cfg" "$run_dir/nvram" "$run_dir/snap" "$run_dir/mame-log"

replay_midi=""
add_event() {
	if [[ -n "$replay_midi" ]]; then replay_midi+=" ; "; fi
	replay_midi+="$1@$2"
}
patch_change() {
	local time="$1" program="$2" program_hex
	printf -v program_hex '%02x' "$program"
	add_event "b0 00 00 b0 20 00 c0 $program_hex" "$time"
}
set_param() {
	local time="$1" param="$2" value="$3"
	local p_lo=$((param & 127)) p_hi=$(((param >> 7) & 127))
	local v_lo=$((value & 127)) v_hi=$(((value >> 7) & 127))
	local p_lo_hex p_hi_hex v_lo_hex v_hi_hex
	printf -v p_lo_hex '%02x' "$p_lo"
	printf -v p_hi_hex '%02x' "$p_hi"
	printf -v v_lo_hex '%02x' "$v_lo"
	printf -v v_hi_hex '%02x' "$v_hi"
	add_event "f0 42 30 41 41 01 $p_lo_hex $p_hi_hex $v_lo_hex $v_hi_hex f7" "$time"
}

patch_change 14.974 1
patch_change 17.844 2
patch_change 21.990 3
patch_change 23.628 4
patch_change 26.694 3
patch_change 27.016 2
patch_change 27.344 1
patch_change 28.432 0

set_param 34.291 144 31
set_param 34.407 144 32
set_param 34.507 144 92
set_param 34.607 144 146
set_param 34.708 144 175
set_param 34.808 144 199
set_param 35.882 131 119
set_param 36.082 131 120
set_param 36.183 131 194
set_param 36.284 131 199
set_param 37.342 118 159
set_param 37.486 118 160
set_param 37.585 118 199
set_param 38.575 105 62
set_param 38.690 105 63
set_param 38.790 105 199
set_param 45.024 371 35
set_param 49.300 364 65
set_param 49.556 364 66
set_param 49.657 364 78
set_param 49.758 364 99
set_param 51.115 364 93
set_param 51.302 364 92
set_param 51.401 364 75
set_param 51.502 364 0
set_param 52.547 365 61
set_param 52.677 365 62
set_param 52.777 365 83
set_param 52.878 365 99
set_param 54.678 364 6
set_param 55.944 364 7
set_param 56.044 364 11
set_param 56.143 364 12
set_param 56.244 364 13
set_param 56.344 364 14
set_param 57.327 364 15
set_param 57.427 364 18
set_param 58.568 365 94
set_param 58.869 365 93
set_param 58.970 365 92
set_param 59.069 365 91
set_param 59.169 365 89
set_param 59.270 365 87
set_param 59.402 365 86
set_param 62.669 355 70
set_param 62.844 355 71
set_param 62.945 355 99
set_param 64.267 356 35
set_param 64.440 356 36
set_param 64.541 356 46
set_param 64.648 356 99
set_param 67.653 365 82
set_param 67.831 365 81
set_param 67.932 365 76
set_param 68.032 365 64
set_param 68.132 365 56
set_param 68.232 365 51
set_param 68.332 365 47
set_param 68.432 365 38
set_param 68.528 365 36
set_param 69.103 364 31
set_param 69.203 364 30
set_param 69.303 364 29
set_param 69.404 364 17
set_param 69.504 364 11
set_param 69.605 364 0
set_param 71.307 355 70
set_param 71.483 355 69
set_param 71.583 355 57
set_param 71.683 355 47
set_param 71.750 355 35
patch_change 78.865 1
patch_change 80.182 2
patch_change 81.661 3

# Approximate the original continuous playing load. The GUI diagnostic retained
# counts and last messages, not every MIDI event, so use deterministic overlapping
# three-note chords across the same interval.
for second in $(seq 11 86); do
	root=$((45 + (second % 12)))
	velocity=$((72 + (second % 36)))
	printf -v root_hex '%02x' "$root"
	printf -v third_hex '%02x' "$((root + 4))"
	printf -v fifth_hex '%02x' "$((root + 7))"
	printf -v velocity_hex '%02x' "$velocity"
	add_event "90 $root_hex $velocity_hex 90 $third_hex $velocity_hex 90 $fifth_hex $velocity_hex" "$second.100"
	add_event "80 $root_hex 40 80 $third_hex 40 80 $fifth_hex 40" "$second.650"
done

echo "[replay] output: $run_dir" >&2
cd "$run_dir"
env \
	PROPHOST_BLOCK=512 \
	PROPHOST_FAST_RING=1 \
	PROPHOST_TEST_MIDI="$replay_midi" \
	PROPHOST_WAV_OUT="$run_dir/replay.wav" \
	KPROP_LIE_BATTERY_OK=1 \
	SDL_VIDEODRIVER=dummy \
	SDL_AUDIODRIVER=dummy \
	"$repo_dir/console_host-irqq" korgprop \
		-rompath "$rom_path" \
		-cfg_directory "$run_dir/cfg" \
		-nvram_directory "$run_dir/nvram" \
		-snapshot_directory "$run_dir/snap" \
		-log \
		-video none -sound none -nothrottle -skip_gameinfo \
		-seconds_to_run "$run_seconds" \
		2>"$run_dir/console.log"
