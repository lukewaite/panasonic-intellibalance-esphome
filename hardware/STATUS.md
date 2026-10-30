# Rev A — status and next steps

**Start here when parts or boards arrive.** Design rationale is in [DESIGN.md](DESIGN.md); this
file is what's ordered, what to check on arrival, and the bring-up procedure in the order to do it.

Last updated 2026-10-30. Branch `hardware-rev-a`.

## Orders (placed 2026-10-10)

### JLCPCB — boards, SMD assembled

- 5 × PCB, 110 × 56 mm, 2 layers, FR-4 1.6 mm, green, HASL (leaded), 1 oz, tented vias.
- **Economic PCBA, top side, 2 of the 5 boards assembled.** The other 3 are bare spares.
- Files uploaded: `kicad/fab/ervlink-gerbers.zip`, `ervlink-bom.csv`, `ervlink-cpl.csv` (the CPL
  with the rotation/offset corrections from commit `0c63334`; the preview was checked part by part).
- PCB remark: any rails/tabs on the 56 mm short edges only.
- "Confirm Parts Placement" was recommended (Yes). If set, JLCPCB emails a placement to approve
  before soldering. **Approve it promptly or the order waits.** Check U1 (ESP module body inside the
  top edge), U2 pin 1 top-left, U3 tab on the right, Q1–Q3 pin 1 top-left, J4 pegs in the holes.
- Shipping: Global Standard Direct Line (8–12 business days after 4–6 days production). Likely
  handed to Canada Post; expect GST/HST and possibly a handling fee.
- Order total US$35.26 + US$10.72 shipping.

### DigiKey — owner-fitted parts and enclosure

| DigiKey # | Part | Qty | For |
|---|---|---|---|
| Z2615-ND | Omron/Aratas G6S-2 DC5 relay | 2 | K1 |
| CKN9565-ND | C&K OS102011MS2QN1 slide switch (vertical) | 2 | SW1 |
| 277-1265-ND | Phoenix 1715747 4-pos 5.08 mm screw terminal | 4 | J1, J2 |
| S1011EC-40-ND | Sullins PRPC040SAAN-RC 1×40 header | 1 | J3 (8 pins per board) |
| HM104-ND | Hammond 1591CSBK box | 1 | enclosure |
| 36-25511-ND | Keystone 25511 M3 × 12 mm nylon F-F standoff | 10 | 5 per board |

Not ordered, owner has them: **M3 × 6 or M3 × 8 screws** (Voron stock; never longer than 8 mm,
two would meet inside the 12 mm standoff), a USB-C cable and 5 V USB power supply, the Saleae, and
a short 4-conductor thermostat-wire jumper from the ERV terminals to J1.

## Not done yet

- **Analog capture of the installed board (step 4.4–4.7).** Skipped so far: listen-only firmware
  decodes both lines cleanly, so it is not needed to listen. Still needed **before intercept**:
  eTX/pTX levels set R24/R25 (the driver's high level towards the wall control).
- **Bench checks (step 5)** not done: relay click, SW1 override, EN-low relay-off, 3.3 V under
  Thread transmit. SW1 is now fitted and set to AUTO, so only the firmware (GPIO11, held off at
  boot) keeps the relay in passthrough.
- **`tools/decode.py` takes two channels named `RXD` and `TXD`.** The bring-up capture has four
  (both sides of each line). Either export/decode one pair at a time (name the pair RXD/TXD), or
  extend the tool to take a channel mapping.
- The explainer artifact (https://claude.ai/artifact/MWfcdhNfVyDjRu9qpwLJSf) matches rev A as
  ordered.

## Bring-up log

**2026-10-29 — board 1 installed, listen-only firmware running.**

- All THT parts fitted except **SW1** (left off on purpose: with SW1 out the coil has no supply,
  so the board is passthrough-only whatever the firmware does). SW1 orientation does not matter:
  the middle pin is common and the slider joins it to the outer pin on the side it is pushed
  towards (checked on the part). Pin 1 (+5V) is the outer pin on the AUTO side of the layout, so
  the silkscreen is right either way round.
- 2026-10-30: SW1 fitted and set to **AUTO**. Not yet mounted in the box: the 1591C floor needs
  the five standoff holes drilled (DESIGN.md "Enclosure"); the standoffs are nylon, so don't glue.
- Installed unpowered: the wall control worked exactly as before (passthrough proven).
- **Wire the data lines crossed** — the ERV's and the wall control's `TXD` terminal goes to the
  board's `RXD`, and vice versa, on both J1 and J2. Label-to-label puts the commands on the TXD
  tap and nothing on the RXD tap. See CLAUDE.md "Wiring".
- **A blank ESP32-C6 module boot-loops** (`invalid header: 0xffffffff`, USB drops every ~2.5 s),
  so the browser's serial picker can't hold the port. Put it in download mode first:
  `esptool --chip esp32c6 --port /dev/cu.usbmodem* --after no-reset flash-id` (or short the BOOT
  pad to GND at power-up). esptool read: ESP32-C6 rev v0.2, 8 MB flash, 40 MHz.
- First flash over USB from the HA ESPHome Device Builder ("Plug into this computer"); since then
  logs, states and OTA run over Thread. Secrets `thread_tlv` and `ervlink_api_key` live in the
  add-on's `secrets.yaml`; OTA inherits the API key (`ota: encryption: {}`).
- The board is powered from a USB brick. JP1 stays open (ERV 5 V budget unknown).
- Result: **RXD 25.0 frames/s, TXD 19.0 frames/s, 0 bad frames on both**, no unexpected
  registers, 4 junk bytes on RXD at boot only. Registers are HA entities `sensor.erv_link_*`.

## When the boards and parts arrive

### 1. Inspect (no soldering yet)

- Assembled boards: U1 sits squarely on its pads with the antenna in the top-right corner; U2's
  dot is top-left at the ▼; U3's tab is on the right; J4 sits at the bottom edge, opening facing out.
- If JLCPCB added rails, snap them off. Sand any tab nubs on the **long** edges: the board has only
  ~0.6 mm per side across the box.

### 2. Solder the through-hole parts

Lowest first, so each part sits flat with the board upside down: J3 (8 pins snapped off the strip,
short pins through the board), SW1, K1, then J1 and J2.

- **K1** fits only one way: the end with the lone pair of pins (coil) goes at the square pad.
- **J1/J2** wire openings face **outward** over the bottom edge.

### 3. Meter checks, unpowered

- Each J1 pin beeps to the same J2 pin (5V–5V, 0V–0V, RXD–RXD, TXD–TXD), through the relay's NC
  contacts for RXD and TXD.
- No adjacent pins short on J1 or J2.
- Board 5V and 3V3 (J3 pin 2) are not shorted to GND.
- **SW1: find which side is AUTO.** AUTO should connect switch pins 1–2 (relay coil enabled). Mark
  it. PASS holds the relay off regardless of firmware.

### 4. Install unpowered and capture with the Saleae (no firmware, no USB)

With no power the relay rests in passthrough, so this is a plain wiring change.

1. **Switch the ERV off at mains** before touching the terminals (5V is live; a slip onto 0V shorts
   the ERV's supply).
2. Short jumper from the ERV's terminal block to **J1 "TO ERV"**; the existing wall-control run to
   **J2 "TO WALL CONTROL"**. Order on both: 5V 0V RXD TXD, same as CN2.
3. Power the ERV. **The wall control must behave exactly as before.**
4. Saleae, analog, ≥1.25 MS/s like the 2025-12-14 capture: GND → J3 GND; channels on J3 **pRX,
   eRX, eTX, pTX**. Ignore dRX/dTX (driver outputs, idle when unpowered).
5. Record a mains power-on and a few button presses, logging **timestamps for every press**. Store
   as `captures/YYYY-MM-DD/` with notes from `captures/TEMPLATE-notes.md`. Export analog too.
6. Decode (see the decode.py note above). Expect 100 % BCC/parity/stop on every channel.
7. **Measure eTX's high level.** That's the ERV's TXD at the ERV end on a clean ground. If it is
   well above 2.3 V, the TXD tap's threshold (51 k / 10 k reference) needs changing before powering
   the board. Compare all levels with the July decode in CLAUDE.md.

### 5. Bench test on USB (ERV disconnected)

- 3.3 V present; PWR LED on. ESP flashes over USB (hold BOOT pad to GND at reset if needed).
- Relay clicks when GPIO11 goes high; INT LED follows; SW1 in PASS stops it.
- **Hold EN low (reset): relay must stay off.**
- Scope 3.3 V while the radio transmits (AMS1117 with ceramic capacitors is unverified).
- Optional: feed each tap from a bench supply and check it flips at the DESIGN.md thresholds
  (RXD 2.25–2.47 V, TXD 1.04–1.28 V).

### 6. Onward

Listen-only firmware on the ERV (register mapping via Home Assistant history), then intercept
forward-only, then injecting the known On/Off command. Details: DESIGN.md "Bring-up" steps 4–6.

## Box

Drill the long wall next to the terminals: two Ø8–10 mm cable holes and a 13 × 7.5 mm USB-C slot
(try your cable's plug first). Positions and floor holes for the five standoffs are in DESIGN.md
"Enclosure — Hammond 1591C". Measure the real floor thickness before drilling. Mount the box so the
antenna end sits ≥15 mm above the ERV's metal (12 mm standoffs give ~16 mm; don't go lower).
