#!/usr/bin/env python3
"""Run deterministic DAW/editor control soaks through the real plugin host."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import random
import re
import shutil
import subprocess
import tempfile
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CASES = ((48000, 32), (48000, 128), (48000, 512), (48000, 1024),
                 (44100, 128), (96000, 128))
SUMMARY_RE = re.compile(
    r"\[stress-health\] SUMMARY attempts=(\d+) replies=(\d+) misses=(\d+) "
    r"worst_consecutive_misses=(\d+) max_latency=([0-9.]+) verdict=(PASS|FAIL)")
DROPS_RE = re.compile(
    r"output_dropped=(?P<output>\d+) input_dropped=(?P<input>\d+).*"
    r"ui_adin_dropped=(?P<ui_adin>\d+) audio_adin_dropped=(?P<audio_adin>\d+) "
    r"oversized_blocks=(?P<oversized>\d+)")
ARP_RECOVERY_RE = re.compile(
    r"\[arp-recovery\] SUMMARY requested=(?P<requested>[01]) "
    r"replied=(?P<replied>[01]) pending=(?P<pending>[01]) "
    r"max_latency=(?P<latency>[0-9.]+) verdict=(?P<verdict>PASS|FAIL)")
CLOCK_GATE_RE = re.compile(r"\[editor-clock-gate\] suppressed=(?P<count>\d+)")
EDITOR_PACER_RE = re.compile(
    r"\[editor-command-pacer\] sent=(?P<sent>\d+) coalesced=(?P<coalesced>\d+) "
    r"cancelled=(?P<cancelled>\d+) dropped=(?P<dropped>\d+) "
    r"pending=(?P<pending>\d+) verdict=(?P<verdict>PASS|FAIL)")
EDITOR_RECOVERY_RE = re.compile(
    r'\[editor-recovery\] final_lcd="([^"]*)" expected=([AB]\d\d:) '
    r'verdict=(PASS|FAIL)')
EDITOR_SCHEDULER_RE = re.compile(
    r"\[editor-scheduler\] patch_intents=(\d+) patch_sends=(\d+) "
    r"dump_requests=(\d+) dump_sends=(\d+) verdict=(PASS|FAIL)")
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
    r"(?:WIRE=(?P<wire_compared>\d+)/(?P<wire_mismatches>\d+)/"
    r"(?P<wire_unexpected>\d+)/(?P<wire_pending>\d+)/"
    r"(?P<wire_expected>[0-9A-F]+)/(?P<wire_actual>[0-9A-F]+)/"
    r"(?P<wire_mismatch_time>[0-9.]+),)?"
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
        "board_link_bytes": (None if fields["wire_compared"] is None else {
            "compared": int(fields["wire_compared"]),
            "mismatches": int(fields["wire_mismatches"]),
            "unexpected": int(fields["wire_unexpected"]),
            "pending": int(fields["wire_pending"]),
            "last_expected": int(fields["wire_expected"], 16),
            "last_actual": int(fields["wire_actual"], 16),
            "last_mismatch_time_seconds": float(fields["wire_mismatch_time"]),
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
    if snapshot is None:
        return None
    wire = snapshot["board_link_bytes"]
    if isinstance(wire, dict) and (wire["mismatches"] != 0 or
                                   wire["unexpected"] != 0 or
                                   wire["pending"] != 0):
        return "board_link_byte_mismatch"
    if health_pass:
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


def default_asset(*relative_parts: str) -> Path:
    """Find shared ROM/NVRAM assets in either supported local worktree layout."""
    candidates = (
        ROOT.parent / "mame" / Path(*relative_parts),
        ROOT / "extern" / "mame" / Path(*relative_parts),
        ROOT.parents[1] / Path(*relative_parts),
    )
    return next((path for path in candidates if path.exists()), candidates[0])


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


def load_scenario(path: Path) -> dict[str, object]:
    """Load the versioned action timeline used by the one native timing host."""
    try:
        scenario = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise argparse.ArgumentTypeError(f"cannot read scenario: {exc}") from exc
    if not isinstance(scenario, dict) or scenario.get("schema") != 1:
        raise argparse.ArgumentTypeError("scenario must be an object with schema 1")
    if not isinstance(scenario.get("name"), str) or not scenario["name"]:
        raise argparse.ArgumentTypeError("scenario needs a nonempty name")
    actions = scenario.get("actions")
    if not isinstance(actions, list):
        raise argparse.ArgumentTypeError("scenario actions must be an array")
    previous_at = -1.0
    for index, action in enumerate(actions):
        if not isinstance(action, dict) or not isinstance(action.get("op"), str):
            raise argparse.ArgumentTypeError(f"scenario action {index} needs an op")
        at = action.get("at")
        if not isinstance(at, (int, float)) or at < 0:
            raise argparse.ArgumentTypeError(
                f"scenario action {index} needs a nonnegative at")
        if at < previous_at:
            raise argparse.ArgumentTypeError("scenario actions must be time ordered")
        previous_at = float(at)
    return scenario


def rapid_patch_scenario(program_values: list[int], program_start: float,
                         program_interval: float, phase_seconds: float) -> dict[str, object]:
    actions = [
        {"at": round(program_start + phase_seconds + index * program_interval, 9),
         "op": "select_patch", "args": [program]}
        for index, program in enumerate(program_values)
    ]
    if actions:
        actions.append({"at": round(float(actions[-1]["at"]) + 0.350, 9),
                        "op": "request_program_dump"})
    return {"schema": 1, "name": "rapid-patch-browse", "actions": actions}


def editor_boundary_scenario(seed: int, phase_seconds: float) -> dict[str, object]:
    """Exercise every production action family around known scheduler boundaries."""
    raw_pattern = [((index * 37) ^ seed) & 0xff for index in range(128)]
    offset = phase_seconds
    actions: list[dict[str, object]] = [
        {"at": 15.000 + offset, "op": "select_patch", "args": [1]},
        {"at": 15.149 + offset, "op": "select_patch", "args": [2]},
        {"at": 15.151 + offset, "op": "select_patch", "args": [3]},
        {"at": 15.300 + offset, "op": "request_program_dump"},
        {"at": 15.301 + offset, "op": "request_program_dump"},
        {"at": 18.000 + offset, "op": "set_param", "args": [355, 17]},
        {"at": 18.120 + offset, "op": "set_global_param", "args": [171, 1]},
        {"at": 18.240 + offset, "op": "set_pattern_param", "args": [33, -25]},
        {"at": 18.400 + offset, "op": "select_arp_pattern", "args": [5]},
        {"at": 18.650 + offset, "op": "set_arp_control", "args": [2, 127]},
        {"at": 19.000 + offset, "op": "request_arp_pattern_dump", "args": [5]},
        {"at": 20.000 + offset, "op": "send_arp_pattern_data", "args": [5],
         "bytes": raw_pattern},
        {"at": 21.000 + offset, "op": "rename_patch", "text": "STABILITY TEST"},
        {"at": 21.399 + offset, "op": "send_macro", "text": "saw"},
        {"at": 22.000 + offset, "op": "panel_pulse", "args": [2, 4]},
        {"at": 22.200 + offset, "op": "set_adin", "args": [8, 160]},
        {"at": 22.201 + offset, "op": "set_wheel2", "args": [128]},
        {"at": 22.202 + offset, "op": "set_cc_map", "args": [1, 1]},
        {"at": 22.400 + offset, "op": "send_midi", "bytes": [0x90, 60, 80]},
        {"at": 22.650 + offset, "op": "send_midi", "bytes": [0x80, 60, 0]},
        {"at": 22.401 + offset, "op": "daw_midi", "bytes": [0x90, 64, 80]},
        {"at": 22.651 + offset, "op": "daw_midi", "bytes": [0x80, 64, 0]},
        {"at": 24.000 + offset, "op": "write_patch"},
        {"at": 28.500 + offset, "op": "request_program_dump"},
    ]
    actions.sort(key=lambda action: float(action["at"]))
    return {"schema": 1, "name": "editor-boundaries", "actions": actions}


def editor_pairwise_scenario(seed: int, phase_seconds: float) -> dict[str, object]:
    """Cover ordered pairs of ten editor/DAW action families in four seed shards."""
    templates: list[tuple[str, dict[str, object]]] = [
        ("patch", {"op": "select_patch", "args": [7]}),
        ("program", {"op": "set_param", "args": [355, 23]}),
        ("global", {"op": "set_global_param", "args": [171, 1]}),
        ("pattern", {"op": "set_pattern_param", "args": [33, -12]}),
        ("arp", {"op": "select_arp_pattern", "args": [5]}),
        ("panel", {"op": "panel_pulse", "args": [2, 4]}),
        ("analog", {"op": "set_adin", "args": [8, 144]}),
        ("config", {"op": "set_cc_map", "args": [1, 1]}),
        ("editor_midi", {"op": "send_midi", "bytes": [0xb0, 1, 32]}),
        # A DAW Program Change is the strongest cross-origin collision: it opens
        # the audio-thread patch-load gate while the paired editor action enters
        # through the shared message-thread pacer.
        ("daw_midi", {"op": "daw_midi", "bytes": [0xc0, 7]}),
    ]
    boundary_offsets = (0.001, 0.015, 0.079, 0.149, 0.151, 0.399,
                        0.401, 0.649, 0.651, 0.799, 0.801)
    pairs = [(left, right) for left in range(len(templates))
             for right in range(len(templates))]
    shard = seed % 4
    selected = [pair for index, pair in enumerate(pairs) if index % 4 == shard]
    actions: list[dict[str, object]] = []
    when = 15.0 + phase_seconds
    for index, (left, right) in enumerate(selected):
        delta = boundary_offsets[(seed + index) % len(boundary_offsets)]
        for at, template_index in ((when, left), (when + delta, right)):
            action = dict(templates[template_index][1])
            action["at"] = round(at, 9)
            actions.append(action)
        when += 1.25
    return {"schema": 1, "name": "editor-pairwise", "shard": shard,
            "families": [name for name, _ in templates], "actions": actions}


def program_clock_collision_scenario(phase_seconds: float) -> dict[str, object]:
    return {"schema": 1, "name": "program-clock-collision", "actions": [
        {"at": round(15.0 + phase_seconds, 9), "op": "select_patch", "args": [7]},
    ]}


def daw_program_clock_collision_scenario(phase_seconds: float) -> dict[str, object]:
    at = round(15.150 + phase_seconds, 9)
    return {"schema": 1, "name": "daw-program-clock-collision", "actions": [
        {"at": at, "op": "daw_midi", "bytes": [0xb0, 0x00, 0x00]},
        {"at": at, "op": "daw_midi", "bytes": [0xb0, 0x20, 0x00]},
        {"at": at, "op": "daw_midi", "bytes": [0xc0, 0x07]},
    ]}


def negative_midi_overflow_scenario(phase_seconds: float) -> dict[str, object]:
    """Overfill the real 4 KiB immediate MIDI ring to prove the drop oracle."""
    at = round(15.0 + phase_seconds, 9)
    return {"schema": 1, "name": "negative-midi-overflow", "actions": [
        {"at": at, "op": "send_midi", "bytes": [0xb0, 1, index & 0x7f]}
        for index in range(5000)
    ]}


def negative_stuck_note_scenario(phase_seconds: float) -> dict[str, object]:
    """Omit a DAW note-off to prove the final-state/note-drain oracle."""
    return {"schema": 1, "name": "negative-stuck-note", "actions": [
        {"at": round(15.0 + phase_seconds, 9), "op": "daw_midi",
         "bytes": [0x90, 127, 100]},
    ]}


def editor_storm_scenario(seed: int, phase_seconds: float, seconds: float,
                          action_rate: float) -> dict[str, object]:
    """Generate a resolved, replayable mixed-action storm through production seams."""
    rng = random.Random(seed)
    start = 15.0 + phase_seconds
    end = seconds - 8.0
    interval = 1.0 / action_rate
    program_params = (105, 118, 131, 144, 355, 356, 364, 365, 371)
    panel_buttons = ((2, 4), (4, 0), (4, 1), (4, 2), (4, 3))
    actions: list[dict[str, object]] = []
    last_program_dump = -100.0
    last_arp_dump = -100.0
    wrote = False
    when = start
    while when < end:
        roll = rng.randrange(100)
        action: dict[str, object]
        if roll < 8:
            action = {"op": "select_patch", "args": [rng.randrange(128)]}
        elif roll < 36:
            action = {"op": "set_param",
                      "args": [rng.choice(program_params), rng.randrange(200)]}
        elif roll < 44:
            action = {"op": "set_pattern_param",
                      "args": [rng.randrange(1, 129), rng.randrange(-48, 128)]}
        elif roll < 49:
            action = {"op": "set_global_param", "args": [171, rng.randrange(2)]}
        elif roll < 55:
            action = {"op": "select_arp_pattern", "args": [rng.randrange(10)]}
        elif roll < 60:
            control = rng.randrange(2, 6)
            value = rng.randrange(4) if control == 3 else rng.choice((0, 127))
            action = {"op": "set_arp_control", "args": [control, value]}
        elif roll < 66:
            action = {"op": "panel_pulse", "args": list(rng.choice(panel_buttons))}
        elif roll < 73:
            action = {"op": "set_adin", "args": [rng.randrange(8, 15), rng.randrange(256)]}
        elif roll < 77:
            action = {"op": "set_wheel2", "args": [rng.randrange(256)]}
        elif roll < 80:
            action = {"op": "set_cc_map", "args": [rng.randrange(1, 32), rng.randrange(6)]}
        elif roll < 86:
            note = rng.randrange(48, 85)
            action = {"op": "send_midi", "bytes": [0x90, note, rng.randrange(40, 120)]}
            actions.append({"at": round(min(when + 0.24, end + 0.5), 9),
                            "op": "send_midi", "bytes": [0x80, note, 0]})
        elif roll < 92:
            note = rng.randrange(48, 85)
            action = {"op": "daw_midi", "bytes": [0x90, note, rng.randrange(40, 120)]}
            actions.append({"at": round(min(when + 0.24, end + 0.5), 9),
                            "op": "daw_midi", "bytes": [0x80, note, 0]})
        elif roll == 92:
            action = {"op": "rename_patch", "text": f"STORM {seed:08X}"[:16]}
        elif roll == 93:
            action = {"op": "send_macro",
                      "text": rng.choice(("saw", "filter_thru", "bypass_fx"))}
        elif roll == 94 and when - last_program_dump >= 2.0:
            action = {"op": "request_program_dump"}
            last_program_dump = when
        elif roll == 95 and when - last_arp_dump >= 4.0:
            action = {"op": "request_arp_pattern_dump", "args": [rng.randrange(10)]}
            last_arp_dump = when
        elif roll == 96:
            action = {"op": "send_arp_pattern_data", "args": [rng.randrange(10)],
                      "bytes": [rng.randrange(256) for _ in range(128)]}
        elif roll == 97 and not wrote and seconds >= 40.0:
            action = {"op": "write_patch"}
            wrote = True
        else:
            action = {"op": "set_param",
                      "args": [rng.choice(program_params), rng.randrange(200)]}
        action["at"] = round(when, 9)
        actions.append(action)
        when += interval * rng.uniform(0.75, 1.25)
    actions.append({"at": round(seconds - 4.5, 9), "op": "request_program_dump"})
    actions.append({"at": round(seconds - 4.0, 9), "op": "request_arp_pattern_dump",
                    "args": [0]})
    actions.sort(key=lambda item: float(item["at"]))
    return {"schema": 1, "name": "editor-storm", "seed": seed,
            "action_rate": action_rate, "actions": actions}


def action_coverage(scenario: dict[str, object] | None) -> dict[str, object]:
    actions = [] if scenario is None else scenario["actions"]
    assert isinstance(actions, list)
    operations = [str(action["op"]) for action in actions]
    pairs = [f"{left}->{right}" for left, right in zip(operations, operations[1:])]
    return {
        "action_count": len(operations),
        "operations": dict(sorted(Counter(operations).items())),
        "adjacent_pairs": dict(sorted(Counter(pairs).items())),
    }


def replay_seed(scenario: dict[str, object] | None) -> int:
    """Return the seed that also drives replay's simultaneous host workload."""
    if scenario is None:
        return 0x50524F50
    return int(scenario.get("seed", 0x50524F50))


def minimize_trigger_prefix(scenario: dict[str, object],
                            trigger_time: float) -> dict[str, object]:
    """Remove actions that occur after a frozen flight recorder proved the fault."""
    actions = scenario["actions"]
    assert isinstance(actions, list)
    prefix = [action for action in actions if float(action["at"]) <= trigger_time]
    minimized = dict(scenario)
    minimized["actions"] = prefix
    minimized["minimization"] = {
        "kind": "proven_trigger_prefix",
        "trigger_time_seconds": trigger_time,
        "original_action_count": len(actions),
        "retained_action_count": len(prefix),
    }
    return minimized


def compact_pass_artifacts(case_dir: Path, record: dict[str, object]) -> None:
    """Keep replay/result evidence for passes; failures always retain full artifacts."""
    for filename in ("midi.jsonl", "host.log", "control-flight.json", "error.log"):
        path = case_dir / filename
        if path.is_file():
            path.unlink()
    nvram = case_dir / "nvram"
    if nvram.is_dir():
        shutil.rmtree(nvram)
    record["artifacts"] = {
        "scenario": str(case_dir / "scenario.json"),
        "result": str(case_dir / "result.json"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--host", type=Path, default=default_host())
    parser.add_argument("--rompath", type=Path, default=default_asset("00-roms"))
    parser.add_argument("--nvram-seed", type=Path,
                        default=default_asset("nvram", "korgprop", "sysram"))
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--scenario", choices=("mixed-daw", "rapid-patch-browse",
                                                "editor-boundaries", "editor-pairwise",
                                                "program-clock-collision",
                                                "daw-program-clock-collision",
                                                "editor-storm",
                                                "negative-midi-overflow", "negative-stuck-note"),
                        default="mixed-daw")
    parser.add_argument("--replay-scenario", type=Path,
                        help="replay an exact schema-1 action timeline")
    parser.add_argument("--retain", choices=("compact", "all"), default="compact",
                        help="retain compact pass evidence or every artifact; failures are always full")
    parser.add_argument("--action-rate", type=float, default=10.0,
                        help="generated editor-storm actions per second")
    parser.add_argument("--case", action="append", type=parse_case, dest="cases")
    parser.add_argument("--seed", action="append", type=parse_int, dest="seeds")
    parser.add_argument("--phase-sample", action="append", type=int, dest="phase_samples")
    parser.add_argument("--health-interval", type=float, default=5.0)
    parser.add_argument("--health-timeout", type=float, default=3.0)
    parser.add_argument("--program-interval", type=float)
    parser.add_argument("--program-start", type=float)
    parser.add_argument("--program-value", action="append", type=int, dest="program_values")
    parser.add_argument("--max-health-misses", type=int, default=2)
    parser.add_argument("--no-setup", action="store_true")
    parser.add_argument("--no-clock", action="store_true")
    parser.add_argument("--no-notes", action="store_true")
    parser.add_argument("--no-controls", action="store_true")
    parser.add_argument("--no-params", action="store_true")
    parser.add_argument("--no-programs", action="store_true")
    args = parser.parse_args()

    replay_scenario = None
    if args.replay_scenario is not None:
        try:
            replay_scenario = load_scenario(args.replay_scenario.resolve())
        except argparse.ArgumentTypeError as exc:
            parser.error(str(exc))
        args.scenario = str(replay_scenario["name"])

    if args.program_interval is None:
        args.program_interval = 0.070 if args.scenario == "rapid-patch-browse" else 7.0
    if args.program_start is None:
        args.program_start = 15.0 if args.scenario == "rapid-patch-browse" else 18.0
    if args.scenario == "rapid-patch-browse":
        args.no_setup = True
        args.no_clock = True
        args.no_controls = True
        args.no_params = True
        if args.program_values is None:
            if replay_scenario is None:
                args.program_values = list(range(1, 11)) + list(range(9, -1, -1))
            else:
                args.program_values = [
                    int(action["args"][0]) for action in replay_scenario["actions"]
                    if action.get("op") == "select_patch" and action.get("args")]
    elif args.scenario in ("editor-boundaries", "editor-pairwise",
                           "program-clock-collision", "negative-midi-overflow",
                           "negative-stuck-note", "editor-storm"):
        args.no_programs = True
    elif args.scenario == "daw-program-clock-collision":
        args.no_programs = True

    # Patch-load atomicity intentionally gates raw DAW dump probes during a dense
    # storm. Keep retrying until the generator's quiet tail, where mandatory
    # causal editor recovery probes decide the final verdict.
    if args.scenario == "editor-storm" and args.max_health_misses == 2:
        args.max_health_misses = 30

    if args.seconds < 16.0:
        parser.error("--seconds must be at least 16")
    if args.action_rate <= 0.0 or args.action_rate > 200.0:
        parser.error("--action-rate must be in (0, 200]")
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
    # The typed timeline is only half of an exact mixed editor/DAW replay: the
    # host also generates seeded notes, controls, clock, and parameter traffic.
    # Preserve the scenario's seed unless the caller explicitly overrides it.
    seeds = args.seeds or [replay_seed(replay_scenario)]
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
                if replay_scenario is not None:
                    resolved_scenario = replay_scenario
                elif args.scenario == "rapid-patch-browse":
                    resolved_scenario = rapid_patch_scenario(
                        args.program_values or [], args.program_start,
                        args.program_interval, phase_sample / rate)
                elif args.scenario == "editor-boundaries":
                    resolved_scenario = editor_boundary_scenario(
                        seed, phase_sample / rate)
                elif args.scenario == "editor-pairwise":
                    resolved_scenario = editor_pairwise_scenario(
                        seed, phase_sample / rate)
                elif args.scenario == "program-clock-collision":
                    resolved_scenario = program_clock_collision_scenario(
                        phase_sample / rate)
                elif args.scenario == "daw-program-clock-collision":
                    resolved_scenario = daw_program_clock_collision_scenario(
                        phase_sample / rate)
                elif args.scenario == "editor-storm":
                    resolved_scenario = editor_storm_scenario(
                        seed, phase_sample / rate, args.seconds, args.action_rate)
                elif args.scenario == "negative-midi-overflow":
                    resolved_scenario = negative_midi_overflow_scenario(
                        phase_sample / rate)
                elif args.scenario == "negative-stuck-note":
                    resolved_scenario = negative_stuck_note_scenario(
                        phase_sample / rate)
                else:
                    resolved_scenario = None
                scenario_path = case_dir / "scenario.json"
                if resolved_scenario is not None:
                    scenario_path.write_text(
                        json.dumps(resolved_scenario, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
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
                    "--seconds", str(args.seconds), "--stress",
                    "--scenario", args.scenario, "--seed", str(seed),
                    "--health-interval", str(args.health_interval),
                    "--health-timeout", str(args.health_timeout),
                    "--program-interval", str(args.program_interval),
                    "--program-start", str(args.program_start),
                    "--max-health-misses", str(args.max_health_misses),
                    "--output", str(capture),
                ]
                if resolved_scenario is not None:
                    command.extend(("--scenario-file", str(scenario_path)))
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
                arp_recovery_match = ARP_RECOVERY_RE.search(log_text)
                clock_gate_match = CLOCK_GATE_RE.search(log_text)
                editor_pacer_match = EDITOR_PACER_RE.search(log_text)
                editor_recovery_match = EDITOR_RECOVERY_RE.search(log_text)
                editor_scheduler_match = EDITOR_SCHEDULER_RE.search(log_text)
                final_health = parse_final_health(log_text)
                control_flight = parse_control_flight(log_text)
                editor_board_link_drained = False
                editor_h8_sci_clean = False
                board_link_bytes_match = False
                if final_health is not None:
                    editor_board_link_drained = (
                        final_health["mailbox"]["count"] == 0 and
                        final_health["h8_command"]["count"] == 0)
                    h8_errors = final_health["h8_sci_errors"]
                    editor_h8_sci_clean = (h8_errors is not None and
                                           h8_errors["count"] == 0)
                    wire = final_health["board_link_bytes"]
                    board_link_bytes_match = (
                        wire is not None and wire["compared"] > 0 and
                        wire["mismatches"] == 0 and
                        wire["unexpected"] == 0 and wire["pending"] == 0)
                checks = {
                    "host_exit": returncode == 0,
                    "no_timeout": not timed_out,
                    "health_summary": summary_match is not None,
                    "health_pass": summary_match is not None and summary_match.group(6) == "PASS",
                    "midi_output_no_drops": (drops_match is not None and
                                             drops_match.group("output") == "0"),
                    "midi_input_no_drops": (drops_match is not None and
                                            drops_match.group("input") == "0"),
                    "ui_adin_no_drops": (drops_match is not None and
                                         drops_match.group("ui_adin") == "0"),
                    "audio_adin_no_drops": (drops_match is not None and
                                            drops_match.group("audio_adin") == "0"),
                    "no_oversized_blocks": (drops_match is not None and
                                            drops_match.group("oversized") == "0"),
                    "arp_dump_fresh": (arp_recovery_match is not None and
                                       arp_recovery_match.group("verdict") == "PASS"),
                    "clock_gate_accounted": clock_gate_match is not None,
                    "editor_command_pacer_bounded": (
                        editor_pacer_match is not None and
                        editor_pacer_match.group("verdict") == "PASS"),
                    "program_clock_collision_gated": (
                        args.scenario not in ("program-clock-collision",
                                              "daw-program-clock-collision") or
                        (clock_gate_match is not None and
                         int(clock_gate_match.group("count")) > 0)),
                    "editor_recovery": (args.scenario != "rapid-patch-browse" or
                                        (editor_recovery_match is not None and
                                         editor_recovery_match.group(3) == "PASS")),
                    "editor_scheduler_bounded": (
                        args.scenario != "rapid-patch-browse" or
                        (editor_scheduler_match is not None and
                         editor_scheduler_match.group(5) == "PASS")),
                    "board_link_flight_healthy": (control_flight is not None and
                                                  not control_flight["triggered"]),
                    "editor_board_link_drained": editor_board_link_drained,
                    "editor_h8_sci_clean": editor_h8_sci_clean,
                    "board_link_bytes_match": board_link_bytes_match,
                }
                health = None if summary_match is None else {
                    "attempts": int(summary_match.group(1)),
                    "replies": int(summary_match.group(2)),
                    "misses": int(summary_match.group(3)),
                    "worst_consecutive_misses": int(summary_match.group(4)),
                    "max_latency_seconds": float(summary_match.group(5)),
                }
                editor_pacer = None if editor_pacer_match is None else {
                    key: int(editor_pacer_match.group(key))
                    for key in ("sent", "coalesced", "cancelled", "dropped", "pending")
                }
                fault_signature = classify_final_health(final_health, checks["health_pass"])
                observed_pass = all(checks.values())
                negative_control = args.scenario.startswith("negative-")
                if args.scenario == "negative-midi-overflow":
                    negative_control_detected = (
                        not checks["midi_input_no_drops"] and checks["no_timeout"])
                elif args.scenario == "negative-stuck-note":
                    negative_control_detected = (
                        not checks["health_pass"] and not checks["host_exit"] and
                        checks["midi_input_no_drops"] and checks["no_timeout"])
                else:
                    negative_control_detected = False
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
                minimized_artifact = None
                if (resolved_scenario is not None and control_flight is not None and
                        control_flight["triggered"]):
                    minimized = minimize_trigger_prefix(
                        resolved_scenario, float(control_flight["trigger_time_seconds"]))
                    minimized_artifact = case_dir / "minimized-prefix.json"
                    minimized_artifact.write_text(
                        json.dumps(minimized, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
                record = {
                    "name": name, "seed": seed, "rate": rate, "block": block,
                    "phase_sample": phase_sample,
                    "seconds": args.seconds, "scenario": args.scenario,
                    "checks": checks, "health": health,
                    "editor_command_pacer": editor_pacer,
                    "final_health": final_health, "fault_signature": fault_signature,
                    "control_flight": flight_summary,
                    "action_coverage": action_coverage(resolved_scenario),
                    "observed_verdict": "PASS" if observed_pass else "FAIL",
                    "negative_control": negative_control,
                    "negative_control_detected": negative_control_detected,
                    "verdict": ("PASS" if negative_control_detected else "FAIL")
                    if negative_control else ("PASS" if observed_pass else "FAIL"),
                    "command": command,
                    "artifacts": {"midi": str(capture), "host_log": str(log)},
                }
                error_log = case_dir / "error.log"
                if error_log.is_file():
                    record["artifacts"]["mame_error_log"] = str(error_log)
                if flight_artifact is not None:
                    record["artifacts"]["control_flight"] = str(flight_artifact)
                if minimized_artifact is not None:
                    record["artifacts"]["minimized_prefix"] = str(minimized_artifact)
                records.append(record)
                if (args.retain == "compact" and record["verdict"] == "PASS" and
                        not negative_control):
                    compact_pass_artifacts(case_dir, record)
                (case_dir / "result.json").write_text(
                    json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                print(f"  {record['verdict']} health={health}")

    summary = {
        "schema": 1,
        "scenario": args.scenario,
        "host": str(args.host.resolve()),
        "rompath": str(args.rompath.resolve()),
        "phase_samples": phases,
        "program_interval": args.program_interval,
        "program_start": args.program_start,
        "program_values": args.program_values,
        "retention": args.retain,
        "action_rate": args.action_rate,
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
