# SPDX-License-Identifier: AGPL-3.0-only
"""The capture gate must reject silence and tail loss even with a valid onset."""
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
from scipy.io import wavfile

import analyze_plugin_latency as analyzer


class CaptureAcceptanceTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.prefix = Path(self.directory.name) / 'capture'
        self.reference = Path(self.directory.name) / 'reference.wav'
        self.audio = np.zeros((96000, 2), dtype=np.float32)
        self.audio[24096:95000] = [0.1, -0.05]
        wavfile.write(self.reference, 48000, self.audio)
        self.write_audio()
        self.receipt = dict(sample_rate=48000, final_latency_samples=64,
                            notes=[dict(sample=24000, seconds=.5)], process_failed=False)
        self.write_receipt()
        self.prefix.with_suffix('.blocks.csv').write_text(
            'sample,count,late_ms,render_ms,reported_latency_samples,realtime\n'
            '0,128,0.01,0.02,64,1\n'
            '128,128,-100,100,64,0\n'
            '256,128,0.01,0.02,64,1\n')

    def write_audio(self):
        wavfile.write(self.prefix.with_suffix('.wav'), 48000, self.audio)

    def write_receipt(self):
        self.prefix.with_suffix('.json').write_text(json.dumps(self.receipt))

    def analyze(self):
        return analyzer.analyze(self.prefix, self.reference)

    def test_matching_capture_passes_and_counts_return_to_realtime(self):
        result = self.analyze()
        self.assertTrue(result['acceptance']['passed'])
        self.assertEqual(result['host_pacing']['paced_callback_count'], 2)
        self.assertEqual(result['host_pacing']['callback_deadline_misses'], 0)

    def test_silence_fails_even_with_identical_reference(self):
        self.audio.fill(0)
        self.write_audio()
        wavfile.write(self.reference, 48000, self.audio)
        self.assertFalse(self.analyze()['acceptance']['passed'])

    def test_lost_tail_fails_despite_correct_onset(self):
        self.audio[72000:] = 0
        self.write_audio()
        result = self.analyze()
        self.assertEqual(result['notes'][0]['thresholds']['0.0001']['delay_samples'], 96)
        self.assertFalse(result['acceptance']['passed'])
        self.assertEqual(result['pcm_reference']['first_difference_frame'], 72000)

    def test_nonfinite_audio_fails(self):
        self.audio[80000] = np.nan
        self.write_audio()
        self.assertFalse(self.analyze()['audio_finite'])
        self.assertFalse(self.analyze()['acceptance']['passed'])

    def test_native_process_failure_fails(self):
        self.receipt['process_failed'] = True
        self.write_receipt()
        self.assertFalse(self.analyze()['acceptance']['passed'])

    def test_reference_is_required_for_acceptance(self):
        self.assertFalse(analyzer.analyze(self.prefix)['acceptance']['passed'])

    def test_cli_failure_is_nonzero(self):
        self.audio[72000:] = 0
        self.write_audio()
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(analyzer.main([str(self.prefix), '--reference', str(self.reference)]), 1)

    def test_measurement_mode_does_not_claim_acceptance(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(analyzer.main([str(self.prefix), '--measure-only']), 0)
        result = json.loads(self.prefix.with_suffix('.analysis.json').read_text())
        self.assertFalse(result['acceptance']['passed'])


if __name__ == '__main__':
    unittest.main()
