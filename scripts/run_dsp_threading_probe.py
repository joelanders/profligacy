#!/usr/bin/env python3
"""Run the real engine timeline probe with isolated state and reproducible inputs.

Never infer realtime health from an offline render. Paced missing frames and
host wake-up lateness are reported separately; both modes retain the plugin's
existing latency. Compare serial and multi-core DSP runs for exact PCM and MIDI.
"""
from pathlib import Path
import argparse
import array
import gzip
import hashlib
import json
import math
import os
import platform
import resource
import shutil
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', required=True, type=Path)
    ap.add_argument('--rompath', required=True, type=Path)
    ap.add_argument('--nvram-seed', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    ap.add_argument('--engine', choices=['dynarec', 'interpreter'], default='dynarec')
    ap.add_argument('--block', type=int, default=128)
    ap.add_argument('--seconds', type=float, default=8)
    ap.add_argument('--speed', type=float, default=1)
    ap.add_argument('--paced', action='store_true')
    ap.add_argument('--dense', action='store_true')
    ap.add_argument('--programs', action='store_true')
    ap.add_argument('--all-programs', action='store_true', help='Visit all 128 program numbers; requires at least 130 seconds')
    ap.add_argument('--digest', action='store_true', help='Record DSP state and delay-RAM checkpoints for comparison')
    ap.add_argument('--multicore', action='store_true', help='Enable multi-core DSP execution (the DSP cluster)')
    ap.add_argument('--multicore-toggle-blocks', type=int, default=0,
        help='Flip the multi-core preference every N callbacks, checking exact output across switches')
    ap.add_argument('--require-no-misses', action='store_true')
    ap.add_argument('--timeout', type=float, default=120)
    ap.add_argument('--reference', type=Path, help='Previous run directory; compare exact PCM, origin and MIDI')
    args = ap.parse_args()
    for label, path in [('binary', args.binary), ('NVRAM seed', args.nvram_seed)]:
        if not path.is_file():
            ap.error(f'{label} is not a file: {path}')
    if not args.rompath.is_dir():
        ap.error(f'ROM path is not a directory: {args.rompath}')
    if not 1 <= args.block <= 16384 or args.seconds <= 0 or args.speed <= 0 or args.timeout <= 0:
        ap.error('block must be in 1..16384, duration and speed must be positive')
    if not all(math.isfinite(v) for v in [args.seconds, args.speed, args.timeout]):
        ap.error('duration, speed and timeout must be finite')
    if args.multicore_toggle_blocks < 0:
        ap.error('--multicore-toggle-blocks must not be negative')
    if args.all_programs and args.seconds < 130:
        ap.error('--all-programs requires at least 130 seconds')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    for name in ['nvram/korgprop', 'cfg', 'snap']:
        (out / name).mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.nvram_seed, out / 'nvram/korgprop/sysram')
    binary = args.binary.resolve()
    binary_hash = sha(binary)
    command = [str(binary), 'korgprop', '-rompath', str(args.rompath.resolve()),
        '-nvram_directory', str(out / 'nvram'), '-cfg_directory', str(out / 'cfg'),
        '-snapshot_directory', str(out / 'snap'), '-video', 'none', '-sound', 'none',
        '-videodriver', 'dummy', '-nothrottle', '-skip_gameinfo']
    settings = dict(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy',
        PROPHOST_BLOCK=str(args.block), PROPHOST_PROBE_SECONDS=str(args.seconds),
        PROPHOST_PROBE_SPEED=str(args.speed), PROPHOST_PROBE_OUT=str(out / 'audio'),
        KPROP_DSP_PERFRAME='4' if args.engine == 'dynarec' else '0', KPROP_PF4_CMEM_DEOPT='1',
        PROPHOST_MULTICORE_DSP='1' if args.multicore else '0',
        PROPHOST_MULTICORE_DSP_TOGGLE_BLOCKS=str(args.multicore_toggle_blocks))
    for key, enabled in [('PROPHOST_PROBE_PACED', args.paced),
            ('PROPHOST_PROBE_DENSE', args.dense), ('PROPHOST_PROBE_PROGRAMS', args.programs),
            ('PROPHOST_PROBE_ALL_PROGRAMS', args.all_programs),
            ('KPROP_DSP_STATE_DIGEST', args.digest)]:
        if enabled:
            settings[key] = '1'
    env = {k:v for k,v in os.environ.items() if not k.startswith(
        ('KPROP_', 'PROPHOST_', 'PROPHECY_', 'PROFLIGACY_', 'SDL_'))}
    env.update(settings)
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter()
    with (out / 'run.log').open('w') as log:
        proc = subprocess.run(command, cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT,
                              timeout=max(args.timeout, args.seconds * 8))
    wall = time.perf_counter() - start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    result = dict(arguments={k:str(v) if isinstance(v, Path) else v for k,v in vars(args).items()},
        command=command, environment=settings, binary_sha256=binary_hash,
        instrumentation_environment={k:env[k] for k in ['TSAN_OPTIONS', 'ASAN_OPTIONS', 'UBSAN_OPTIONS'] if k in env},
        machine=dict(system=platform.platform(), architecture=platform.machine(), logical_cpus=os.cpu_count()),
        seed_sha256=sha(args.nvram_seed), returncode=proc.returncode, process_wall_seconds=wall,
        process_cpu_seconds=after.ru_utime + after.ru_stime - before.ru_utime - before.ru_stime,
        context_switches=after.ru_nvcsw + after.ru_nivcsw - before.ru_nvcsw - before.ru_nivcsw)
    text = (out / 'run.log').read_text(errors='replace')
    for line in text.splitlines():
        if line.startswith('[dsp-probe] '):
            result['metrics'] = json.loads(line[len('[dsp-probe] '):])
        elif line.startswith('[dsp-probe-scheduling] '):
            result['scheduling'] = {k:int(v) for k,v in
                (item.split('=', 1) for item in line.split()[1:])}
        elif line.startswith('[dsp-probe-multicore] '):
            result['multicore'] = {k:int(v) for k,v in
                (item.split('=', 1) for item in line.split()[1:])}
    result['state_digests'] = [line for line in text.splitlines() if line.startswith('[dsp-state] ')]
    result['memory_digests'] = [line for line in text.splitlines() if line.startswith('[dsp-memory] ')]
    pcm = out / 'audio.pcm'
    if pcm.exists():
        samples = array.array('h', pcm.read_bytes())
        result['pcm_sha256'] = sha(pcm)
        result['pcm_frames'] = len(samples) // 2
        result['nonzero_samples'] = sum(v != 0 for v in samples)
        result['peak'] = max(map(abs, samples), default=0)
        with pcm.open('rb') as source, gzip.open(str(pcm)+'.gz', 'wb', compresslevel=6) as dest:
            shutil.copyfileobj(source, dest)
        pcm.unlink()
    midi = out / 'audio.midi.csv'
    if midi.exists():
        result['midi_sha256'] = sha(midi)
    metrics = result.get('metrics', {})
    result['valid_capture'] = bool(metrics and 'pcm_sha256' in result and 'midi_sha256' in result
        and result['pcm_frames'] == metrics['frames'] and metrics['midi_dropped'] == 0
        and metrics['tx_dropped'] == 0 and sha(binary) == binary_hash)
    if args.all_programs:
        result['valid_capture'] &= metrics.get('programs_observed') == 128 and metrics.get('programs_match', False)
    result['deadlines_met'] = metrics.get('missing_frames', -1) == 0
    if args.reference:
        reference = json.loads((args.reference / 'receipt.json').read_text())
        result['equivalent'] = bool(reference.get('valid_capture') and reference.get('returncode') == 0)
        result['equivalent'] &= all(reference.get(k) == result.get(k) for k in
                                   ['pcm_sha256', 'pcm_frames', 'midi_sha256'])
        result['equivalent'] &= result.get('metrics', {}).get('origin') == reference.get('metrics', {}).get('origin')
        # Stop is asynchronous. Compare every digest through the shorter common
        # observation interval, and require enough checkpoints to be meaningful.
        if args.digest:
            a, b = reference.get('state_digests', []), result['state_digests']
            n = min(len(a), len(b))
            result['state_checkpoints_compared'] = n
            result['equivalent'] &= n >= 100 and a[:n] == b[:n]
            a, b = reference.get('memory_digests', []), result['memory_digests']
            n = min(len(a), len(b))
            result['memory_checkpoints_compared'] = n
            result['equivalent'] &= n >= 10 and a[:n] == b[:n]
    (out / 'receipt.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({'output': str(out), **result.get('metrics', {}),
        **{k:result[k] for k in ['pcm_sha256', 'midi_sha256', 'equivalent', 'state_checkpoints_compared', 'memory_checkpoints_compared'] if k in result}}), flush=True)
    raise SystemExit(proc.returncode or (8 if not result['valid_capture'] else 0)
        or (7 if args.reference and not result['equivalent'] else 0)
        or (9 if args.require_no_misses and not result['deadlines_met'] else 0))


if __name__ == '__main__':
    main()
