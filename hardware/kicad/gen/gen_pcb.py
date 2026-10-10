#!/usr/bin/env python3
"""Build ervlink.kicad_pcb from the schematic's netlist: place, route, pour.

Run with KiCad's bundled Python (it provides `pcbnew`):

    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/3.9/bin/python3 gen/gen_pcb.py

Steps: export the netlist with kicad-cli, place footprints from the PLACE table,
draw the outline and antenna keep-out, export Specctra DSN, autoroute with
Freerouting, import the session, add GND pours on both layers, fill, save.
"""
import math
import os
import subprocess
import sys

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sexp import find, find_all, parse  # noqa: E402

PROJ = os.path.normpath(os.path.join(HERE, ".."))
SCH = os.path.join(PROJ, "ervlink.kicad_sch")
PCB = os.path.join(PROJ, "ervlink.kicad_pcb")
BUILD = os.path.join(PROJ, "build")
KICAD_CLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
KICAD_FP = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
LOCAL_FP = {"Espressif": os.path.join(PROJ, "lib", "Espressif.pretty")}
FREEROUTING = os.path.expanduser("~/.local/share/freerouting/freerouting-2.4.1.jar")

W, H = 85.0, 60.0          # board size, mm
OX, OY = 100.0, 60.0       # board origin on the page

# ref: (x, y, rotation_deg) in board mm, y down. Rationale in DESIGN.md:
# antenna on the top edge, every cable entry on the bottom edge, relay and
# screw terminals far from the antenna.
PLACE = {
    # top edge: module antenna flush with the top edge, keep-out to its left
    "U1": (74.5, 16.0, 0),
    "C6": (62.8, 9.0, 180), "C5": (62.8, 11.2, 180),  # pad 1 right next to U1 pin 2 (+3V3)
    "SW1": (14.0, 5.0, 0),
    # relay and its driver, left
    "K1": (20.0, 12.0, 0),
    "D1": (22.54, 8.9, 0),        # bridges coil pins 1 and 12
    "R1": (13.5, 24.0, 0), "LED2": (13.5, 27.0, 0),
    "Q1": (14.0, 32.0, 0), "R2": (10.0, 34.2, 0), "R3": (10.0, 37.2, 0),
    "J3": (4.0, 14.0, 0),
    "R20": (9.0, 40.8, 0), "R21": (13.5, 40.8, 0),
    # comparator taps, centre
    "U2": (40.0, 16.0, 0), "C3": (45.2, 14.5, 90),
    "R4": (31.0, 8.0, 0), "R5": (31.0, 11.0, 0), "C1": (31.0, 14.0, 0), "R6": (31.0, 17.0, 0),
    "R7": (31.0, 20.0, 0), "R8": (31.0, 23.0, 0), "R9": (31.0, 26.0, 0),
    "R10": (49.0, 8.0, 0), "R11": (49.0, 11.0, 0), "C2": (49.0, 14.0, 0), "R12": (49.0, 17.0, 0),
    "R13": (49.0, 20.0, 0), "R14": (49.0, 23.0, 0), "R15": (49.0, 26.0, 0),
    # line drivers, centre
    "Q2": (34.0, 36.0, 0), "R16": (29.0, 34.0, 0), "R17": (29.0, 38.0, 0),
    "R18": (39.0, 34.0, 0), "R19": (39.0, 38.0, 0),
    "Q3": (48.0, 36.0, 0), "R22": (43.0, 34.0, 0), "R23": (43.0, 38.0, 0),
    "R24": (53.0, 34.0, 0), "R25": (53.0, 38.0, 0),
    # ESP support, left of the module
    "R26": (60.5, 14.0, 0), "C4": (60.5, 16.5, 0), "R27": (60.5, 19.0, 0),
    "TP1": (56.5, 14.0, 0), "TP2": (56.5, 17.5, 0),
    # power, bottom right
    "J4": (73.0, 56.6, 0),        # USB-C shell lip ~0.25 mm past the bottom edge
    "R28": (68.0, 47.5, 0), "R29": (78.0, 47.5, 0),
    "U3": (60.0, 44.0, 0), "C7": (55.0, 50.0, 90), "C8": (60.0, 50.0, 90), "C9": (64.0, 50.0, 90),
    "JP1": (23.5, 44.2, 0), "D6": (23.5, 41.0, 0),    # next to the BUS5V run
    "R30": (72.0, 36.0, 0), "LED1": (72.0, 39.0, 0),
    # bus terminals, bottom edge, wire entry facing out
    "J1": (6.0, 54.5, 0),
    "J2": (32.0, 54.5, 0),
    # mounting holes
    "H1": (3.5, 3.5, 0), "H2": (81.5, 30.5, 0), "H3": (3.5, 44.5, 0), "H4": (81.5, 43.0, 0),
}

# Assembly doesn't need designators; keep them where a person will look:
# ICs, connectors, and the resistors tuned at bring-up.
SHOW_PASSIVE_REF = {"R18", "R19", "R20", "R21", "R24", "R25"}
HIDE_REF = {"J1", "J2", "D1", "D6", "JP1", "TP1", "TP2", "LED1", "LED2"}
REF_AT = {"K1": (22.4, 18.2)}   # inside the relay outline

SILK = [  # (text, x, y, size_mm, justify)  JLCPCB minimum: 1.0 mm high, 0.15 mm stroke
    ("TO ERV", 13.6, 46.0, 1.2, "c"), ("TO WALL CONTROL", 39.6, 46.0, 1.2, "c"),
    ("ERV LINK rev A", 40.0, 29.0, 1.4, "c"),
    ("AUTO", 8.0, 5.0, 1.0, "c"), ("PASS", 25.0, 5.0, 1.0, "c"),
    ("EN", 54.4, 14.0, 1.0, "r"), ("BOOT", 54.4, 17.5, 1.0, "r"),
    ("JP1: NOT WITH USB", 25.6, 44.2, 1.0, "l"), ("PWR", 74.5, 39.0, 1.0, "l"),
    ("INT", 11.3, 27.0, 1.0, "r"),
] + [(t, x, 48.1, 1.2, "c") for x0 in (6.0, 32.0)
     for t, x in zip(("5V", "0V", "RXD", "TXD"), (x0 + 5.08 * i for i in range(4)))] \
  + [(t, 6.0, 14.0 + 2.54 * i, 1.0, "l")
     for i, t in enumerate(("GND", "3V3", "pRX", "eRX", "eTX", "pTX", "dRX", "dTX"))]


def mm(v):
    return pcbnew.FromMM(v)


def pt(x, y):
    return pcbnew.VECTOR2I(mm(OX + x), mm(OY + y))


def netlist():
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, "ervlink.net")
    subprocess.run([KICAD_CLI, "sch", "export", "netlist", "-o", out, SCH], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tree = parse(open(out, encoding="utf-8").read())
    comps = {}
    for c in find_all(find(tree, "components"), "comp"):
        ref = find(c, "ref")[1]
        fields = {find(f, "name")[1]: (f[2] if len(f) > 2 else "")
                  for f in find_all(find(c, "fields") or [], "field")}
        props = {find(p, "name")[1] for p in find_all(c, "property")}
        comps[ref] = dict(value=find(c, "value")[1], footprint=find(c, "footprint")[1],
                          fields={k: v for k, v in fields.items()
                                  if k not in ("Footprint", "Datasheet", "Description") and v},
                          dnp="dnp" in props)
    nets = {}
    for n in find_all(find(tree, "nets"), "net"):
        name = find(n, "name")[1]
        for node in find_all(n, "node"):
            nets[(find(node, "ref")[1], find(node, "pin")[1])] = name
    return comps, nets


def load_fp(fpid):
    lib, name = fpid.split(":")
    path = LOCAL_FP.get(lib, os.path.join(KICAD_FP, lib + ".pretty"))
    fp = pcbnew.FootprintLoad(path, name)
    if fp is None:
        raise SystemExit(f"footprint not found: {fpid}")
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


def line(board, layer, a, b, width=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(pt(*a))
    s.SetEnd(pt(*b))
    s.SetLayer(layer)
    s.SetWidth(mm(width))
    board.Add(s)


def zone(board, layer, corners, net=None, keepout=False, name="", tracks_only=False):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    if keepout:
        z.SetIsRuleArea(True)
        z.SetDoNotAllowTracks(True)
        z.SetDoNotAllowVias(not tracks_only)
        z.SetDoNotAllowZoneFills(not tracks_only)
        z.SetDoNotAllowPads(False)
        z.SetDoNotAllowFootprints(False)
        z.SetLayerSet(pcbnew.LSET.AllCuMask())
    else:
        z.SetNet(net)
        z.SetLocalClearance(mm(0.3))
        z.SetMinThickness(mm(0.25))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        z.SetThermalReliefGap(mm(0.3))
        z.SetThermalReliefSpokeWidth(mm(0.4))
    if name:
        z.SetZoneName(name)
    poly = z.Outline()
    poly.NewOutline()
    for x, y in corners:
        poly.Append(mm(OX + x), mm(OY + y))
    board.Add(z)
    return z


def build_board():
    comps, pinnets = netlist()
    board = pcbnew.BOARD()
    board.SetCopperLayerCount(2)
    ds = board.GetDesignSettings()
    ds.m_MinClearance = mm(0.15)
    ds.m_TrackMinWidth = mm(0.15)
    ds.m_ViasMinSize = mm(0.6)
    ds.m_MinThroughDrill = mm(0.3)
    ds.m_CopperEdgeClearance = mm(0.3)
    nc = ds.m_NetSettings.GetDefaultNetclass()
    nc.SetClearance(mm(0.15))
    nc.SetTrackWidth(mm(0.25))
    nc.SetViaDiameter(mm(0.6))
    nc.SetViaDrill(mm(0.3))
    # Wide copper for anything carrying supply current. BUS5V feeds the wall
    # control over the thermostat cable; its draw hasn't been measured, so size
    # it like the cable rather than the load.
    ns = ds.m_NetSettings
    for cls, width, nets in (("Bus", 1.0, ["/BUS5V"]), ("Power", 0.4, ["/+5V", "/+3V3"])):
        pc = pcbnew.NETCLASS(cls)
        pc.SetClearance(mm(0.2))
        pc.SetTrackWidth(mm(width))
        pc.SetViaDiameter(mm(0.8))
        pc.SetViaDrill(mm(0.4))
        ns.SetNetclass(cls, pc)
        for n in nets:
            ns.SetNetclassPatternAssignment(n, cls)

    netinfo = {}
    for name in sorted(set(pinnets.values())):
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
        netinfo[name] = ni

    missing = [r for r in comps if r not in PLACE]
    if missing:
        raise SystemExit(f"no placement for: {missing}")
    for ref, c in sorted(comps.items()):
        fp = load_fp(c["footprint"])
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        for k, v in c["fields"].items():
            fp.SetField(k, v)
            f = fp.GetField(k)
            f.SetVisible(False)
            f.SetLayer(pcbnew.F_Fab)
        if c["dnp"]:
            fp.SetDNP(True)
        x, y, rot = PLACE[ref]
        fp.SetPosition(pt(x, y))
        fp.SetOrientationDegrees(rot)
        for pad in fp.Pads():
            net = pinnets.get((ref, pad.GetNumber()))
            if net:
                pad.SetNet(netinfo[net])
        ref_text = fp.Reference()
        ref_text.SetTextSize(pcbnew.VECTOR2I(mm(1.0), mm(1.0)))
        ref_text.SetTextThickness(mm(0.15))
        passive = ref[0] in "RC" and ref[1:].isdigit()
        if ref in HIDE_REF or (passive and ref not in SHOW_PASSIVE_REF):
            ref_text.SetVisible(False)
        if ref in REF_AT:
            ref_text.SetPosition(pt(*REF_AT[ref]))
        board.Add(fp)

    # outline
    corners = [(0, 0), (W, 0), (W, H), (0, H)]
    for a, b in zip(corners, corners[1:] + corners[:1]):
        line(board, pcbnew.Edge_Cuts, a, b, 0.1)

    # Antenna keep-out, all layers. Espressif (ESP32-C6 PCB layout guidelines):
    # >=15 mm clear beside the antenna, module top at the board edge.
    ux, uy, _ = PLACE["U1"]
    ant_bottom = uy - 9.0                 # antenna region ends ~9.75 mm above the origin
    zone(board, pcbnew.F_Cu, [(ux - 9.0 - 15.0, 0), (W, 0), (W, ant_bottom), (ux - 9.0 - 15.0, ant_bottom)],
         keepout=True, name="ANTENNA_KEEPOUT")
    # No tracks under the module body (pour and vias allowed), so signals escape outward
    zone(board, pcbnew.F_Cu, [(ux - 7.5, ant_bottom), (ux + 7.5, ant_bottom), (ux + 7.5, uy + 8.0),
                              (ux - 7.5, uy + 8.0)], keepout=True, tracks_only=True, name="MODULE_NO_TRACKS")

    # Decoupling: C6 then C5 straight onto U1 pin 2, locked so the router keeps it
    fps = {f.GetReference(): f for f in board.GetFootprints()}
    pad = lambda ref, num: [p for p in fps[ref].Pads() if p.GetNumber() == num][0]
    for a, b in ((pad("U1", "2"), pad("C6", "1")), (pad("C6", "1"), pad("C5", "1"))):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(a.GetPosition())
        t.SetEnd(b.GetPosition())
        t.SetWidth(mm(0.4))
        t.SetLayer(pcbnew.F_Cu)
        t.SetNet(a.GetNet())
        t.SetLocked(True)
        board.Add(t)
    # EPAD: solid connection plus vias in the gaps of its 3x3 pad grid
    epad = [p for p in fps["U1"].Pads() if p.GetNumber() == "29"]
    for p in epad:
        p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)
    for f in fps.values():                         # solid GND pad connections (machine reflow;
        for p in f.Pads():                         # spokes don't fit between dense tracks)
            if p.GetNetname() == "/GND" and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD:
                p.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)

    for text, x, y, size, just in SILK:
        t = pcbnew.PCB_TEXT(board)
        t.SetText(text)
        t.SetPosition(pt(x, y))
        t.SetLayer(pcbnew.F_SilkS)
        t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
        t.SetTextThickness(mm(max(0.15, size * 0.15)))
        t.SetHorizJustify({"l": pcbnew.GR_TEXT_H_ALIGN_LEFT, "r": pcbnew.GR_TEXT_H_ALIGN_RIGHT,
                           "c": pcbnew.GR_TEXT_H_ALIGN_CENTER}[just])
        board.Add(t)
    return board, netinfo


def add_via(board, pos, net, size=0.6, drill=0.3):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pos)
    v.SetWidth(mm(size))
    v.SetDrill(mm(drill))
    v.SetNet(net)
    board.Add(v)
    return v


def strip_net(dsn, net):
    """Remove one net from a Specctra DSN so the autorouter leaves it alone."""
    key = f"(net {net}\n"
    i = dsn.find(key)
    if i < 0:
        raise SystemExit(f"net {net} not found in DSN")
    depth, j = 0, i
    while True:
        if dsn[j] == "(":
            depth += 1
        elif dsn[j] == ")":
            depth -= 1
            if depth == 0:
                break
        j += 1
    dsn = dsn[:i] + dsn[j + 1:]
    return dsn.replace(f" {net} ", " ").replace(f" {net}\n", "\n")


def autoroute(board):
    """Route everything except GND, which the pours and stitching vias carry."""
    dsn = os.path.join(BUILD, "ervlink.dsn")
    ses = os.path.join(BUILD, "ervlink.ses")
    if not pcbnew.ExportSpecctraDSN(board, dsn):
        raise SystemExit("DSN export failed")
    with open(dsn) as f:
        text = strip_net(f.read(), "/GND")
    with open(dsn, "w") as f:
        f.write(text)
    if os.path.exists(ses):
        os.remove(ses)
    cmd = ["mise", "exec", "--", "java", "-jar", FREEROUTING, "-de", dsn, "-do", ses,
           "-mp", "60", "--gui.enabled=false"]
    print("routing:", " ".join(cmd[3:]))
    subprocess.run(cmd, check=True, cwd=PROJ)
    if not os.path.exists(ses) or not pcbnew.ImportSpecctraSES(board, ses):
        raise SystemExit("SES import failed")


def seg_dist(p, a, b):
    ax, ay, bx, by, px, py = a.x, a.y, b.x, b.y, p.x, p.y
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def clear_of_copper(board, pos, gnd, radius, gap=0.25):
    """True if a via of `radius` at pos keeps `gap` from all non-GND copper."""
    need = mm(radius + gap)
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNetCode() == gnd.GetNetCode() and p.GetNetCode() != 0:
                continue
            bb = p.GetBoundingBox()
            dx = max(bb.GetLeft() - pos.x, 0, pos.x - bb.GetRight())
            dy = max(bb.GetTop() - pos.y, 0, pos.y - bb.GetBottom())
            if math.hypot(dx, dy) < need:
                return False
    for t in board.GetTracks():
        if t.GetNetCode() == gnd.GetNetCode():
            if t.GetClass() == "PCB_VIA" and (t.GetPosition() - pos).EuclideanNorm() < mm(0.9):
                return False
            continue
        half = (t.GetWidth(pcbnew.F_Cu) if t.GetClass() == "PCB_VIA" else t.GetWidth()) / 2
        if t.GetClass() == "PCB_VIA":
            if (t.GetPosition() - pos).EuclideanNorm() < need + half:
                return False
        elif seg_dist(pos, t.GetStart(), t.GetEnd()) < need + half:
            return False
    return True


def in_rect(pos, r):
    x, y = pcbnew.ToMM(pos.x) - OX, pcbnew.ToMM(pos.y) - OY
    return r[0] <= x <= r[2] and r[1] <= y <= r[3]


def no_via_zones():
    ux, uy, _ = PLACE["U1"]
    keep = [(ux - 24.5, 0, W, uy - 8.4)]                       # antenna keep-out (+margin)
    for ref in ("J1", "J2", "J4", "K1", "SW1", "J3"):          # THT bodies and the USB shell
        x, y, _ = PLACE[ref]
        keep.append({"J1": (x - 3.5, y - 5.6, x + 18.7, H), "J2": (x - 3.5, y - 5.6, x + 18.7, H),
                     "J4": (x - 5.5, y - 5.6, x + 5.5, H), "K1": (x - 2.0, y - 2.0, x + 7.0, y + 15.0),
                     "SW1": (x - 4.0, y - 3.2, x + 8.0, y + 2.8), "J3": (x - 1.6, y - 1.6, x + 1.6, y + 19.4)}[ref])
    return keep


def pour_and_stitch(board, netinfo):
    gnd = netinfo["/GND"]
    outline = [(0.3, 0.3), (W - 0.3, 0.3), (W - 0.3, H - 0.3), (0.3, H - 0.3)]
    zones = [zone(board, layer, outline, net=gnd, name="GND") for layer in (pcbnew.F_Cu, pcbnew.B_Cu)]
    # EPAD vias in the gaps of the module's 3x3 ground-pad grid (after routing:
    # the session import replaces all vias)
    u1 = [f for f in board.GetFootprints() if f.GetReference() == "U1"][0]
    epad = [p for p in u1.Pads() if p.GetNumber() == "29"]
    xs = sorted({p.GetPosition().x for p in epad})
    ys = sorted({p.GetPosition().y for p in epad})
    for xa, xb in zip(xs, xs[1:]):
        for ya, yb in zip(ys, ys[1:]):
            add_via(board, pcbnew.VECTOR2I((xa + xb) // 2, (ya + yb) // 2), gnd)
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    keep = no_via_zones()

    # 1. Fan out every SMD GND pad to a via (and a short track), trying 8 directions
    fanned = 0
    for fp in board.GetFootprints():
        for p in fp.Pads():
            if p.GetNetCode() != gnd.GetNetCode() or p.GetAttribute() != pcbnew.PAD_ATTRIB_SMD:
                continue
            if fp.GetReference() in ("J4", "U1"):          # U1 GND pads sit in the pour
                continue
            c = p.GetPosition()
            r = max(p.GetSize().x, p.GetSize().y) / 2 + mm(0.65)
            ok = False
            for k in range(16):
                a = math.radians(k * 22.5)
                pos = pcbnew.VECTOR2I(int(c.x + r * math.cos(a)), int(c.y + r * math.sin(a)))
                if (any(in_rect(pos, z) for z in keep) or not in_rect(pos, (1.2, 1.2, W - 1.2, H - 1.2))
                        or not clear_of_copper(board, pos, gnd, 0.3)):
                    continue
                t = pcbnew.PCB_TRACK(board)
                t.SetStart(c)
                t.SetEnd(pos)
                t.SetWidth(mm(0.4))
                t.SetLayer(pcbnew.F_Cu)
                t.SetNet(gnd)
                if not clear_of_track_path(board, t, gnd):
                    continue
                board.Add(t)
                add_via(board, pos, gnd)
                ok = fanned = fanned + 1
                break
            if not ok:
                print(f"  no fan-out room for {fp.GetReference()}.{p.GetNumber()}")

    # 2. Stitching grid (4 mm) wherever both layers are GND-filled, plus a fence
    #    along the antenna keep-out's lower edge
    filler.Fill(board.Zones())
    ux, uy, _ = PLACE["U1"]
    pts = [(x, y) for x in frange(2.0, W - 2.0, 4.0) for y in frange(2.0, H - 2.0, 4.0)]
    pts += [(x, uy - 8.1) for x in frange(ux - 24.0, ux - 9.5, 1.5)]
    stitched = 0
    for x, y in pts:
        pos = pt(x, y)
        if any(in_rect(pos, z) for z in keep):
            continue
        if not all(z.HitTestFilledArea(z.GetLayer(), pos, 0) for z in zones):
            continue
        if not clear_of_copper(board, pos, gnd, 0.3, gap=0.35):
            continue
        add_via(board, pos, gnd)
        stitched += 1
    filler.Fill(board.Zones())

    # 3. Any pour island with no via or through-hole GND pad gets a via inside it
    rescued = 0
    for z in zones:
        polys = z.GetFilledPolysList(z.GetLayer())
        for i in range(polys.OutlineCount()):
            anchored = any(polys.Contains(t.GetPosition(), i) for t in board.GetTracks()
                           if t.GetClass() == "PCB_VIA" and t.GetNetCode() == gnd.GetNetCode())
            anchored = anchored or any(
                polys.Contains(p.GetPosition(), i) for f in board.GetFootprints() for p in f.Pads()
                if p.GetNetCode() == gnd.GetNetCode() and p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH)
            if anchored:
                continue
            bb = polys.Outline(i).BBox()
            step = mm(0.25)
            done = False
            for yy in range(bb.GetTop(), bb.GetBottom(), step):
                for xx in range(bb.GetLeft(), bb.GetRight(), step):
                    pos = pcbnew.VECTOR2I(xx, yy)
                    if (polys.Contains(pos, i) and all(o.HitTestFilledArea(o.GetLayer(), pos, 0) for o in zones)
                            and clear_of_copper(board, pos, gnd, 0.3, gap=0.25)):
                        add_via(board, pos, gnd)
                        rescued += 1
                        done = True
                        break
                if done:
                    break
            if not done:
                print(f"  island on {z.GetLayerName()} near "
                      f"({pcbnew.ToMM(bb.GetCenter().x) - OX:.1f}, {pcbnew.ToMM(bb.GetCenter().y) - OY:.1f}) has no via")
    filler.Fill(board.Zones())
    print(f"GND: {fanned} fan-out vias, {stitched} stitching vias, {rescued} island vias")


def clear_of_track_path(board, track, gnd, gap=0.25):
    """True if a candidate fan-out track keeps `gap` from other-net pads and tracks."""
    a, b = track.GetStart(), track.GetEnd()
    half = track.GetWidth() / 2
    for i in range(1, 8):
        p = pcbnew.VECTOR2I(a.x + (b.x - a.x) * i // 8, a.y + (b.y - a.y) * i // 8)
        if not clear_of_copper(board, p, gnd, pcbnew.ToMM(half), gap):
            return False
    return True


def frange(a, b, step):
    v = a
    while v <= b + 1e-9:
        yield round(v, 3)
        v += step


def main():
    board, netinfo = build_board()
    if "--no-route" not in sys.argv:
        autoroute(board)
        pour_and_stitch(board, netinfo)
    board.Save(PCB)
    print("wrote", os.path.relpath(PCB))


if __name__ == "__main__":
    main()
