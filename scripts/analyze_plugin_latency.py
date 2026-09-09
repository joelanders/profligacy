#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Measure a controlled latency capture and gate its complete PCM against a reference."""
import argparse
import csv
import json
from pathlib import Path
import sys
import numpy as np
from scipy.io import wavfile


def analyze(prefix, reference=None, max_onset_ms=100.0):
    prefix = Path(prefix)
    receipt = json.loads(prefix.with_suffix('.json').read_text())
    rate, pcm = wavfile.read(prefix.with_suffix('.wav'))
    audio = pcm.astype(np.float64)
    if np.issubdtype(pcm.dtype, np.signedinteger):
        audio /= 2 ** (8 * pcm.dtype.itemsize - 1)
    elif not np.issubdtype(pcm.dtype, np.floating):
        raise ValueError('Expected signed integer or floating-point PCM')
    level = np.max(np.abs(audio), axis=1)
    reported = receipt['final_latency_samples'] * 1000 / rate
    notes = []
    for event in receipt['notes']:
        sample = event['sample']
        pre = level[max(0,sample-round(.25*rate)):sample]
        post = level[sample:min(len(level), sample+round(2*rate))]
        pre_peak = float(np.max(pre)) if len(pre) else 0.0
        post_peak = float(np.max(post)) if len(post) else 0.0
        row = dict(event, pre_peak=pre_peak, post_peak=post_peak, thresholds={})
        for threshold in [1/32768, 1e-4, 1e-3]:
            found = np.flatnonzero(post > threshold)
            onset = int(found[0]) if len(found) else None
            key = str(threshold)
            row['thresholds'][key] = {
                'prequiet': bool(pre_peak <= threshold),
                'delay_samples': onset,
                'delay_ms': None if onset is None else onset*1000/rate,
                'residual_after_reported_ms': None if onset is None else onset*1000/rate-reported,
            }
        notes.append(row)
    with prefix.with_suffix('.blocks.csv').open() as f:
        blocks = list(csv.DictReader(f))
    offline_at = receipt.get('offline_after_seconds', -1)
    timed_blocks = [b for b in blocks if b['realtime'] == '1'] if blocks and 'realtime' in blocks[0] else blocks if offline_at < 0 or receipt.get('pace_offline',False) else [b for b in blocks if int(b['sample']) < offline_at*rate]
    late = np.array([float(b['late_ms']) for b in timed_blocks])
    render = np.array([float(b['render_ms']) for b in timed_blocks])
    sizes = np.array([int(b['count']) for b in timed_blocks])
    result = {
        'schema': 1, 'receipt': receipt, 'reported_ms': reported,
        'audio_finite': bool(np.isfinite(audio).all()),
        'notes': notes,
        'host_pacing': {
            'scope': 'Only wall-clock-paced callbacks; fast offline callbacks excluded.',
            'paced_callback_count': len(timed_blocks),
            'all_render_seconds': sum(float(b['render_ms']) for b in blocks) / 1000,
            'late_p50_ms': float(np.percentile(late,50)) if len(late) else None,
            'late_p99_ms': float(np.percentile(late,99)) if len(late) else None,
            'late_max_ms': float(np.max(late)) if len(late) else None,
            'late_over_block_count': int(np.sum(late > sizes*1000/rate)),
            'callback_deadline_misses': int(np.sum(late + render > sizes*1000/rate)),
            'render_p99_ms': float(np.percentile(render,99)) if len(render) else None,
            'render_max_ms': float(np.max(render)) if len(render) else None,
            'reported_values': sorted({int(b['reported_latency_samples']) for b in blocks}),
        },
        'interpretation': 'Audio onset includes patch/firmware response. Residual is not automatically all wrapper error. Reject onset thresholds whose prequiet is false.',
    }
    errors = []
    if rate != receipt['sample_rate']:
        errors.append('WAV sample rate differs from host receipt')
    if not result['audio_finite']:
        errors.append('audio contains non-finite samples')
    if receipt.get('process_failed', False):
        errors.append('native plugin process call failed')
    if not notes:
        errors.append('capture contains no scheduled test notes')
    if result['host_pacing']['reported_values'] != [receipt['final_latency_samples']]:
        errors.append('reported latency changed during the capture')
    for row in notes:
        onset = row['thresholds']['0.0001']
        if not 0 <= row['sample'] < len(audio):
            errors.append(f"note at {row['seconds']}s is outside the recording")
        elif not onset['prequiet']:
            errors.append(f"note at {row['seconds']}s has no quiet onset reference")
        elif onset['delay_ms'] is None:
            errors.append(f"note at {row['seconds']}s produced no detectable audio")
        elif not reported <= onset['delay_ms'] <= max_onset_ms:
            errors.append(f"note at {row['seconds']}s onset is outside the accepted latency range")
    if reference is not None:
        reference_rate, expected = wavfile.read(reference)
        if reference_rate != rate or expected.shape != pcm.shape or expected.dtype != pcm.dtype:
            errors.append('complete PCM reference format or length differs')
            result['pcm_reference'] = {'path': str(reference), 'matches': False}
        else:
            differences = np.flatnonzero(np.any(pcm != expected, axis=1))
            result['pcm_reference'] = {
                'path': str(reference), 'matches': len(differences) == 0,
                'different_frames': len(differences),
                'first_difference_frame': int(differences[0]) if len(differences) else None,
            }
            if len(differences):
                errors.append('complete PCM differs from reference')
    else:
        errors.append('complete PCM reference is required for acceptance')
    result['acceptance'] = {'passed': not errors, 'errors': errors}
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('prefix', type=Path)
    parser.add_argument('--reference', type=Path, help='Complete matching WAV; exact PCM comparison')
    parser.add_argument('--max-onset-ms', type=float, default=100.0)
    parser.add_argument('--measure-only', action='store_true',
                        help='Write measurements without claiming acceptance; allows creating a reference')
    args = parser.parse_args(argv)
    result = analyze(args.prefix, args.reference, args.max_onset_ms)
    args.prefix.with_suffix('.analysis.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f"Reported latency: {result['reported_ms']:.6f} ms")
    for row in result['notes']:
        onset = row['thresholds']['0.0001']
        print(f"note at {row['seconds']:.3f}s: quiet={onset['prequiet']} delay={onset['delay_ms']} ms")
    print(json.dumps(result['host_pacing'], indent=2))
    if args.measure_only:
        print('MEASUREMENTS ONLY: capture has not been accepted against complete reference PCM')
        return 0
    for error in result['acceptance']['errors']:
        print(f'FAIL: {error}', file=sys.stderr)
    return 0 if result['acceptance']['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
