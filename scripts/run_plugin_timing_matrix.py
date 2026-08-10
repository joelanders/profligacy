#!/usr/bin/env python3
"""Exercise external-clock timing through the real JUCE processBlock path."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import arp_timing_analyze as timing


ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / "build-cmake" / "ProphecyPluginTimingHost_artefacts" / "Release" / "ProphecyPluginTimingHost"
CASES = ((48000, 32), (48000, 128), (48000, 512), (48000, 1024),
         (44100, 128), (96000, 128))
EXPECTED_INTERVAL = 4.0 * 60.0 / (24.0 * 120.0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    parser.add_argument("--case", help="run one case as RATE:BLOCK")
    parser.add_argument("--rompath", type=Path, default=ROOT.parent / "mame" / "00-roms")
    parser.add_argument("--nvram-seed", type=Path,
                        default=ROOT.parent / "mame" / "nvram" / "korgprop" / "sysram")
    args = parser.parse_args()
    output = args.output or Path(tempfile.gettempdir()) / (
        "prophecy_plugin_timing_" + dt.datetime.now().strftime("%Y%m%d_%H%M%S"))
    output.mkdir(parents=True, exist_ok=False)
    if not HOST.is_file() or not args.rompath.is_dir() or not args.nvram_seed.is_file():
        raise SystemExit("missing timing host, ROM path, or NVRAM seed")

    cases = list(CASES)
    if args.case:
        rate_text, block_text = args.case.split(":", 1)
        selected = (int(rate_text), int(block_text))
        if selected not in cases:
            raise SystemExit(f"unknown case {args.case}")
        cases = [selected]

    records: list[dict[str, object]] = []
    for index, (rate, block) in enumerate(cases, 1):
        name = f"rate_{rate}_block_{block}"
        case_dir = output / name
        nvram = case_dir / "nvram" / "korgprop"
        nvram.mkdir(parents=True)
        shutil.copy2(args.nvram_seed, nvram / "sysram")
        capture = case_dir / "midi.jsonl"
        log = case_dir / "host.log"
        environment = os.environ.copy()
        environment.update({
            "PROPHECY_ROMPATH": str(args.rompath),
            "PROPHECY_NVRAM": str(case_dir / "nvram"),
            "PROPHECY_EDITOR_SELFTEST": "1",
            "SDL_VIDEODRIVER": "dummy",
            "SDL_AUDIODRIVER": "dummy",
        })
        command = (str(HOST), "--rate", str(rate), "--block", str(block),
                   "--output", str(capture))
        print(f"[{index}/{len(cases)}] {name}", flush=True)
        with log.open("w", encoding="utf-8") as stream:
            completed = subprocess.run(command, cwd=ROOT, env=environment,
                                       stdout=stream, stderr=subprocess.STDOUT, check=False)
        log_text = log.read_text(encoding="utf-8", errors="replace")
        result = timing.analyze(timing.load_records(capture, None, None))
        notes = result["note_on"]
        assert isinstance(notes, dict)
        fit = float(notes.get("fit_interval_s", 0.0))
        p95 = float(notes.get("p95_abs_interval_jitter_s", 999.0))
        maximum = float(notes.get("max_abs_interval_jitter_s", 999.0))
        output_drop_match = re.search(r"output_dropped=(\d+)", log_text)
        input_drop_match = re.search(r"input_dropped=(\d+)", log_text)
        output_drops = int(output_drop_match.group(1)) if output_drop_match else -1
        input_drops = int(input_drop_match.group(1)) if input_drop_match else -1
        checks = {
            "host_exit": completed.returncode == 0,
            "observer_no_drops": output_drops == 0,
            "scheduled_input_no_drops": input_drops == 0,
            "enough_notes": int(notes.get("count", 0)) >= 24,
            "mean_period": abs(fit - EXPECTED_INTERVAL) < 0.001,
            "p95_jitter": p95 < 0.005,
            "max_jitter": maximum < 0.010,
            "no_stuck_notes": int(result.get("unmatched_note_ons", -1)) == 0,
        }
        record = {
            "rate": rate, "block": block, "result": result,
            "expected_interval_s": EXPECTED_INTERVAL,
            "checks": checks, "verdict": "PASS" if all(checks.values()) else "FAIL",
            "artifacts": {"midi": str(capture), "host_log": str(log)},
        }
        records.append(record)
        (case_dir / "result.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        print(f"  {record['verdict']} fit={fit * 1000:.3f}ms "
              f"p95={p95 * 1000:.3f}ms max={maximum * 1000:.3f}ms")

    summary = {
        "schema": 1, "expected_interval_s": EXPECTED_INTERVAL,
        "pass_count": sum(record["verdict"] == "PASS" for record in records),
        "fail_count": sum(record["verdict"] != "PASS" for record in records),
        "cases": records,
    }
    (output / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(f"summary: {summary['pass_count']} PASS / {summary['fail_count']} FAIL  {output}")
    return 0 if summary["fail_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
