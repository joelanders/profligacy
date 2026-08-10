#!/usr/bin/env python3
"""Generate the FIXED-ROUTES table for the editor from the sysex param manifest.

The Prophecy's hard-wired modulation: params whose source is fixed by the hardware
(velocity, aftertouch, CC#1, keyboard tracking) rather than chosen from a source
enum. Replicates the sokol GUI's isFixedModulationAmountParam / fixedRouteSourceName
predicates (korgprophecy_gui.mm:3602/3617). 78 routes, 72 writable.

Unlike the sokol routing list (read-only), the editor makes the writable ones
draggable — they are ordinary verified-transport 0x41 params.

  python3 scripts/gen_fixed_routes.py > table.json
"""
import csv, json, sys, pathlib
M = pathlib.Path('extern/mame/docs/korg/korgprophecy_sysex_param_manifest.tsv')
rows = list(csv.DictReader(open(M), delimiter='\t'))
def is_fixed(r):
    n = r['name']
    if r['kind'] != 'program' or r['value_type'] == 'reserved': return False
    if 'AT Control' in n or 'CC#1 Control' in n or 'Velocity Control' in n: return True
    if ('KBD TRK' in n or 'Keyboard Track' in n) and 'Low Key' not in n and 'High Key' not in n: return True
    return False
def src(n):
    if 'AT Control' in n: return 'After Touch'
    if 'CC#1 Control' in n: return 'CC#1'
    if 'Velocity Control' in n: return 'Velocity'
    if 'KBD TRK' in n or 'Keyboard Track' in n: return 'Note No.'
    return 'Fixed'
def foff(s):
    for t in s.replace(',', ' ').split():
        try: return int(t)
        except ValueError: pass
    return -1
out = []
for r in rows:
    if not is_fixed(r): continue
    lo = int(r['min']) if r['min'].strip() else 0
    hi = int(r['max']) if r['max'].strip() else 127
    out.append({'name': r['name'], 'sub': r['subsection'], 'sec': r['section'],
                'src': src(r['name']), 'p': int(r['param']), 'off': foff(r['verified_offsets']),
                'min': lo, 'max': hi,
                'w': r['status'] in ('verified_transport','reserved_verified_transport')})
json.dump(out, sys.stdout, separators=(',',':'))
print(f"\n// {len(out)} fixed routes, {sum(1 for x in out if x['w'])} writable", file=sys.stderr)
