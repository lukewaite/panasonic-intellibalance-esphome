#!/usr/bin/env python3
"""Generate ervlink.kicad_sch from the netlist below.

Every pin gets a short wire stub and a net label, so connectivity is defined
entirely by net names. Edit the PARTS table, re-run, then check with
`kicad-cli sch erc`. See ../../DESIGN.md for the reasoning behind each value.
"""
import math
import os
import uuid

from libs import flat_symbol, pins
from sexp import Sym, dump

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "ervlink.kicad_sch")
PROJECT = "ervlink"
NS = uuid.UUID("6f1d2c4e-8a8b-4c1e-9a55-2b8d6c1e0e01")
ROOT = str(uuid.uuid5(NS, "root"))

R0805 = "Resistor_SMD:R_0805_2012Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
LED0805 = "LED_SMD:LED_0805_2012Metric"

# JLCPCB basic parts (checked 2026-10-10) unless noted.
LCSC_R = {"100": "C17408", "1k": "C17513", "2.2k": "C17520", "5.1k": "C27834", "10k": "C17414",
          "12k": "C17444", "22k": "C17560", "51k": "C17737", "100k": "C149504", "220k": "C17556"}
LCSC_C = {"100p": "C1790", "100n": "C49678", "1u": "C28323", "10u": "C15850", "22u": "C45783"}

PARTS = []


def part(ref, lib, value, footprint, at, nets, rot=0, unit=1, lcsc="", dnp=False, note=""):
    PARTS.append(dict(ref=ref, lib=lib, value=value, footprint=footprint, at=at, nets=nets,
                      rot=rot, unit=unit, lcsc=lcsc, dnp=dnp, note=note))


def R(ref, value, a, b, at, dnp=False, note=""):
    """Horizontal resistor: pin 1 (left) on net a, pin 2 (right) on net b."""
    part(ref, "Device:R", value, R0805, at, {"1": a, "2": b}, rot=90,
         lcsc="" if dnp else LCSC_R[value], dnp=dnp, note=note)


def C(ref, value, a, b, at):
    part(ref, "Device:C", value.replace("u", "µ") + "F" if value[-1] in "unp" else value,
         C0805, at, {"1": a, "2": b}, rot=90, lcsc=LCSC_C[value])


# ---------------------------------------------------------------- bus connectors
part("J1", "Connector:Screw_Terminal_01x04", "TO ERV",
     "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-4-5.08_1x04_P5.08mm_Horizontal",
     (30, 45), {"1": "BUS5V", "2": "GND", "3": "ERV_RXD", "4": "ERV_TXD"}, note="owner-fitted THT")
part("J2", "Connector:Screw_Terminal_01x04", "TO WALL CONTROL",
     "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-4-5.08_1x04_P5.08mm_Horizontal",
     (30, 75), {"1": "BUS5V", "2": "GND", "3": "PANEL_RXD", "4": "PANEL_TXD"}, note="owner-fitted THT")
part("J3", "Connector_Generic:Conn_01x08", "PROBE",
     "Connector_PinHeader_2.54mm:PinHeader_1x08_P2.54mm_Vertical",
     (30, 110), {"1": "GND", "2": "+3V3", "3": "PANEL_RXD", "4": "ERV_RXD", "5": "ERV_TXD",
                 "6": "PANEL_TXD", "7": "DRV_RXD", "8": "DRV_TXD"}, note="owner-fitted THT")
R("R20", "100k", "PANEL_RXD", "GND", (30, 135), dnp=True, note="Rdl1 dummy load, fit only if needed")
R("R21", "100k", "ERV_TXD", "GND", (30, 145), dnp=True, note="Rdl2 dummy load, fit only if needed")

# ---------------------------------------------------------------- relay
# G6S-2: coil 1/12; pole A COM 4, NC 3, NO 5; pole B COM 9, NC 10, NO 8.
part("K1", "Relay:G6S-2", "G6S-2 DC5", "Relay_THT:Relay_DPDT_Omron_G6S-2", (95, 55),
     {"1": "COIL_HI", "12": "COIL_LO",
      "4": "ERV_RXD", "3": "PANEL_RXD", "5": "DRV_RXD",
      "9": "PANEL_TXD", "10": "ERV_TXD", "8": "DRV_TXD"}, note="owner-fitted THT")
part("SW1", "Switch:SW_SPDT", "FORCE PASSTHROUGH",
     "Button_Switch_THT:SW_Slide_SPDT_Straight_CK_OS102011MS2Q", (95, 90),
     {"1": "+5V", "2": "COIL_HI", "3": None}, note="owner-fitted THT; C position = passthrough")
part("D1", "Diode:1N4148W", "1N4148W", "Diode_SMD:D_SOD-123", (95, 105),
     {"1": "COIL_HI", "2": "COIL_LO"}, lcsc="C81598", note="place directly across K1 coil")
R("R1", "1k", "COIL_HI", "LED2_A", (88, 118))
part("LED2", "Device:LED", "INTERCEPT", LED0805, (122, 118), {"2": "LED2_A", "1": "COIL_LO"},
     rot=180, lcsc="C2296")
part("Q1", "Transistor_FET:AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23", (110, 140),
     {"1": "Q1_G", "2": "GND", "3": "COIL_LO"}, lcsc="C20917")
R("R2", "100", "RELAY_EN", "Q1_G", (80, 140))
R("R3", "100k", "Q1_G", "GND", (80, 150))

# ---------------------------------------------------------------- receive taps (LM393)
# Tap A: wall control RXD (commands) -> RX_PANEL
part("U2", "Comparator:LM393", "LM393DR2G", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", (185, 50),
     {"2": "TAPA_IN", "3": "TAPA_REF", "1": "RX_PANEL"}, unit=1, lcsc="C7955")
R("R4", "100k", "PANEL_RXD", "TAPA_IN", (150, 35))
R("R5", "100k", "TAPA_IN", "GND", (150, 45))
C("C1", "100p", "TAPA_IN", "GND", (150, 55))
R("R6", "22k", "+3V3", "TAPA_REF", (150, 65))
R("R7", "12k", "TAPA_REF", "GND", (150, 75))
R("R8", "220k", "RX_PANEL", "TAPA_REF", (215, 40))
R("R9", "10k", "+3V3", "RX_PANEL", (215, 60))
# Tap B: ERV TXD (state) -> RX_ERV
part("U2", "Comparator:LM393", "LM393DR2G", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", (185, 110),
     {"6": "TAPB_IN", "5": "TAPB_REF", "7": "RX_ERV"}, unit=2, lcsc="C7955")
R("R10", "100k", "ERV_TXD", "TAPB_IN", (150, 95))
R("R11", "100k", "TAPB_IN", "GND", (150, 105))
C("C2", "100p", "TAPB_IN", "GND", (150, 115))
R("R12", "51k", "+3V3", "TAPB_REF", (150, 125))
R("R13", "10k", "TAPB_REF", "GND", (150, 135))
R("R14", "220k", "RX_ERV", "TAPB_REF", (215, 100))
R("R15", "10k", "+3V3", "RX_ERV", (215, 120))
# Comparator power
part("U2", "Comparator:LM393", "LM393DR2G", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", (185, 160),
     {"8": "+5V", "4": "GND"}, unit=3, lcsc="C7955")
C("C3", "100n", "+5V", "GND", (205, 160))

# ---------------------------------------------------------------- line drivers (open drain)
part("Q2", "Transistor_FET:AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23", (285, 45),
     {"1": "Q2_G", "2": "GND", "3": "DRV_RXD"}, lcsc="C20917", note="drives ERV RXD in intercept")
R("R16", "100", "TX_ERV", "Q2_G", (255, 45))
R("R17", "100k", "Q2_G", "GND", (255, 55))
R("R18", "1k", "+5V", "DRV_RXD", (305, 30), note="Ru1 pull-up / source resistance")
R("R19", "2.2k", "DRV_RXD", "GND", (305, 60), dnp=True, note="Rp1 shunt, fit only to lower the high level")

part("Q3", "Transistor_FET:AO3400A", "AO3400A", "Package_TO_SOT_SMD:SOT-23", (285, 105),
     {"1": "Q3_G", "2": "GND", "3": "DRV_TXD"}, lcsc="C20917", note="drives panel TXD in intercept")
R("R22", "100", "TX_PANEL", "Q3_G", (255, 105))
R("R23", "100k", "Q3_G", "GND", (255, 115))
R("R24", "2.2k", "+5V", "DRV_TXD", (305, 90), note="Ru2 pull-up, tune at bring-up")
R("R25", "2.2k", "DRV_TXD", "GND", (305, 120), dnp=True, note="Rp2 shunt, tune at bring-up")

# ---------------------------------------------------------------- ESP32-C6
part("U1", "Espressif:ESP32-C6-WROOM-1", "ESP32-C6-WROOM-1-N8", "Espressif:ESP32-C6-WROOM-1",
     (370, 80),
     {"1": "GND", "28": "GND", "29": "GND", "2": "+3V3", "3": "EN",
      # Footprint pins 1-14 are the module's left column (toward the comparators).
      # Gate-driving pins have no pull-up at reset: GPIO10, GPIO11, GPIO3.
      "9": "RX_PANEL",    # GPIO1,  UART1 RX (left)
      "11": "TX_ERV",     # GPIO10, UART1 TX (left, no pull at reset)
      "8": "RX_ERV",      # GPIO0,  UART0 RX (left)
      "26": "TX_PANEL",   # GPIO3,  UART0 TX (right, no pull at reset)
      "12": "RELAY_EN",   # GPIO11 (left, no pull at reset)
      "13": "USB_DN", "14": "USB_DP",
      "15": "BOOT",       # GPIO9 strap
      "10": "GPIO8",      # GPIO8 strap, pulled high
      "4": None, "5": None, "6": None, "7": None, "16": None, "17": None, "18": None,
      "19": None, "20": None, "21": None, "22": None, "23": None, "24": None, "25": None,
      "27": None},
     lcsc="C5366877")
R("R26", "10k", "+3V3", "EN", (320, 135), note="EN pull-up")
C("C4", "1u", "EN", "GND", (320, 145))
R("R27", "10k", "+3V3", "GPIO8", (320, 155), note="GPIO8 strap high")
part("TP1", "Connector:TestPoint", "EN", "TestPoint:TestPoint_Pad_D1.5mm", (385, 145),
     {"1": "EN"}, note="short to GND to reset")
part("TP2", "Connector:TestPoint", "BOOT", "TestPoint:TestPoint_Pad_D1.5mm", (400, 145),
     {"1": "BOOT"}, note="GPIO9; short to GND at reset for ROM download mode")
C("C5", "10u", "+3V3", "GND", (350, 135))
C("C6", "100n", "+3V3", "GND", (350, 145))

# ---------------------------------------------------------------- USB-C and power
part("J4", "Connector:USB_C_Receptacle_USB2.0_16P", "USB-C",
     "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12", (50, 215),
     {"A1": "GND", "B1": "GND", "A12": "GND", "B12": "GND", "SH": "GND",
      "A4": "+5V", "A9": "+5V", "B4": "+5V", "B9": "+5V",
      "A5": "CC1", "B5": "CC2", "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DN", "B7": "USB_DN",
      "A8": None, "B8": None}, lcsc="C165948")
R("R28", "5.1k", "CC1", "GND", (90, 200))
R("R29", "5.1k", "CC2", "GND", (90, 210))
part("U3", "Regulator_Linear:AMS1117-3.3", "AMS1117-3.3", "Package_TO_SOT_SMD:SOT-223-3_TabPin2",
     (150, 215), {"3": "+5V", "2": "+3V3", "1": "GND"}, lcsc="C6186")
C("C7", "10u", "+5V", "GND", (130, 235))
C("C8", "22u", "+3V3", "GND", (170, 235))
C("C9", "10u", "+3V3", "GND", (170, 252))
part("JP1", "Jumper:SolderJumper_2_Open", "BUS POWER (open)",
     "Jumper:SolderJumper-2_P1.3mm_Open_RoundedPad1.0x1.5mm", (220, 205),
     {"1": "BUS5V", "2": "JP1_B"}, note="never close with USB connected")
part("D6", "Diode:SS14", "SS14", "Diode_SMD:D_SMA", (255, 205), {"2": "JP1_B", "1": "+5V"},
     rot=180, lcsc="C2480")
R("R30", "1k", "+5V", "LED1_A", (220, 225))
part("LED1", "Device:LED", "POWER", LED0805, (255, 225), {"2": "LED1_A", "1": "GND"}, rot=180,
     lcsc="C2297")

part("#FLG1", "power:PWR_FLAG", "PWR_FLAG", "", (290, 200), {"1": "GND"})
part("#FLG2", "power:PWR_FLAG", "PWR_FLAG", "", (305, 200), {"1": "+5V"})
part("#FLG3", "power:PWR_FLAG", "PWR_FLAG", "", (320, 200), {"1": "BUS5V"})

for i, at in enumerate([(290, 240), (305, 240), (320, 240), (335, 240), (350, 240)], start=1):
    part(f"H{i}", "Mechanical:MountingHole", "M3", "MountingHole:MountingHole_3.2mm_M3", at, {})


# ================================================================ writer
def uid(*k):
    return str(uuid.uuid5(NS, "/".join(map(str, k))))


def font(size=1.27):
    return [Sym("effects"), [Sym("font"), [Sym("size"), size, size]]]


def prop(name, value, x, y, hide=False, angle=0):
    eff = font()
    if hide:
        eff.append([Sym("hide"), Sym("yes")])
    return [Sym("property"), name, value, [Sym("at"), x, y, angle], eff]


def transform(px, py, ang, rot):
    r = math.radians(rot)
    rx = px * math.cos(r) - py * math.sin(r)
    ry = px * math.sin(r) + py * math.cos(r)
    return round(rx, 4), round(-ry, 4), (ang + rot) % 360


DIRS = {0: (1, 0), 90: (0, -1), 180: (-1, 0), 270: (0, 1)}


def build():
    lib_defs, body = {}, []
    stub = 2.54
    for p in PARTS:
        lib, name = p["lib"].split(":")
        if p["lib"] not in lib_defs:
            lib_defs[p["lib"]] = flat_symbol(lib, name)
        sym = lib_defs[p["lib"]]
        sx, sy = (round(v / 2.54) * 2.54 for v in p["at"])
        sx, sy = round(sx, 4), round(sy, 4)
        is_power = p["ref"].startswith("#")
        in_bom = not (is_power or p["ref"].startswith(("H", "TP", "JP")))
        node = [Sym("symbol"), [Sym("lib_id"), p["lib"]], [Sym("at"), sx, sy, p["rot"]],
                [Sym("unit"), p["unit"]], [Sym("exclude_from_sim"), Sym("no")],
                [Sym("in_bom"), Sym("yes") if in_bom else Sym("no")],
                [Sym("on_board"), Sym("no") if is_power else Sym("yes")],
                [Sym("dnp"), Sym("yes") if p["dnp"] else Sym("no")],
                [Sym("uuid"), uid(p["ref"], p["unit"])]]
        dy = 2.54 if p["rot"] in (90, 270) else 6.35
        fa = 90 if p["rot"] in (90, 270) else 0
        node.append(prop("Reference", p["ref"], sx, sy - dy, hide=is_power, angle=fa))
        node.append(prop("Value", p["value"], sx, sy + dy, hide=is_power, angle=fa))
        node.append(prop("Footprint", p["footprint"], sx, sy, hide=True))
        node.append(prop("Datasheet", "", sx, sy, hide=True))
        if p["lcsc"]:
            node.append(prop("LCSC", p["lcsc"], sx, sy, hide=True))
        if p["note"]:
            node.append(prop("Note", p["note"], sx, sy, hide=True))
        unit_pins = [q for q in pins(sym) if q[0] in (0, p["unit"])]
        for q in unit_pins:
            node.append([Sym("pin"), q[1], [Sym("uuid"), uid(p["ref"], p["unit"], "pin", q[1])]])
        node.append([Sym("instances"), [Sym("project"), PROJECT,
                     [Sym("path"), "/" + ROOT, [Sym("reference"), p["ref"]], [Sym("unit"), p["unit"]]]]])
        body.append(node)

        seen = set()
        for (unit, num, pname, ptype, px, py, ang) in unit_pins:
            if num not in p["nets"] and ptype != "no_connect":
                raise SystemExit(f"{p['ref']} unit {p['unit']}: pin {num} ({pname}) not assigned")
            net = p["nets"].get(num)
            dx, dy, pang = transform(px, py, ang, p["rot"])
            x, y = round(sx + dx, 4), round(sy + dy, 4)
            if (x, y) in seen:
                continue
            seen.add((x, y))
            if net is None:
                body.append([Sym("no_connect"), [Sym("at"), x, y], [Sym("uuid"), uid(p["ref"], num, "nc")]])
                continue
            lang = (pang + 180) % 360
            ux, uy = DIRS[lang]
            ex, ey = round(x + ux * stub, 4), round(y + uy * stub, 4)
            body.append([Sym("wire"), [Sym("pts"), [Sym("xy"), x, y], [Sym("xy"), ex, ey]],
                         [Sym("stroke"), [Sym("width"), 0], [Sym("type"), Sym("default")]],
                         [Sym("uuid"), uid(p["ref"], p["unit"], num, "w")]])
            just = {0: ["left", "bottom"], 90: ["left", "bottom"], 180: ["right", "bottom"],
                    270: ["right", "bottom"]}[lang]
            eff = font()
            eff.append([Sym("justify")] + [Sym(j) for j in just])
            body.append([Sym("label"), net, [Sym("at"), ex, ey, lang], [Sym("fields_autoplaced"), Sym("yes")],
                         eff, [Sym("uuid"), uid(p["ref"], p["unit"], num, "l")]])

    sch = [Sym("kicad_sch"), [Sym("version"), 20250114], [Sym("generator"), "eeschema"],
           [Sym("generator_version"), "10.0"], [Sym("uuid"), ROOT], [Sym("paper"), "A3"],
           [Sym("title_block"), [Sym("title"), "ERV link interface board"], [Sym("date"), "2026-10-10"],
            [Sym("rev"), "A"], [Sym("comment"), 1, "Generated by gen/gen_sch.py - edit the script, not this file"],
            [Sym("comment"), 2, "See hardware/DESIGN.md"]],
           [Sym("lib_symbols")] + list(lib_defs.values())]
    sch += body
    sch.append([Sym("sheet_instances"), [Sym("path"), "/", [Sym("page"), "1"]]])
    sch.append([Sym("embedded_fonts"), Sym("no")])
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(dump(sch) + "\n")
    print(f"wrote {os.path.relpath(OUT)}: {len([p for p in PARTS if not p['ref'].startswith('#')])} symbols")


if __name__ == "__main__":
    build()
