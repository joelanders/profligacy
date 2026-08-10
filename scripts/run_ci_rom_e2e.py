#!/usr/bin/env python3
"""Boot the clean-room ROM through a packaged VST3 and grade the result."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
ROM_SCRIPT = Path(__file__).with_name("build_ci_probe_roms.py")
SPEC = importlib.util.spec_from_file_location("build_ci_probe_roms", ROM_SCRIPT)
assert SPEC and SPEC.loader
roms = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(roms)

JIT_RE = re.compile(
    r"KPROP_JIT_RUNTIME tag=(\S+) calls=(\d+) runs=(\d+) "
    r"fallbacks=(\d+) compiles=(\d+) forced_midframe=(\d+)"
)


def parse_jit_runtime(log: str) -> dict[str, dict[str, int]]:
    result: dict[str, dict[str, int]] = {}
    for match in JIT_RE.finditer(log):
        tag = match.group(1)
        values = {
            key: int(value)
            for key, value in zip(
                ("calls", "runs", "fallbacks", "compiles", "forced_midframe"),
                match.groups()[1:],
            )
        }
        previous = result.get(tag)
        if previous is None or values["calls"] >= previous["calls"]:
            result[tag] = values
    return result


def find_dsp(stats: dict[str, dict[str, int]], number: int) -> tuple[str, dict[str, int]] | None:
    suffix = f"dsp{number}"
    matches = [(tag, values) for tag, values in stats.items() if tag.rstrip(":").endswith(suffix)]
    return matches[0] if len(matches) == 1 else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-host", type=Path, required=True)
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=6.0)
    parser.add_argument("--require-jit-telemetry", action="store_true")
    args = parser.parse_args()
    # The host runs with the receipt directory as cwd for isolation, so resolve
    # caller-relative executable and bundle paths before changing directories.
    args.artifact_host = args.artifact_host.resolve()
    args.plugin = args.plugin.resolve()
    args.work = args.work.resolve()

    args.work.mkdir(parents=True, exist_ok=True)
    rom_root = args.work / "roms"
    nvram_root = args.work / "nvram"
    nvram_root.mkdir(parents=True, exist_ok=True)
    rom_receipt = roms.build(rom_root)
    artifact_receipt_path = args.work / "artifact-host.json"
    wav_path = args.work / "three-dsp-sentinel.wav"
    state_path = args.work / "synthetic-final-state.bin"
    log_path = args.work / "artifact-host.log"
    receipt_path = args.work / "ci-rom-e2e.json"

    environment = os.environ.copy()
    for name in ("PROPHECY_FORCE_NO_ROM",):
        environment.pop(name, None)
    environment.update({
        "PROPHECY_ROMPATH": str(rom_root),
        "PROPHECY_NVRAM": str(nvram_root),
        "PROFLIGACY_CI_EXPOSE_LCD_STATE": "1",
        "KPROP_PF4_STATS": "1",
    })
    command = [
        str(args.artifact_host),
        "--plugin", str(args.plugin),
        "--receipt", str(artifact_receipt_path),
        "--wav", str(wav_path),
        "--seconds", str(args.seconds),
        "--require-audio",
        "--realtime",
        "--expect-state-marker", "CILC:CI V55+H8 OK",
        "--state-out", str(state_path),
    ]
    with log_path.open("w", encoding="utf-8") as log_file:
        completed = subprocess.run(
            command,
            cwd=args.work,
            env=environment,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    log = log_path.read_text(encoding="utf-8", errors="replace")
    stats = parse_jit_runtime(log)
    failures: list[str] = []
    if completed.returncode:
        failures.append(f"artifact host exited {completed.returncode}")
    if not artifact_receipt_path.is_file():
        artifact_receipt: dict[str, object] = {}
        failures.append("artifact host did not write its receipt")
    else:
        artifact_receipt = json.loads(artifact_receipt_path.read_text(encoding="utf-8"))
        if not artifact_receipt.get("success"):
            failures.append("artifact host receipt failed")
    dsp_receipts = []
    for number in (1, 2, 3):
        found = find_dsp(stats, number)
        if found is None:
            if args.require_jit_telemetry:
                failures.append(f"missing unambiguous runtime telemetry for DSP{number}")
            continue
        tag, values = found
        native_ok = values["calls"] > 0 and values["runs"] > 0 and values["compiles"] > 0
        dsp_receipts.append({"dsp": number, "tag": tag, "native_ok": native_ok, **values})
        if args.require_jit_telemetry and not native_ok:
            failures.append(f"DSP{number} did not compile and run native frames")

    receipt = {
        "schema": "profligacy-clean-room-packaged-e2e-v1",
        "success": not failures,
        "command": command,
        "rom": rom_receipt,
        "artifact_host": artifact_receipt,
        "jit_runtime": dsp_receipts,
        "expected": {
            "lcd": "CI V55+H8 OK",
            "audio_dependency": "DSP1 -> DSP2 -> DSP3 -> DAC",
            "three_dsp_execution_proof": "nonzero DAC audio requires DSP1 -> DSP2 -> DSP3",
            "jit_telemetry_required": args.require_jit_telemetry,
            "individual_deopts_allowed": ["DSP2 DIS", "DSP3 DIS", "IDLE termination"],
        },
        "artifacts": {
            "wav": str(wav_path),
            "log": str(log_path),
            "artifact_host_receipt": str(artifact_receipt_path),
            "synthetic_final_state": str(state_path),
        },
        "failures": failures,
    }
    receipt_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({
        "success": receipt["success"],
        "artifact_host": artifact_receipt,
        "jit_runtime": dsp_receipts,
        "failures": failures,
        "receipt": str(receipt_path),
    }, indent=2))
    if failures and log:
        print("--- artifact-host.log tail ---")
        print("\n".join(log.splitlines()[-120:]))
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())
