#!/usr/bin/env python3

import argparse
import tempfile
import unittest
from pathlib import Path

import run_headless_midi_stress as stress


class HeadlessMidiStressTest(unittest.TestCase):
    def test_case_parser(self) -> None:
        self.assertEqual((48000, 512), stress.parse_case("48000:512"))
        with self.assertRaises(argparse.ArgumentTypeError):
            stress.parse_case("48000")
        with self.assertRaises(argparse.ArgumentTypeError):
            stress.parse_case("48000:0")

    def test_scenario_generation_loading_and_coverage(self) -> None:
        scenario = stress.rapid_patch_scenario([1, 2, 0], 15.0, 0.07, 0.001)
        self.assertEqual("rapid-patch-browse", scenario["name"])
        self.assertEqual(4, len(scenario["actions"]))
        self.assertEqual(15.491, scenario["actions"][-1]["at"])
        coverage = stress.action_coverage(scenario)
        self.assertEqual({"request_program_dump": 1, "select_patch": 3},
                         coverage["operations"])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "scenario.json"
            path.write_text(__import__("json").dumps(scenario), encoding="utf-8")
            self.assertEqual(scenario, stress.load_scenario(path))

    def test_exact_replay_inherits_saved_background_seed(self) -> None:
        self.assertEqual(202, stress.replay_seed({"schema": 1, "seed": 202}))
        self.assertEqual(0x50524F50, stress.replay_seed({"schema": 1}))
        self.assertEqual(0x50524F50, stress.replay_seed(None))

    def test_scenario_loader_rejects_out_of_order_actions(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "scenario.json"
            path.write_text(
                '{"schema":1,"name":"bad","actions":['
                '{"at":2,"op":"set_param"},{"at":1,"op":"set_param"}]}',
                encoding="utf-8")
            with self.assertRaises(argparse.ArgumentTypeError):
                stress.load_scenario(path)

    def test_boundary_scenario_models_every_action_operation(self) -> None:
        scenario = stress.editor_boundary_scenario(0x1234, 0.0)
        operations = {action["op"] for action in scenario["actions"]}
        self.assertEqual({
            "select_patch", "request_program_dump", "set_param", "set_global_param",
            "set_pattern_param", "select_arp_pattern", "set_arp_control",
            "request_arp_pattern_dump", "send_arp_pattern_data", "rename_patch",
            "send_macro", "panel_pulse", "set_adin", "set_wheel2", "set_cc_map",
            "send_midi", "daw_midi", "write_patch",
        }, operations)
        arp_load = next(action for action in scenario["actions"]
                        if action["op"] == "send_arp_pattern_data")
        self.assertEqual(128, len(arp_load["bytes"]))

    def test_four_pairwise_shards_cover_all_ordered_family_pairs(self) -> None:
        observed: set[tuple[str, str]] = set()
        op_to_family = {
            "select_patch": "patch", "set_param": "program",
            "set_global_param": "global", "set_pattern_param": "pattern",
            "select_arp_pattern": "arp", "panel_pulse": "panel",
            "set_adin": "analog", "set_cc_map": "config",
            "send_midi": "editor_midi", "daw_midi": "daw_midi",
        }
        for seed in range(4):
            scenario = stress.editor_pairwise_scenario(seed, 0.0)
            actions = scenario["actions"]
            self.assertEqual(50, len(actions))
            self.assertTrue(all(action["bytes"] == [0xc0, 7]
                                for action in actions
                                if action["op"] == "daw_midi"))
            for index in range(0, len(actions), 2):
                observed.add((op_to_family[actions[index]["op"]],
                              op_to_family[actions[index + 1]["op"]]))
        self.assertEqual(100, len(observed))

    def test_program_clock_collision_is_one_exact_editor_action(self) -> None:
        scenario = stress.program_clock_collision_scenario(32 / 48000)
        self.assertEqual("program-clock-collision", scenario["name"])
        self.assertEqual([{"at": 15.000666667, "op": "select_patch", "args": [7]}],
                         scenario["actions"])

    def test_negative_controls_inject_exact_drop_and_final_state_faults(self) -> None:
        overflow = stress.negative_midi_overflow_scenario(0.0)
        self.assertEqual(5000, len(overflow["actions"]))
        self.assertTrue(all(action["op"] == "send_midi"
                            for action in overflow["actions"]))
        stuck = stress.negative_stuck_note_scenario(0.0)
        self.assertEqual([0x90, 127, 100], stuck["actions"][0]["bytes"])

    def test_daw_program_collision_uses_ordered_midi_events(self) -> None:
        scenario = stress.daw_program_clock_collision_scenario(0.0)
        self.assertEqual(["daw_midi", "daw_midi", "daw_midi"],
                         [action["op"] for action in scenario["actions"]])
        self.assertEqual([0xc0, 7], scenario["actions"][-1]["bytes"])

    def test_seeded_editor_storm_is_exact_paired_and_bounded(self) -> None:
        first = stress.editor_storm_scenario(123, 0.0, 45.0, 12.0)
        second = stress.editor_storm_scenario(123, 0.0, 45.0, 12.0)
        self.assertEqual(first, second)
        self.assertGreater(len(first["actions"]), 250)
        self.assertTrue(all(float(action["at"]) <= 41.0
                            for action in first["actions"]))
        self.assertEqual(["request_program_dump", "request_arp_pattern_dump"],
                         [action["op"] for action in first["actions"][-2:]])

    def test_flight_trigger_minimizer_removes_only_causally_late_actions(self) -> None:
        scenario = {"schema": 1, "name": "failure", "actions": [
            {"at": 15.0, "op": "select_patch", "args": [7]},
            {"at": 15.001, "op": "select_patch", "args": [7]},
            {"at": 16.25, "op": "set_param", "args": [355, 1]},
        ]}
        minimized = stress.minimize_trigger_prefix(scenario, 15.724)
        self.assertEqual(2, len(minimized["actions"]))
        self.assertEqual("proven_trigger_prefix", minimized["minimization"]["kind"])
        self.assertEqual(3, len(scenario["actions"]))

    def test_health_summary_parser(self) -> None:
        match = stress.SUMMARY_RE.search(
            "[stress-health] SUMMARY attempts=9 replies=8 misses=1 "
            "worst_consecutive_misses=1 max_latency=0.417 verdict=PASS")
        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual(("9", "8", "1", "1", "0.417", "PASS"), match.groups())

    def test_extended_drop_and_arp_oracles(self) -> None:
        drops = stress.DROPS_RE.search(
            "output_dropped=0 input_dropped=0 scheduled_input_dropped=0 "
            "immediate_input_dropped=0 ui_adin_dropped=0 audio_adin_dropped=0 "
            "oversized_blocks=0")
        self.assertIsNotNone(drops)
        assert drops is not None
        self.assertEqual("0", drops.group("ui_adin"))
        arp = stress.ARP_RECOVERY_RE.search(
            "[arp-recovery] SUMMARY requested=1 replied=1 pending=0 "
            "max_latency=0.225 verdict=PASS")
        self.assertIsNotNone(arp)
        assert arp is not None
        self.assertEqual("PASS", arp.group("verdict"))

    def test_editor_recovery_parser(self) -> None:
        match = stress.EDITOR_RECOVERY_RE.search(
            '[editor-recovery] final_lcd="A00: Prophecy" expected=A00: verdict=PASS')
        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual(("A00: Prophecy", "A00:", "PASS"), match.groups())

    def test_editor_scheduler_parser(self) -> None:
        match = stress.EDITOR_SCHEDULER_RE.search(
            "[editor-scheduler] patch_intents=20 patch_sends=1 "
            "dump_requests=1 dump_sends=2 verdict=PASS")
        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual(("20", "1", "1", "2", "PASS"), match.groups())

    def test_editor_command_pacer_parser(self) -> None:
        match = stress.EDITOR_PACER_RE.search(
            "[editor-command-pacer] sent=44 coalesced=7 cancelled=3 "
            "dropped=0 pending=0 verdict=PASS")
        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual("0", match.group("dropped"))
        self.assertEqual("PASS", match.group("verdict"))

    def test_final_health_parser_and_board_link_classification(self) -> None:
        line = (
            "KPROP_FINAL_HEALTH,T=60.018958,VPC=0B400B,HPC=01093C,"
            "MB=027A/0252/0028,CTRL=021E/0208,A716=00,A721=FF,"
            "H8CMD=349/343/006,SCI=00/0B/70/FF/84/00,"
            "H8I=02/02/00,"
            "H8P=123/01ABCD/22.500000/120/01BCDE/22.750000,"
            "H8E=4/20/01B190/22.400000/4/0/0,"
            "WIRE=2190/1/0/3/30/60/15.555780,"
            "IRQ=2126/1866/1866/260,TXD=7130/1,"
            "U1=486/486/0/0/20/00,U0=48/20/00/0/0/0/0,"
            "IRQS=00/00,IC=10/11/10/12")
        snapshot = stress.parse_final_health(line)
        self.assertIsNotNone(snapshot)
        assert snapshot is not None
        self.assertEqual(0x0b400b, snapshot["v55_pc"])
        self.assertEqual(0x28, snapshot["mailbox"]["count"])
        self.assertEqual(486, snapshot["uart1"]["consumed"])
        self.assertEqual([2, 2, 0], snapshot["h8_intc"])
        self.assertEqual(120, snapshot["h8_command_pointer_writes"]["read"]["count"])
        self.assertEqual(0x01bcde,
                         snapshot["h8_command_pointer_writes"]["read"]["pc"])
        self.assertEqual(4, snapshot["h8_sci_errors"]["overruns"])
        self.assertEqual(0, snapshot["h8_sci_errors"]["framing_errors"])
        self.assertEqual(2190, snapshot["board_link_bytes"]["compared"])
        self.assertEqual(1, snapshot["board_link_bytes"]["mismatches"])
        self.assertEqual(0x30, snapshot["board_link_bytes"]["last_expected"])
        self.assertEqual(0x60, snapshot["board_link_bytes"]["last_actual"])
        self.assertEqual("board_link_byte_mismatch",
                         stress.classify_final_health(snapshot, health_pass=False))
        self.assertEqual("board_link_byte_mismatch",
                         stress.classify_final_health(snapshot, health_pass=True))
        snapshot["board_link_bytes"]["mismatches"] = 0
        snapshot["board_link_bytes"]["pending"] = 0
        self.assertEqual("internal_board_link_stall",
                         stress.classify_final_health(snapshot, health_pass=False))

    def test_final_health_unclassified_when_uart1_is_lossy(self) -> None:
        line = (
            "KPROP_FINAL_HEALTH,T=1.0,VPC=000001,HPC=000002,"
            "MB=0001/0000/0001,CTRL=0000/0000,A716=00,A721=00,"
            "H8CMD=001/000/001,SCI=00/00/00/00/00/00,"
            "IRQ=0/0/0/0,TXD=0/1,U1=2/1/1/0/20/00,"
            "U0=48/20/00/0/0/0/0,IRQS=00/00,IC=10/11/10/12")
        snapshot = stress.parse_final_health(line)
        self.assertIsNone(snapshot["h8_intc"])
        self.assertIsNone(snapshot["h8_command_pointer_writes"])
        self.assertIsNone(snapshot["h8_sci_errors"])
        self.assertIsNone(snapshot["board_link_bytes"])
        self.assertEqual("unclassified_health_failure",
                         stress.classify_final_health(snapshot, health_pass=False))

    def test_control_flight_parser(self) -> None:
        log = "\n".join((
            "KPROP_FLIGHT_BEGIN,COUNT=2,TOTAL=19,TRIGGERED=1,TRIGGER_T=25.250000",
            "KPROP_FLIGHT,I=0,T=25.249000,E=V2H,D=7F,VPC=0B400B,HPC=01AAB6,"
            "MB=027A/0252/0028,CTRL=021E/0208,H8CMD=349/343,"
            "A716=00,A721=FF,U0=48/20/10,IRQS=00/00,SCI=84/7F,"
            "H8E=0/00/000000/0.000000000",
            "KPROP_FLIGHT,I=1,T=25.250000,E=STALL,D=00,VPC=0B400B,HPC=01093C,"
            "MB=027A/0252/0028,CTRL=021E/0208,H8CMD=349/343,"
            "A716=00,A721=FF,U0=48/20/30,IRQS=00/00,SCI=84/00,"
            "H8E=1/10/01093C/25.249943000",
            "KPROP_FLIGHT_END",
        ))
        flight = stress.parse_control_flight(log)
        self.assertIsNotNone(flight)
        assert flight is not None
        self.assertTrue(flight["triggered"])
        self.assertEqual(19, flight["total_events"])
        self.assertEqual(2, len(flight["events"]))
        self.assertEqual("V2H", flight["events"][0]["event"])
        self.assertEqual(0x7f, flight["events"][0]["data"])
        self.assertEqual(0x28, flight["events"][1]["mailbox"]["count"])
        self.assertEqual(1, flight["events"][1]["h8_sci"]["errors"]["count"])
        self.assertEqual(0x01093c,
                         flight["events"][1]["h8_sci"]["errors"]["last_pc"])


if __name__ == "__main__":
    unittest.main()
