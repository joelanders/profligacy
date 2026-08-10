#!/usr/bin/env python3
"""Run the fixed current-MAME versus clean-MAME product PCM gate.

The gate exercises both MAME's direct WAV writer and prophecy-plugin's threaded
console host.  Every run receives an isolated copy of the same settled NVRAM
seed, the same shipping KPROP configuration, and the same injected note.

Expected comparisons:

* reference propmin == reference threaded console
* candidate interpreter == candidate pooled dynarec
* candidate pooled dynarec == candidate threaded console
* reference pooled dynarec == candidate pooled dynarec

Any mismatch remains a failing gate and is described in receipt.json.  The
script never updates a golden or the checked-in MAME submodule pointer.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import wave


SHIPPING_ENV = {
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
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_wav(path: Path) -> tuple[bytes, dict[str, int | str]]:
    with wave.open(str(path), "rb") as handle:
        metadata: dict[str, int | str] = {
            "frames": handle.getnframes(),
            "sample_rate": handle.getframerate(),
            "channels": handle.getnchannels(),
            "sample_width": handle.getsampwidth(),
        }
        pcm = handle.readframes(handle.getnframes())
    metadata["pcm_bytes"] = len(pcm)
    metadata["pcm_sha256"] = hashlib.sha256(pcm).hexdigest()
    metadata["wav_sha256"] = sha256(path)
    return pcm, metadata


def first_difference(left: bytes, right: bytes, channels: int, width: int) -> dict[str, int | float | None]:
    first = next((index for index, pair in enumerate(zip(left, right)) if pair[0] != pair[1]), None)
    frame_bytes = channels * width
    frame = None if first is None else first // frame_bytes
    return {
        "first_byte": first,
        "first_frame": frame,
        "first_seconds": None if frame is None else frame / 48000.0,
        "left_bytes": len(left),
        "right_bytes": len(right),
    }


def clean_environment() -> dict[str, str]:
    environment = os.environ.copy()
    for key in list(environment):
        if key.startswith("KPROP_") or key.startswith("PROPHOST_"):
            environment.pop(key)
    environment.update(SHIPPING_ENV)
    environment.update({"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
    return environment


def run_case(
    *,
    name: str,
    binary: Path,
    is_console: bool,
    perframe: str,
    args: argparse.Namespace,
) -> Path:
    case_dir = args.out / name
    nvram = case_dir / "nvram"
    sysram = nvram / "korgprop" / "sysram"
    sysram.parent.mkdir(parents=True)
    shutil.copy2(args.nvram_seed, sysram)
    wav_path = case_dir / f"{name}.wav"
    log_path = case_dir / f"{name}.log"

    environment = clean_environment()
    environment["KPROP_DSP_PERFRAME"] = perframe
    environment["KPROP_INJECT_NOTE"] = args.note
    if is_console:
        environment.update(
            {
                "PROPHOST_BLOCK": str(args.block),
                "PROPHOST_RING_FRAMES": str(args.ring_frames),
                "PROPHOST_WAV_OUT": str(wav_path),
            }
        )

    command = [
        str(binary),
        "korgprop",
        "-rompath",
        str(args.rompath),
        "-nvram_directory",
        str(nvram),
        "-seconds_to_run",
        str(args.seconds),
        "-video",
        "none",
        "-sound",
        "none",
        "-videodriver",
        "dummy",
        "-nothrottle",
        "-skip_gameinfo",
    ]
    if not is_console:
        command.extend(("-wavwrite", str(wav_path)))

    started = time.monotonic()
    with log_path.open("wb") as log:
        completed = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT, check=False)
    elapsed = time.monotonic() - started
    print(f"{name}: rc={completed.returncode} wall={elapsed:.3f}s", flush=True)
    if completed.returncode:
        raise RuntimeError(f"{name} exited {completed.returncode}; see {log_path}")
    if not wav_path.is_file():
        raise RuntimeError(f"{name} did not create {wav_path}; see {log_path}")
    return wav_path


def existing_file(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError(f"not a file: {path}")
    return path


def existing_dir(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_dir():
        raise argparse.ArgumentTypeError(f"not a directory: {path}")
    return path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-propmin", required=True, type=existing_file)
    parser.add_argument("--reference-console", required=True, type=existing_file)
    parser.add_argument("--candidate-propmin", required=True, type=existing_file)
    parser.add_argument("--candidate-console", required=True, type=existing_file)
    parser.add_argument("--rompath", required=True, type=existing_dir)
    parser.add_argument("--nvram-seed", required=True, type=existing_file)
    parser.add_argument("--out", required=True, type=lambda value: Path(value).expanduser().resolve())
    parser.add_argument("--seconds", type=int, default=14)
    parser.add_argument("--note", default="11.0:2.0:60:100")
    parser.add_argument("--block", type=int, default=512)
    parser.add_argument("--ring-frames", type=int, default=8192)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        raise SystemExit(f"refusing to mix results in non-empty output directory: {args.out}")
    args.out.mkdir(parents=True, exist_ok=True)

    cases = {
        "reference_pf4_propmin": (args.reference_propmin, False, "4"),
        "reference_pf4_console": (args.reference_console, True, "4"),
        "candidate_interp_propmin": (args.candidate_propmin, False, "1"),
        "candidate_pf4_propmin": (args.candidate_propmin, False, "4"),
        "candidate_pf4_console": (args.candidate_console, True, "4"),
    }
    wavs = {
        name: run_case(name=name, binary=binary, is_console=is_console, perframe=perframe, args=args)
        for name, (binary, is_console, perframe) in cases.items()
    }
    pcm: dict[str, bytes] = {}
    wav_metadata: dict[str, dict[str, int | str]] = {}
    for name, path in wavs.items():
        pcm[name], wav_metadata[name] = read_wav(path)

    comparisons = [
        ("reference_internal", "reference_pf4_propmin", "reference_pf4_console"),
        ("candidate_interp_vs_pf4", "candidate_interp_propmin", "candidate_pf4_propmin"),
        ("candidate_internal", "candidate_pf4_propmin", "candidate_pf4_console"),
        ("reference_vs_candidate", "reference_pf4_propmin", "candidate_pf4_propmin"),
    ]
    results: dict[str, dict[str, object]] = {}
    all_passed = True
    for label, left_name, right_name in comparisons:
        left = pcm[left_name]
        right = pcm[right_name]
        metadata = wav_metadata[left_name]
        identical = left == right and len(left) >= 2_000_000
        detail = first_difference(left, right, int(metadata["channels"]), int(metadata["sample_width"]))
        results[label] = {"identical": identical, "left": left_name, "right": right_name, **detail}
        state = "PASS" if identical else "FAIL"
        suffix = "byte-identical" if identical else f"first_frame={detail['first_frame']} first_seconds={detail['first_seconds']}"
        print(f"{state} {label}: {suffix}")
        all_passed &= identical

    receipt = {
        "inputs": {
            "reference_propmin": {"path": str(args.reference_propmin), "sha256": sha256(args.reference_propmin)},
            "reference_console": {"path": str(args.reference_console), "sha256": sha256(args.reference_console)},
            "candidate_propmin": {"path": str(args.candidate_propmin), "sha256": sha256(args.candidate_propmin)},
            "candidate_console": {"path": str(args.candidate_console), "sha256": sha256(args.candidate_console)},
            "roms": {
                "ic12_v17.bin": sha256(args.rompath / "korgprop" / "ic12_v17.bin"),
                "ic22_v17.bin": sha256(args.rompath / "korgprop" / "ic22_v17.bin"),
                "hd44780_a00": "compiled datasheet reconstruction in candidate MAME",
            },
            "nvram_seed": {"path": str(args.nvram_seed), "sha256": sha256(args.nvram_seed)},
            "seconds": args.seconds,
            "note": args.note,
            "block": args.block,
            "ring_frames": args.ring_frames,
        },
        "wav": wav_metadata,
        "comparisons": results,
        "passed": all_passed,
    }
    receipt_path = args.out / "receipt.json"
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"receipt: {receipt_path}")
    print("PCM_EQUIVALENCE_PASS" if all_passed else "PCM_EQUIVALENCE_FAIL")
    return 0 if all_passed else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)
