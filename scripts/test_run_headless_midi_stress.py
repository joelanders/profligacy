#!/usr/bin/env python3

import argparse
import unittest

import run_headless_midi_stress as stress


class HeadlessMidiStressTest(unittest.TestCase):
    def test_case_parser(self) -> None:
        self.assertEqual((48000, 512), stress.parse_case("48000:512"))
        with self.assertRaises(argparse.ArgumentTypeError):
            stress.parse_case("48000")
        with self.assertRaises(argparse.ArgumentTypeError):
            stress.parse_case("48000:0")

    def test_health_summary_parser(self) -> None:
        match = stress.SUMMARY_RE.search(
            "[stress-health] SUMMARY attempts=9 replies=8 misses=1 "
            "worst_consecutive_misses=1 max_latency=0.417 verdict=PASS")
        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual(("9", "8", "1", "1", "0.417", "PASS"), match.groups())

    def test_final_health_parser_and_board_link_classification(self) -> None:
        line = (
            "KPROP_FINAL_HEALTH,T=60.018958,VPC=0B400B,HPC=01093C,"
            "MB=027A/0252/0028,CTRL=021E/0208,A716=00,A721=FF,"
            "H8CMD=349/343/006,SCI=00/0B/70/FF/84/00,"
            "H8I=02/02/00,"
            "H8P=123/01ABCD/22.500000/120/01BCDE/22.750000,"
            "H8E=4/20/01B190/22.400000/4/0/0,"
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
        self.assertEqual("internal_board_link_stall",
                         stress.classify_final_health(snapshot, health_pass=False))
        self.assertIsNone(stress.classify_final_health(snapshot, health_pass=True))

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
