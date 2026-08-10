#!/usr/bin/env python3
"""Synthetic regression tests for the physical arpeggiator capture grader."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import grade_arp_hardware_capture as hardware


def write_jsonl(path: Path, records: list[tuple[float, int]]) -> None:
    records.sort()
    path.write_text("".join(json.dumps({"t": timestamp, "byte": byte}) + "\n"
                            for timestamp, byte in records), encoding="utf-8")


def message(records: list[tuple[float, int]], completion: float, values: tuple[int, ...]) -> None:
    spacing = 0.00032
    start = completion - spacing * (len(values) - 1)
    records.extend((start + index * spacing, value) for index, value in enumerate(values))


def rhythmic_capture(external: bool) -> tuple[list[tuple[float, int]], list[tuple[float, int]]]:
    clock = 60.0 / (24.0 * 120.0)
    midi_in: list[tuple[float, int]] = []
    midi_out: list[tuple[float, int]] = []
    clocks = [(index * clock, 0xF8) for index in range(220)]
    (midi_in if external else midi_out).extend(clocks)
    notes = (60, 64, 67, 71)
    for index in range(12):
        onset = 1.0 + index * 12 * clock
        note = notes[index % len(notes)]
        message(midi_out, onset, (0x90, note, 100))
        message(midi_out, onset + 10 * clock, (0x80, note, 0))
    return midi_in, midi_out


class HardwareGraderTest(unittest.TestCase):
    def test_external_subdivision_and_gate_pass(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            midi_in, midi_out = rhythmic_capture(external=True)
            write_jsonl(root / "in.jsonl", midi_in)
            write_jsonl(root / "out.jsonl", midi_out)
            result = hardware.grade_take(root, {
                "name": "external_eighth", "kind": "external",
                "midi_in": "in.jsonl", "midi_out": "out.jsonl",
                "expected_clocks_per_onset": 12,
            })
            self.assertEqual(result["verdict"], "PASS", result["checks"])
            self.assertAlmostEqual(result["clocks_per_onset"], 12.0, places=6)
            self.assertAlmostEqual(result["external_gate_clocks"][0], 10.0, places=6)

    def test_speed_without_adc_is_explicitly_ungradable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            _, midi_out = rhythmic_capture(external=False)
            write_jsonl(root / "out.jsonl", midi_out)
            result = hardware.grade_take(root, {
                "name": "speed_mid", "kind": "speed", "midi_out": "out.jsonl",
                "speed_position": 0.5, "speed_adc_code": None,
                "expected_clocks_per_onset": 12,
            })
            self.assertEqual(result["verdict"], "CAPTURED-UNGRADABLE")
            self.assertTrue(any(item["label"] == "speed-hardware-equivalence"
                                for item in result["checks"]))

    def test_loopback_common_clock_quality(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = [(index * 0.002, 0xF8 if index % 3 else 0x90) for index in range(240)]
            returned = [(timestamp + 0.001 + ((index % 5) - 2) * 0.00001, byte)
                        for index, (timestamp, byte) in enumerate(source)]
            write_jsonl(root / "source.jsonl", source)
            write_jsonl(root / "return.jsonl", returned)
            result = hardware.grade_take(root, {
                "name": "loopback", "kind": "loopback",
                "midi_in": "source.jsonl", "midi_out": "return.jsonl",
            })
            self.assertEqual(result["verdict"], "PASS", result["checks"])
            self.assertLess(result["loopback"]["p95_abs_error_s"], 0.0005)

    def test_keysync_relational_cross_checks(self) -> None:
        takes = [
            {"kind": "keysync", "key_sync": False, "phase": "a"},
            {"kind": "keysync", "key_sync": False, "phase": "b"},
            {"kind": "keysync", "key_sync": True, "phase": "a"},
            {"kind": "keysync", "key_sync": True, "phase": "b"},
        ]
        results = [
            {"input_note_s": 1.000, "first_output_note_s": 1.083,
             "latency_s": 0.083},
            {"input_note_s": 1.040, "first_output_note_s": 1.083,
             "latency_s": 0.043},
            {"input_note_s": 2.000, "first_output_note_s": 2.003,
             "latency_s": 0.003},
            {"input_note_s": 2.040, "first_output_note_s": 2.0431,
             "latency_s": 0.0031},
        ]
        checks = hardware.cross_checks(takes, results)
        self.assertEqual(len(checks), 4)
        self.assertTrue(all(check["state"] == "PASS" for check in checks), checks)

    def test_template_contains_all_required_capture_classes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = hardware.write_template(Path(temporary) / "campaign")
            document = json.loads(path.read_text(encoding="utf-8"))
            kinds = {take["kind"] for take in document["takes"]}
            self.assertEqual(kinds, hardware.VALID_KINDS)
            self.assertEqual(len(document["takes"]), 58)
            self.assertEqual(sum(take["kind"] == "external" for take in document["takes"]), 18)
            self.assertEqual(sum(take["kind"] == "speed" for take in document["takes"]), 15)
            self.assertEqual(sum(take["kind"] == "offset" for take in document["takes"]), 3)
            self.assertEqual(sum(take["kind"] == "keysync" for take in document["takes"]), 12)
            self.assertTrue(all(take.get("minimum_note_onsets") == 32
                                for take in document["takes"]
                                if take["kind"] in {"internal", "external"}))
            self.assertTrue(all("fixture_case" in take for take in document["takes"]
                                if take["kind"] != "loopback"))
            self.assertTrue((path.parent / "fixtures" / "keysync.json").is_file())
            hardware.validate_manifest(document, path.parent)
            incomplete = hardware.campaign_checks(document, path.parent)
            self.assertTrue(any(check["state"] == "CAPTURED-UNGRADABLE"
                                for check in incomplete))
            fixture = path.parent / "fixtures" / "keysync.json"
            fixture.write_text(fixture.read_text(encoding="utf-8") + " ", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "SHA-256 changed"):
                hardware.validate_manifest(document, path.parent)

    def test_24_bit_pcm_capture_is_supported(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "capture.wav"
            values = [-8_388_608, -123_456, 0, 123_456, 8_388_607, 42]
            with wave.open(str(path), "wb") as output:
                output.setnchannels(2)
                output.setsampwidth(3)
                output.setframerate(96_000)
                output.writeframes(b"".join(value.to_bytes(3, "little", signed=True)
                                            for value in values))
            rate, channels, audio = hardware.audio_timing.read_pcm_wav(path)
            self.assertEqual((rate, channels), (96_000, 2))
            self.assertEqual(audio.astype(int).reshape(-1).tolist(), values)


if __name__ == "__main__":
    unittest.main()
