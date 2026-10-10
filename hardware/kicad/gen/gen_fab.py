#!/usr/bin/env python3
"""Write the JLCPCB order files into ../fab/: gerber zip, BOM and CPL.

Only parts with an LCSC field are assembled by JLCPCB; through-hole parts
(relay, terminals, switch, header) have none and are fitted by the owner.
DNP parts are left out.
"""
import csv
import os
import subprocess
import zipfile
from collections import defaultdict

from sexp import find, find_all, parse

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.normpath(os.path.join(HERE, ".."))
FAB = os.path.join(PROJ, "fab")
BUILD = os.path.join(PROJ, "build")
CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
PCB = os.path.join(PROJ, "ervlink.kicad_pcb")
# JLCPCB places on the body centre. The WROOM footprint origin is the pad-grid
# centre; the body centre is 3 mm toward the antenna (CPL Y is page Y negated).
CPL_OFFSET = {"U1": (0.0, 3.0)}


def run(*args):
    subprocess.run([CLI, *args], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    gerb = os.path.join(FAB, "gerbers")
    os.makedirs(gerb, exist_ok=True)
    os.makedirs(BUILD, exist_ok=True)
    run("pcb", "export", "gerbers", "--layers",
        "F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts",
        "--no-protel-ext", "-o", gerb + "/", PCB)
    run("pcb", "export", "drill", "--format", "excellon", "--excellon-separate-th", "-o", gerb + "/", PCB)
    with zipfile.ZipFile(os.path.join(FAB, "ervlink-gerbers.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(gerb)):
            z.write(os.path.join(gerb, f), f)

    net = os.path.join(BUILD, "ervlink.net")
    run("sch", "export", "netlist", "-o", net, os.path.join(PROJ, "ervlink.kicad_sch"))
    tree = parse(open(net, encoding="utf-8").read())
    parts = {}
    for c in find_all(find(tree, "components"), "comp"):
        fields = {find(f, "name")[1]: (f[2] if len(f) > 2 else "")
                  for f in find_all(find(c, "fields") or [], "field")}
        dnp = any(find(p, "name")[1] == "dnp" for p in find_all(c, "property"))
        if fields.get("LCSC") and not dnp:
            parts[find(c, "ref")[1]] = (find(c, "value")[1], find(c, "footprint")[1].split(":")[1],
                                       fields["LCSC"])

    pos = os.path.join(BUILD, "pos.csv")
    run("pcb", "export", "pos", "--format", "csv", "--units", "mm", "--side", "front",
        "--exclude-dnp", "-o", pos, PCB)
    with open(os.path.join(FAB, "ervlink-cpl.csv"), "w", newline="") as out:
        w = csv.writer(out)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        for row in csv.DictReader(open(pos)):
            if row["Ref"] in parts:
                dx, dy = CPL_OFFSET.get(row["Ref"], (0.0, 0.0))
                w.writerow([row["Ref"], f"{float(row['PosX']) + dx:.4f}mm", f"{float(row['PosY']) + dy:.4f}mm", "Top",
                            str(int(round(float(row["Rot"])))) ])

    groups = defaultdict(list)
    for ref, key in parts.items():
        groups[key].append(ref)
    with open(os.path.join(FAB, "ervlink-bom.csv"), "w", newline="") as out:
        w = csv.writer(out)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
        for (value, fp, lcsc), refs in sorted(groups.items(), key=lambda kv: kv[0][2]):
            refs.sort(key=lambda r: (r.rstrip("0123456789"), int(r[len(r.rstrip("0123456789")):] or 0)))
            w.writerow([value, ",".join(refs), fp, lcsc])
    print(f"fab/: gerber zip, BOM ({len(groups)} lines, {len(parts)} placements), CPL")


if __name__ == "__main__":
    main()
