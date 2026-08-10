#!/usr/bin/env python3
"""Compare clean and reference MAME across plugin control/faceplate seams."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


MIDI_FIXTURE = (
    "f0 42 30 41 41 01 01 00 5a 00 f7@12.0 ; "
    "f0 42 30 41 41 01 02 00 41 00 f7@12.1 ; "
    "f0 42 30 41 41 01 03 00 50 00 f7@12.2 ; "
    "f0 42 30 41 41 00 2a 01 00 00 f7@12.5 ; "
    "f0 42 30 41 41 00 2a 01 01 00 f7@14.2"
)

BASE_ENV = {
    "SDL_VIDEODRIVER": "dummy",
    "SDL_AUDIODRIVER": "dummy",
    "KPROP_LIE_BATTERY_OK": "1",
    "KPROP_DISABLE_AUTO_STIM_SWEEP": "1",
    "KPROP_V55_ADC_BANKSW_EXPERIMENT": "6",
    "KPROP_ADC_MUX_FROM_P2": "0",
    "KPROP_ADC_MUX_FROM_SHADOW": "1",
    "KPROP_ADC_MUX_AUTOSCAN": "0",
    "KPROP_DSP_HOST_MAP": "1,2,3",
    "KPROP_DSP2_INPUT_ROUTE": "normal",
    "KPROP_TXSM_FALLBACK_FIX": "1",
    "KPROP_DSP_SERIAL_FRAME_MODEL": "0",
    "PROPHOST_BLOCK": "512",
    "PROPHOST_RING_FRAMES": "8192",
    "PROPHOST_WAV_OUT": "/dev/null",
    "PROPHOST_TEST_MIDI": MIDI_FIXTURE,
    "PROPHOST_ADIN_AT": "10.0:0,128",
    "PROPHOST_PANEL_AT": "13.0:0,0;13.4:1,6;13.8:1,6",
    "PROPHOST_LCD_AT": "15.0",
    "PROPHOST_DUMP_AT": "15.5",
}

STABLE_PREFIXES = (
    "[console] pushAdin ",
    "[console] LEDS ",
    "[console] panelPulse ",
    "[console] LCD@",
    "[console] LCDRAW ",
    "[console] TX seam: ",
    "[console] scheduled-midi-dropped=",
    "[console] scheduled-control-dropped=",
)


def existing_file(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"file not found: {path}")
    return path


def existing_dir(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_dir():
        raise argparse.ArgumentTypeError(f"directory not found: {path}")
    return path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_sysex(midi_path: Path) -> list[list[int]]:
    stream = [int(json.loads(line)["byte"])
              for line in midi_path.read_text(encoding="utf-8").splitlines()]
    messages: list[list[int]] = []
    start: int | None = None
    for index, byte in enumerate(stream):
        if byte == 0xF0:
            start = index
        elif byte == 0xF7 and start is not None:
            messages.append(stream[start:index + 1])
            start = None
    return messages


def run_host(binary: Path, label: str, output: Path, rompath: Path,
             nvram_seed: Path) -> dict[str, object]:
    run_dir = output / label
    nvram_dir = run_dir / "nvram"
    bank = nvram_dir / "korgprop" / "sysram"
    bank.parent.mkdir(parents=True)
    shutil.copy2(nvram_seed, bank)
    midi_path = run_dir / "midi.jsonl"
    wav_path = run_dir / "control.wav"
    log_path = run_dir / "console.log"

    environment = os.environ.copy()
    environment.pop("KPROP_INJECT_NOTE", None)
    environment.update(BASE_ENV)
    environment["PROPHOST_MIDI_TX_OUT"] = str(midi_path)
    environment["PROPHOST_WAV_OUT"] = str(wav_path)
    command = [
        str(binary), "korgprop", "-rompath", str(rompath),
        "-nvram_directory", str(nvram_dir),
        "-video", "none", "-sound", "none", "-videodriver", "dummy",
        "-nothrottle", "-skip_gameinfo", "-seconds_to_run", "18",
    ]
    with log_path.open("w", encoding="utf-8") as log:
        completed = subprocess.run(command, cwd=run_dir, env=environment,
                                   stdout=log, stderr=subprocess.STDOUT,
                                   check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{label} exited {completed.returncode}; see {log_path}")
    if not midi_path.is_file() or not bank.is_file() or not wav_path.is_file():
        raise RuntimeError(f"{label} did not produce audio, MIDI, and NVRAM artifacts")

    log_lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    stable = [line for line in log_lines if line.startswith(STABLE_PREFIXES)]
    observer = next((re.search(r"midi-tx-events=(\d+) clocks=(\d+) dropped=(\d+)", line)
                     for line in log_lines if "midi-tx-events=" in line), None)
    underrun = next((re.search(
        r"underrun-detail total_callbacks=(\d+) total_frames=(\d+).*"
        r"post_warmup_callbacks=(\d+) post_warmup_frames=(\d+)", line)
        for line in log_lines if "underrun-detail" in line), None)
    messages = parse_sysex(midi_path)
    current_dumps = [message for message in messages
                     if len(message) >= 5 and message[4] == 0x40]
    bank_bytes = bank.read_bytes()
    name = bank_bytes[0x20A10:0x20A20].decode("latin1")
    invariants = {
        "adin_applied": sum(line.startswith("[console] pushAdin ") for line in stable) == 1,
        "panel_pulses": sum(line.startswith("[console] panelPulse ") for line in stable) == 3,
        "led_snapshots": sum(line.startswith("[console] LEDS ") for line in stable) == 2,
        "lcd_text": any(" ok=1 " in line for line in stable if line.startswith("[console] LCD@")),
        "lcd_raw": any(line.startswith("[console] LCDRAW ") for line in stable),
        "program_written": name.startswith("ZAP"),
        "current_program_dump": len(current_dumps) == 1 and len(current_dumps[0]) > 500,
        "midi_observer_no_drop": observer is not None and int(observer.group(3)) == 0,
        "scheduled_midi_no_drop": "[console] scheduled-midi-dropped=0" in stable,
        "scheduled_control_no_drop": (
            "[console] scheduled-control-dropped=panel:0 adin:0" in stable
        ),
        "runtime_self_clocked": any("VERDICT: SELF-CLOCKED" in line for line in log_lines),
        "audio_no_underrun": (
            underrun is not None and all(int(underrun.group(i)) == 0 for i in range(1, 5))
        ),
    }
    return {
        "label": label,
        "stable_events": stable,
        "invariants": invariants,
        "name": name,
        "nvram_sha256": sha256(bank),
        "midi_sha256": sha256(midi_path),
        "wav_sha256": sha256(wav_path),
        "midi_event_count": len(midi_path.read_text(encoding="utf-8").splitlines()),
        "sysex_count": len(messages),
        "current_dump_length": len(current_dumps[0]) if current_dumps else 0,
        "artifacts": {
            "log": str(log_path), "midi": str(midi_path),
            "nvram": str(bank), "wav": str(wav_path),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference-console", type=existing_file, required=True)
    parser.add_argument("--candidate-console", type=existing_file, required=True)
    parser.add_argument("--rompath", type=existing_dir, required=True)
    parser.add_argument("--nvram-seed", type=existing_file, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    output = args.out.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)
    try:
        reference = run_host(args.reference_console, "reference", output,
                             args.rompath, args.nvram_seed)
        candidate = run_host(args.candidate_console, "candidate", output,
                             args.rompath, args.nvram_seed)
    except (OSError, RuntimeError) as error:
        print(f"ERROR control_equivalence {error}")
        return 2

    checks = {
        "reference_invariants": all(reference["invariants"].values()),
        "candidate_invariants": all(candidate["invariants"].values()),
        "stable_events_identical": reference["stable_events"] == candidate["stable_events"],
        "midi_identical": reference["midi_sha256"] == candidate["midi_sha256"],
        "audio_identical": reference["wav_sha256"] == candidate["wav_sha256"],
        "nvram_identical": reference["nvram_sha256"] == candidate["nvram_sha256"],
        "program_name_identical": reference["name"] == candidate["name"],
        "sysex_shape_identical": (
            reference["sysex_count"] == candidate["sysex_count"] and
            reference["current_dump_length"] == candidate["current_dump_length"]
        ),
    }
    passed = all(checks.values())
    receipt = {
        "schema": 1,
        "passed": passed,
        "checks": checks,
        "inputs": {
            "reference_console": {"path": str(args.reference_console),
                                  "sha256": sha256(args.reference_console)},
            "candidate_console": {"path": str(args.candidate_console),
                                  "sha256": sha256(args.candidate_console)},
            "nvram_seed": {"path": str(args.nvram_seed),
                           "sha256": sha256(args.nvram_seed)},
            "rompath": str(args.rompath),
        },
        "reference": reference,
        "candidate": candidate,
    }
    receipt_path = output / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                            encoding="utf-8")
    for name, value in checks.items():
        print(f"{'PASS' if value else 'FAIL'} {name}")
    print(f"receipt: {receipt_path}")
    print("CONTROL_EQUIVALENCE_PASS" if passed else "CONTROL_EQUIVALENCE_FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    raise SystemExit(main())
