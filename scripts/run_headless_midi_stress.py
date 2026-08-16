#!/usr/bin/env python3
"""Run deterministic DAW-style MIDI/control soaks through the real plugin host."""

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


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CASES = ((48000, 32), (48000, 128), (48000, 512), (48000, 1024),
                 (44100, 128), (96000, 128))
SUMMARY_RE = re.compile(
    r"\[stress-health\] SUMMARY attempts=(\d+) replies=(\d+) misses=(\d+) "
    r"worst_consecutive_misses=(\d+) max_latency=([0-9.]+) verdict=(PASS|FAIL)")
DROPS_RE = re.compile(r"output_dropped=(\d+) input_dropped=(\d+)")
FINAL_HEALTH_RE = re.compile(
    r"KPROP_FINAL_HEALTH,T=(?P<time>[0-9.]+),"
    r"VPC=(?P<vpc>[0-9A-F]+),HPC=(?P<hpc>[0-9A-F]+),"
    r"MB=(?P<mb_w>[0-9A-F]+)/(?P<mb_r>[0-9A-F]+)/(?P<mb_count>[0-9A-F]+),"
    r"CTRL=(?P<ctrl_w>[0-9A-F]+)/(?P<ctrl_r>[0-9A-F]+),"
    r"A716=(?P<a716>[0-9A-F]+),A721=(?P<a721>[0-9A-F]+),"
    r"H8CMD=(?P<h8_w>[0-9A-F]+)/(?P<h8_r>[0-9A-F]+)/(?P<h8_count>[0-9A-F]+),"
    r"SCI=(?P<sci>[0-9A-F/]+),(?:H8I=(?P<h8_intc>[0-9A-F/]+),)?"
    r"(?:H8P=(?P<h8_writes>\d+)/(?P<h8_write_pc>[0-9A-F]+)/"
    r"(?P<h8_write_time>[0-9.]+)/(?P<h8_reads>\d+)/"
    r"(?P<h8_read_pc>[0-9A-F]+)/(?P<h8_read_time>[0-9.]+),)?"
    r"(?:H8E=(?P<h8_error_reads>\d+)/(?P<h8_error_status>[0-9A-F]+)/"
    r"(?P<h8_error_pc>[0-9A-F]+)/(?P<h8_error_time>[0-9.]+)/"
    r"(?P<h8_overrun_reads>\d+)/(?P<h8_framing_reads>\d+)/(?P<h8_parity_reads>\d+),)?"
    r"IRQ=(?P<irq>[0-9/]+),"
    r"TXD=(?P<txd_edges>\d+)/(?P<txd_state>\d+),"
    r"U1=(?P<u1_rx>\d+)/(?P<u1_consumed>\d+)/(?P<u1_overruns>\d+)/"
    r"(?P<u1_framing>\d+)/(?P<u1_status>[0-9A-F]+)/(?P<u1_data>[0-9A-F]+),"
    r"U0=(?P<u0_mode>[0-9A-F]+)/(?P<u0_status>[0-9A-F]+)/(?P<u0_data>[0-9A-F]+)/"
    r"(?P<u0_active>\d+)/(?P<u0_loaded>\d+)/(?P<u0_rx_full>\d+)/(?P<u0_cts>\d+),"
    r"IRQS=(?P<irq_pending>[0-9A-F]+)/(?P<irq_service>[0-9A-F]+),"
    r"IC=(?P<irq_control>[0-9A-F/]+)", re.IGNORECASE)
FLIGHT_BEGIN_RE = re.compile(
    r"KPROP_FLIGHT_BEGIN,COUNT=(?P<count>\d+),TOTAL=(?P<total>\d+),"
    r"TRIGGERED=(?P<triggered>[01]),TRIGGER_T=(?P<trigger_time>[0-9.]+)")
FLIGHT_EVENT_RE = re.compile(
    r"KPROP_FLIGHT,I=(?P<index>\d+),T=(?P<time>[0-9.]+),"
    r"E=(?P<event>[A-Z0-9_]+),D=(?P<data>[0-9A-F]+),"
    r"VPC=(?P<vpc>[0-9A-F]+),HPC=(?P<hpc>[0-9A-F]+),"
    r"MB=(?P<mb_w>[0-9A-F]+)/(?P<mb_r>[0-9A-F]+)/(?P<mb_count>[0-9A-F]+),"
    r"CTRL=(?P<ctrl_w>[0-9A-F]+)/(?P<ctrl_r>[0-9A-F]+),"
    r"H8CMD=(?P<h8_w>[0-9A-F]+)/(?P<h8_r>[0-9A-F]+),"
    r"A716=(?P<a716>[0-9A-F]+),A721=(?P<a721>[0-9A-F]+),"
    r"U0=(?P<u0_mode>[0-9A-F]+)/(?P<u0_status>[0-9A-F]+)/(?P<u0_flags>[0-9A-F]+),"
    r"IRQS=(?P<irq_pending>[0-9A-F]+)/(?P<irq_service>[0-9A-F]+),"
    r"SCI=(?P<h8_ssr>[0-9A-F]+)/(?P<h8_rdr>[0-9A-F]+),"
    r"H8E=(?P<h8_error_count>\d+)/(?P<h8_error_status>[0-9A-F]+)/"
    r"(?P<h8_error_pc>[0-9A-F]+)/(?P<h8_error_time>[0-9.]+)", re.IGNORECASE)


def parse_final_health(log_text: str) -> dict[str, object] | None:
    """Parse the low-perturbation MAME shutdown snapshot, when available."""
    match = FINAL_HEALTH_RE.search(log_text)
    if match is None:
        return None
    fields = match.groupdict()

    def hex_value(name: str) -> int:
        return int(fields[name], 16)

    def decimal_value(name: str) -> int:
        return int(fields[name])

    return {
        "time_seconds": float(fields["time"]),
        "v55_pc": hex_value("vpc"),
        "h8_pc": hex_value("hpc"),
        "mailbox": {
            "write": hex_value("mb_w"), "read": hex_value("mb_r"),
            "count": hex_value("mb_count"),
        },
        "control": {"write": hex_value("ctrl_w"), "read": hex_value("ctrl_r")},
        "latches": {"a716": hex_value("a716"), "a721": hex_value("a721")},
        "h8_command": {
            "write": hex_value("h8_w"), "read": hex_value("h8_r"),
            "count": hex_value("h8_count"),
        },
        "sci": [int(value, 16) for value in fields["sci"].split("/")],
        "h8_intc": (None if fields["h8_intc"] is None else
                     [int(value, 16) for value in fields["h8_intc"].split("/")]),
        "h8_command_pointer_writes": (None if fields["h8_writes"] is None else {
            "write": {
                "count": int(fields["h8_writes"]),
                "pc": int(fields["h8_write_pc"], 16),
                "time_seconds": float(fields["h8_write_time"]),
            },
            "read": {
                "count": int(fields["h8_reads"]),
                "pc": int(fields["h8_read_pc"], 16),
                "time_seconds": float(fields["h8_read_time"]),
            },
        }),
        "h8_sci_errors": (None if fields["h8_error_reads"] is None else {
            "count": int(fields["h8_error_reads"]),
            "last_status": int(fields["h8_error_status"], 16),
            "last_pc": int(fields["h8_error_pc"], 16),
            "last_time_seconds": float(fields["h8_error_time"]),
            "overruns": int(fields["h8_overrun_reads"]),
            "framing_errors": int(fields["h8_framing_reads"]),
            "parity_errors": int(fields["h8_parity_reads"]),
        }),
        "h8_irq": [int(value) for value in fields["irq"].split("/")],
        "txd": {"edges": decimal_value("txd_edges"), "state": decimal_value("txd_state")},
        "uart1": {
            "received": decimal_value("u1_rx"),
            "consumed": decimal_value("u1_consumed"),
            "overruns": decimal_value("u1_overruns"),
            "framing_errors": decimal_value("u1_framing"),
            "status": hex_value("u1_status"), "data": hex_value("u1_data"),
        },
        "uart0": {
            "mode": hex_value("u0_mode"), "status": hex_value("u0_status"),
            "data": hex_value("u0_data"), "tx_active": bool(decimal_value("u0_active")),
            "tx_loaded": bool(decimal_value("u0_loaded")),
            "rx_full": bool(decimal_value("u0_rx_full")), "cts": decimal_value("u0_cts"),
        },
        "serial_irq": {
            "pending": hex_value("irq_pending"), "in_service": hex_value("irq_service"),
            "control": [int(value, 16) for value in fields["irq_control"].split("/")],
        },
    }


def parse_control_flight(log_text: str) -> dict[str, object] | None:
    """Parse the deferred board-link flight-recorder dump, when available."""
    begin = FLIGHT_BEGIN_RE.search(log_text)
    if begin is None:
        return None

    events: list[dict[str, object]] = []
    for match in FLIGHT_EVENT_RE.finditer(log_text, begin.end()):
        fields = match.groupdict()

        def hex_value(name: str) -> int:
            return int(fields[name], 16)

        events.append({
            "index": int(fields["index"]),
            "time_seconds": float(fields["time"]),
            "event": fields["event"].upper(),
            "data": hex_value("data"),
            "v55_pc": hex_value("vpc"),
            "h8_pc": hex_value("hpc"),
            "mailbox": {
                "write": hex_value("mb_w"), "read": hex_value("mb_r"),
                "count": hex_value("mb_count"),
            },
            "control": {"write": hex_value("ctrl_w"), "read": hex_value("ctrl_r")},
            "h8_command": {"write": hex_value("h8_w"), "read": hex_value("h8_r")},
            "latches": {"a716": hex_value("a716"), "a721": hex_value("a721")},
            "uart0": {
                "mode": hex_value("u0_mode"), "status": hex_value("u0_status"),
                "flags": hex_value("u0_flags"),
            },
            "serial_irq": {
                "pending": hex_value("irq_pending"),
                "in_service": hex_value("irq_service"),
            },
            "h8_sci": {
                "status": hex_value("h8_ssr"), "data": hex_value("h8_rdr"),
                "errors": {
                    "count": int(fields["h8_error_count"]),
                    "last_status": hex_value("h8_error_status"),
                    "last_pc": hex_value("h8_error_pc"),
                    "last_time_seconds": float(fields["h8_error_time"]),
                },
            },
        })

    return {
        "dump_count": int(begin.group("count")),
        "total_events": int(begin.group("total")),
        "triggered": begin.group("triggered") == "1",
        "trigger_time_seconds": float(begin.group("trigger_time")),
        "events": events,
    }


def classify_final_health(snapshot: dict[str, object] | None, health_pass: bool) -> str | None:
    """Name the observed freeze signature without making it a pass criterion."""
    if snapshot is None or health_pass:
        return None
    uart1 = snapshot["uart1"]
    mailbox = snapshot["mailbox"]
    h8_command = snapshot["h8_command"]
    uart0 = snapshot["uart0"]
    serial_irq = snapshot["serial_irq"]
    assert isinstance(uart1, dict) and isinstance(mailbox, dict)
    assert isinstance(h8_command, dict) and isinstance(uart0, dict)
    assert isinstance(serial_irq, dict)
    uart1_healthy = (uart1["received"] == uart1["consumed"] and
                     uart1["overruns"] == 0 and uart1["framing_errors"] == 0)
    board_link_backlogged = mailbox["count"] != 0 and h8_command["count"] != 0
    uart0_idle = (not uart0["tx_active"] and not uart0["tx_loaded"] and
                  not uart0["rx_full"] and serial_irq["pending"] == 0 and
                  serial_irq["in_service"] == 0)
    if uart1_healthy and board_link_backlogged and uart0_idle:
        return "internal_board_link_stall"
    return "unclassified_health_failure"


def default_host() -> Path:
    candidates = (
        ROOT / "build-cmake" / "ProphecyPluginTimingHost",
        ROOT / "build-cmake" / "ProphecyPluginTimingHost_artefacts" / "Release"
        / "ProphecyPluginTimingHost",
    )
    return next((path for path in candidates if path.is_file()), candidates[0])


def parse_case(text: str) -> tuple[int, int]:
    try:
        rate_text, block_text = text.split(":", 1)
        rate, block = int(rate_text), int(block_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("case must be RATE:BLOCK") from exc
    if rate < 8000 or block <= 0:
        raise argparse.ArgumentTypeError("rate and block must be positive")
    return rate, block


def parse_int(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("expected an integer (decimal or 0x-prefixed)") from exc
    if not 0 <= value <= 0xffffffff:
        raise argparse.ArgumentTypeError("seed must fit in an unsigned 32-bit integer")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--host", type=Path, default=default_host())
    parser.add_argument("--rompath", type=Path, default=ROOT.parent / "mame" / "00-roms")
    parser.add_argument("--nvram-seed", type=Path,
                        default=ROOT.parent / "mame" / "nvram" / "korgprop" / "sysram")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--case", action="append", type=parse_case, dest="cases")
    parser.add_argument("--seed", action="append", type=parse_int, dest="seeds")
    parser.add_argument("--phase-sample", action="append", type=int, dest="phase_samples")
    parser.add_argument("--health-interval", type=float, default=5.0)
    parser.add_argument("--health-timeout", type=float, default=3.0)
    parser.add_argument("--program-interval", type=float, default=7.0)
    parser.add_argument("--program-start", type=float, default=18.0)
    parser.add_argument("--program-value", action="append", type=int, dest="program_values")
    parser.add_argument("--max-health-misses", type=int, default=2)
    parser.add_argument("--no-setup", action="store_true")
    parser.add_argument("--no-clock", action="store_true")
    parser.add_argument("--no-notes", action="store_true")
    parser.add_argument("--no-controls", action="store_true")
    parser.add_argument("--no-params", action="store_true")
    parser.add_argument("--no-programs", action="store_true")
    args = parser.parse_args()

    if args.seconds < 16.0:
        parser.error("--seconds must be at least 16")
    if args.program_interval <= 0.0:
        parser.error("--program-interval must be positive")
    if args.program_start < 0.0:
        parser.error("--program-start must be nonnegative")
    if args.program_values is not None and any(
            not 0 <= value <= 127 for value in args.program_values):
        parser.error("--program-value must be between 0 and 127")
    args.host = args.host.resolve()
    args.rompath = args.rompath.resolve()
    args.nvram_seed = args.nvram_seed.resolve()
    for path, label in ((args.host, "timing host"), (args.rompath, "ROM path"),
                        (args.nvram_seed, "NVRAM seed")):
        if not path.exists():
            parser.error(f"missing {label}: {path}")

    cases = args.cases or list(DEFAULT_CASES)
    seeds = args.seeds or [0x50524F50]
    phases = args.phase_samples or [0]
    if any(phase < 0 for phase in phases):
        parser.error("--phase-sample must be nonnegative")
    for rate, block in cases:
        if any(phase >= block for phase in phases):
            parser.error(f"phase sample must be less than block size {block}")
    output = args.output or Path(tempfile.gettempdir()) / (
        "profligacy_midi_stress_" + dt.datetime.now().strftime("%Y%m%d_%H%M%S"))
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)

    records: list[dict[str, object]] = []
    total = len(cases) * len(seeds) * len(phases)
    case_number = 0
    for seed in seeds:
        for rate, block in cases:
            for phase_sample in phases:
                case_number += 1
                name = (f"seed_{seed:08x}_rate_{rate}_block_{block}"
                        f"_phase_{phase_sample}")
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
                    "SDL_VIDEODRIVER": "dummy",
                    "SDL_AUDIODRIVER": "dummy",
                })
                environment.setdefault("KPROP_LOG_CONTROL_HEALTH_FINAL", "1")
                environment.setdefault("KPROP_CONTROL_FLIGHT_RECORDER", "1")
                command = [
                    str(args.host), "--rate", str(rate), "--block", str(block),
                    "--phase-samples", str(phase_sample),
                    "--seconds", str(args.seconds), "--stress", "--seed", str(seed),
                    "--health-interval", str(args.health_interval),
                    "--health-timeout", str(args.health_timeout),
                    "--program-interval", str(args.program_interval),
                    "--program-start", str(args.program_start),
                    "--max-health-misses", str(args.max_health_misses),
                    "--output", str(capture),
                ]
                for program_value in args.program_values or ():
                    command.extend(("--program-value", str(program_value)))
                for option in ("no_setup", "no_clock", "no_notes", "no_controls",
                               "no_params", "no_programs"):
                    if getattr(args, option):
                        command.append("--" + option.replace("_", "-"))
                print(f"[{case_number}/{total}] {name}", flush=True)
                timed_out = False
                returncode = -1
                try:
                    with log.open("w", encoding="utf-8") as stream:
                        completed = subprocess.run(
                            command, cwd=case_dir, env=environment, stdout=stream,
                            stderr=subprocess.STDOUT, check=False,
                            timeout=max(args.seconds * 1.5, args.seconds + 30.0))
                    returncode = completed.returncode
                except subprocess.TimeoutExpired:
                    timed_out = True

                log_text = log.read_text(encoding="utf-8", errors="replace")
                summary_match = SUMMARY_RE.search(log_text)
                drops_match = DROPS_RE.search(log_text)
                final_health = parse_final_health(log_text)
                control_flight = parse_control_flight(log_text)
                checks = {
                    "host_exit": returncode == 0,
                    "no_timeout": not timed_out,
                    "health_summary": summary_match is not None,
                    "health_pass": summary_match is not None and summary_match.group(6) == "PASS",
                    "midi_output_no_drops": drops_match is not None and drops_match.group(1) == "0",
                    "midi_input_no_drops": drops_match is not None and drops_match.group(2) == "0",
                }
                health = None if summary_match is None else {
                    "attempts": int(summary_match.group(1)),
                    "replies": int(summary_match.group(2)),
                    "misses": int(summary_match.group(3)),
                    "worst_consecutive_misses": int(summary_match.group(4)),
                    "max_latency_seconds": float(summary_match.group(5)),
                }
                fault_signature = classify_final_health(final_health, checks["health_pass"])
                flight_artifact = None
                flight_summary = None
                if control_flight is not None:
                    flight_artifact = case_dir / "control-flight.json"
                    flight_artifact.write_text(
                        json.dumps(control_flight, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
                    flight_summary = {
                        key: value for key, value in control_flight.items() if key != "events"
                    }
                    flight_summary["parsed_events"] = len(control_flight["events"])
                record = {
                    "name": name, "seed": seed, "rate": rate, "block": block,
                    "phase_sample": phase_sample,
                    "seconds": args.seconds, "checks": checks, "health": health,
                    "final_health": final_health, "fault_signature": fault_signature,
                    "control_flight": flight_summary,
                    "verdict": "PASS" if all(checks.values()) else "FAIL",
                    "command": command,
                    "artifacts": {"midi": str(capture), "host_log": str(log)},
                }
                error_log = case_dir / "error.log"
                if error_log.is_file():
                    record["artifacts"]["mame_error_log"] = str(error_log)
                if flight_artifact is not None:
                    record["artifacts"]["control_flight"] = str(flight_artifact)
                records.append(record)
                (case_dir / "result.json").write_text(
                    json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                print(f"  {record['verdict']} health={health}")

    summary = {
        "schema": 1,
        "host": str(args.host.resolve()),
        "rompath": str(args.rompath.resolve()),
        "phase_samples": phases,
        "program_interval": args.program_interval,
        "program_start": args.program_start,
        "program_values": args.program_values,
        "traffic": {
            name: not getattr(args, "no_" + name)
            for name in ("setup", "clock", "notes", "controls", "params", "programs")
        },
        "pass_count": sum(record["verdict"] == "PASS" for record in records),
        "fail_count": sum(record["verdict"] != "PASS" for record in records),
        "cases": records,
    }
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"summary: {summary['pass_count']} PASS / {summary['fail_count']} FAIL  {output}")
    return 0 if summary["fail_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
