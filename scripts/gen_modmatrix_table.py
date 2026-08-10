#!/usr/bin/env python3
"""Generate the MOD MATRIX route table for the editor from the sysex param manifest.

Mirrors the editor's manifest-driven route discovery: typed modulation selectors
plus the legacy dedicated EG/LFO conventions, followed by adjacent amount pairing.
It deliberately fixes three limitations in the old sokol discovery: Korg's two
"Mod.Srource" typos remain discoverable, exceptional selectors such as Wah Sweep
are included, and full destination names prevent unrelated routes from colliding.

  python3 scripts/gen_modmatrix_table.py [manifest.tsv] > table.json
"""
import csv, json, sys, pathlib

MANIFEST = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
    pathlib.Path(__file__).resolve().parents[1] / "extern/mame/docs/korg/korgprophecy_sysex_param_manifest.tsv")

# modulationDestinationName's verbatim strip list, in SOKOL'S ORDER — the first matching
# suffix in list order strips per pass (so " Mod.LFO" beats " Pitch Mod.LFO" on
# "OSC1 Pitch Mod.LFO", leaving "OSC1 Pitch", exactly like the GUI).
STRIP_SUFFIXES = [
    " Smooth Bending Controller", " Mod.Source", " Mod.Intensity", " Mod.Int.",
    " Mod.EG", " Mod.EG Intensity", " Mod.EG Int.",
    " Mod.LFO", " Mod.LFO Intensity", " Mod.LFO Int.",
    " Pitch Mod.LFO", " Pitch Mod.LFO Intensity",
    " Pitch Mod.LFO Int.AT Control", " Pitch Mod.LFO Int.CC#1 Control",
    " Pressure EG", " Pressure EG Intensity", " Pressure EG Int.Mod.Source",
    " Pressure EG Int.Mod.Intensity", " Pressure LFO", " Pressure LFO Intensity",
    " Pressure Mod.Source", " Pressure Mod.Intensity", " LFO Select",
    " Depth Mod.Source", " Depth Mod.Intensity",
    " Effect Balance Mod.Source", " Effect Balance Mod.Intensity",
    " Panpot Mod.Source", " Panpot Mod.Intensity",
    " Velocity Control", " AT Control", " CC#1 Control",
]

def is_general(r):
    # value_type is the authoritative signal.  In particular, the Prophecy PDF/manifest
    # spell OSC1/2 Pitch Mod."Srource" and Wah uses the exceptional name "Sweep Source".
    # Name matching is retained for the old smooth-bending rows whose type predates the
    # manifest classification.
    n = r["name"]
    return r["value_type"] == "modsource" or n.endswith(" Smooth Bending Controller")
def is_eg(n):      return n.endswith(" Mod.EG") or n.endswith(" Pressure EG")
def is_lfo(n):     return n.endswith(" Mod.LFO") or n.endswith(" Pitch Mod.LFO") or n.endswith(" Pressure LFO") or n.endswith(" LFO Select")
def is_amount(n):  return ("Intensity" in n) or ("Int." in n) or ("Depth" in n)

def strip_suffixes(name):
    name = name.replace(" Mod.Srource", " Mod.Source")
    if name.endswith(" Sweep Source"):
        return name[: -len(" Source")].strip()
    if name.endswith(" Smooth Bending Controller"):
        return name[: -len(" Controller")].strip()
    if name.endswith(" Pressure EG Intensity"):
        return name[: -len(" EG Intensity")].strip()
    if name.endswith(" Pressure LFO Intensity"):
        return name[: -len(" LFO Intensity")].strip()
    if name.endswith(" Pressure EG"):
        return name[: -len(" EG")].strip()
    if name.endswith(" Pressure LFO"):
        return name[: -len(" LFO")].strip()
    if name.endswith(" Int.Mod.Source"):
        return (name[: -len(" Int.Mod.Source")] + " Intensity").strip()
    if name.endswith(" Int.Mod.Intensity"):
        return (name[: -len(" Int.Mod.Intensity")] + " Intensity").strip()
    changed = True
    while changed:
        changed = False
        for s in STRIP_SUFFIXES:            # sokol list order, first match strips
            if name.endswith(s):
                name = name[: -len(s)]
                changed = True
                break
    return name.strip()

def sokol_dest(row):  # full modulationDestinationName incl. subsection strip (for pairing only)
    n = strip_suffixes(row["name"])
    sub = row["subsection"].strip()
    if sub and n.startswith(sub + " "):
        n = n[len(sub) + 1:]
    return n.strip()

def first_off(s):
    for tok in s.replace(",", " ").split():
        try: return int(tok)
        except ValueError: pass
    return -1

def main():
    rows = list(csv.DictReader(open(MANIFEST), delimiter="\t"))
    out = []
    for i, r in enumerate(rows):
        n = r["name"]
        if r["kind"] != "program" or not (is_general(r) or is_eg(n) or is_lfo(n)):
            continue
        kind = "gen" if is_general(r) else ("eg" if is_eg(n) else "lfo")
        # findLikelyAmountParam: next 4 rows, same kind/group/section/subsection,
        # amount-looking name, matching destination
        amt = None
        for j in range(i + 1, min(i + 5, len(rows))):
            c = rows[j]
            if c["kind"] != r["kind"] or c["group"] != r["group"]: continue
            if c["section"] != r["section"] or c["subsection"] != r["subsection"]: continue
            if not is_amount(c["name"]): continue
            if sokol_dest(c) == sokol_dest(r) or sokol_dest(r) in c["name"]:
                amt = c; break
        writable = lambda x: x["status"] in ("verified_transport", "reserved_verified_transport") and x["value_type"] != "reserved"
        def rng(x):
            lo = int(x["min"]) if x["min"].strip() else 0
            hi = int(x["max"]) if x["max"].strip() else 127
            return lo, hi
        amt_lo, amt_hi = rng(amt) if amt else (0, 0)
        out.append({
            "id":    f"p{r['param']}",
            "label": strip_suffixes(n),                # improvement: unambiguous row label
            "sec":   r["section"],
            "sub":   r["subsection"],
            "kind":  kind,
            "ampOrder": r["section"] == "Amplitude" and "Amplitude Mod.EG" in n,
            "sel": {"p": int(r["param"]), "off": first_off(r["verified_offsets"])},
            "amt": ({"p": int(amt["param"]), "off": first_off(amt["verified_offsets"]),
                     "min": amt_lo, "max": amt_hi} if amt else None),
            "w": bool(writable(r) and (amt is None or writable(amt))),  # editable + decodable
        })
    json.dump(out, sys.stdout, separators=(",", ":"))
    print(f"\n// {len(out)} routes", file=sys.stderr)

if __name__ == "__main__":
    main()
