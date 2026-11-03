# Panasonic Intelli-Balance 200 Wall Control Reverse-Engineering

Decoding the low-voltage protocol between a **Panasonic Intelli-Balance 200 ERV** and its
**FV-SW20VEC1 wall control**, with the goal of building an ESPHome-based inline interface for
Home Assistant (similar in spirit to ratgdo).

**Status:** the transport is fully decoded and verified. The register *semantics* are only
partially mapped — see [Register map](#register-map) and [Next steps](#next-steps).

## The protocol

### Physical layer

**4800 baud, 8 data bits, even parity, 1 stop bit (8E1), inverted.**

Inverted matters: both lines **idle at 0 V**, so the start bit is a *rising* edge. A conventional
non-inverted UART decode produces pure garbage on this link.

| | RXD line | TXD line |
|---|---|---|
| Idle | 0 V | 0 V |
| Active | 4.7 V | 2.3 V |
| Bit time | 208.4 µs | 208.4 µs |

Two practical gotchas:

- **Edges are RC-limited** — roughly 80 µs of slew on a 208 µs bit, and the two lines have very
  different active levels, so no single threshold suits both. Thresholds must sit at each line's
  true mid-swing (≈2.35 V for RXD, ≈1.15 V for TXD). Anywhere else and you get a duty-cycle skew
  that shifts the sampling points and corrupts the decode while looking like "noise" or
  "oscillator tolerance". `tools/decode.py` measures these from the capture, so this is handled.
- **The TXD line's 2.3 V high is below the ESP32 V_IH of ~2.48 V.** An ESP32 cannot reliably read
  that line directly; it needs a comparator or level shifter. Whether 2.3 V is the genuine driven
  level or an artifact of probe placement is still unconfirmed.

### Frame format

```
0x02 (STX) | A1 A0 | D7 D6 D5 D4 D3 D2 D1 D0 | 0x03 (ETX) | BCC
```

13 bytes. `A1 A0` and `D7..D0` are **ASCII hex characters**, so each frame carries a
2-character register address plus 4 data bytes.

**BCC** is the complement of the XOR over every byte from the address through ETX inclusive:

```python
def bcc(payload):        # payload = the 10 ASCII chars between STX and ETX
    x = 0
    for b in payload:
        x ^= b
    return (~(x ^ 0x03)) & 0xFF
```

Worked example — `02 '0' '0' 'F' 'F' 'F' 'F' 'F' 'F' 'F' 'F' 03 FC`: `'0'^'0'` is 0, the eight
`'F'` cancel to 0, `^ 0x03` gives `0x03`, and `~0x03` is `0xFC`. ✓

### Direction

**RXD = wall control → unit (commands). TXD = unit → wall control (state).**

The wire labels are therefore from the **unit's** point of view, not the panel's.

This is established from the data, not assumed. On the On/Off press, RXD register `00` changed,
held its new value for exactly 3 cycles, then **reverted** to its idle pattern — after which TXD
register `01` latched that same value permanently. A transient burst that reverts is a command;
the value that latches is the resulting state.

### Link timing

Both sides **free-run independently at ~200 ms** — no polling, no arbitration, no master. RXD's
period is 200.1 ms and TXD's is 200.2 ms, and they drift slowly against each other. Within a
cycle, blocks are spaced ~29.7 ms.

- RXD sends **5 blocks/cycle**: `00 10 20 30 F0`
- TXD sends **4 blocks/cycle**: `01 11 21 31`
- Once per second, TXD's slot carries `A1 B1 F1` *instead of* `01 11 21 31`

The address low nibble encodes direction: `0` on RXD, `1` on TXD. Because the two directions are
on separate wires and neither side waits for the other, an inline interface is much simpler than
a polled bus would be.

## Decoding a capture

`tools/decode.py` implements the whole pipeline — analog CSV → mid-swing re-threshold → inverted
8E1 UART → framing → BCC check. Standard library only, no dependencies.

```bash
# from the committed edge lists (instant)
./tools/decode.py --edges captures/2025-12-14/derived --start 6.8

# only frames whose register value changed — the entire capture in ~25 lines
./tools/decode.py --edges captures/2025-12-14/derived --start 6.8 --changes

# from a fresh Saleae analog export (~10 s for 26.9M samples)
./tools/decode.py captures/YYYY-MM-DD/analog.csv --save-edges captures/YYYY-MM-DD/derived
```

No per-capture tuning is needed: thresholds are measured from each channel's own rails and columns
are matched by Saleae channel name. The tool reports the detected voltage rails, the measured bit
time, and byte/parity/stop/frame/BCC counts per line.

**Treat anything below 100 % on all of those as wrong parameters or a bad capture, not a noisy
bus** — on a good capture every single byte and frame checks out. The tool prints a warning when
the decode isn't clean.

### Saleae capture settings

| Setting | Value |
|---|---|
| Channels | 2 (currently D2 = TXD, D3 = RXD) |
| **Analog** | **required** — this is what the decoder reads |
| Analog sample rate | 1.25 MS/s (≈260 samples/bit; 625 kS/s is still ample) |
| Digital | optional, and not used by the decoder |
| Duration | 20–30 s per capture (≈40 MB `.sal`) |

For the built-in **Async Serial** analyzer, if you want to read frames live in the Saleae app:

| Setting | Value |
|---|---|
| Bit Rate | **4800** |
| Bits per Frame | 8 |
| Stop Bits | 1 |
| Parity | **Even** |
| Significant Bit | Least Significant Bit first |
| Signal inversion | **Inverted** |

Expect the in-app decode to still flag some framing errors, especially on TXD — the Saleae's
digital threshold is a single fixed level and cannot suit both a 4.7 V line and a 2.3 V one at
once. That is a display artifact of the digital threshold, not a bad capture. `tools/decode.py`
on the analog export is the source of truth.

### Capture timing constraints

These follow from the link timing and set how long to hold still around each action:

- Cycle time is 200 ms, so leave **≥1 s** either side of an action to catch the transition.
- Registers `A1`/`B1`/`F1` appear only **once per second** — a capture shorter than a few seconds
  can miss the sensor data entirely.
- A command occupies the command line for **3 cycles (600 ms)** and then reverts, so a press is
  visible on RXD for only ~0.6 s. Don't trim captures tightly.
- After mains power-on the unit is **silent for 2.1 s** while it boots.
- Allow **≥3 s between successive presses** so each command burst is unambiguous.

## Register map

From the 2025-12-14 capture (ERV mains power-on, then an On/Off press). This is one capture of one
transition, so it pins down very little.

Command line (RXD) — note the `FF` staircase in the idle pattern:

| addr | idle / no request | during On/Off command (3 cycles) |
|---|---|---|
| `00` | `FFFFFF00` | `10303000` |
| `10` | `FFFF0000` | `20FF0000` |
| `20` | `FF000000` | `00000000` |
| `30` | `00000000` | `00000000` |
| `F0` | `00000000` | `00000000` |

At mains power-on all five RXD registers send `FFFFFFFF` once, then the line goes quiet for 2.1 s
while the unit boots.

State line (TXD):

| addr | unit off | unit on |
|---|---|---|
| `01` | `00000000` | `10303000` |
| `11` | `00000000` | `20000000` |
| `21` / `31` / `A1` | `00000000` | `00000000` |
| `B1` | `41D13055` | `41D13055` |
| `F1` | `00003E00` | `00003E00` |

### Hypotheses — unconfirmed

Everything below is speculation and should be treated as such until a second capture confirms it.

- **`B1` is probably the sensor register.** The operator noted outside temp 37 °F and outside
  humidity 85 %RH, and `B1 = 41 D1 30 55` contains `0x55 = 85`, matching outside RH exactly. The
  rest is guesswork: `0x41 = 65` (inside temp °F?), `0x30 = 48` (inside RH %?), `0xD1 = 209`
  (outside temp in some offset encoding? 37 °F = 2.78 °C doesn't fall out of this cleanly).
  One matching byte out of four is not evidence — it could easily be coincidence.
- **`00`/`01` = `10 30 30 00`:** the two equal `0x30` bytes look like supply and exhaust airflow,
  which the operator noted as 80 cfm / 80 cfm. But `0x30` is 48, not 80, so the mapping is unknown
  — percent of maximum? A speed index? Byte 0 (`0x10`) is plausibly a mode/on flag.
- **`0xFF` in a command byte may mean "no change / don't care",** which fits both the idle pattern
  and register `10` sending `20FF0000` while the resulting state is `20000000`.
- **`F1 = 00003E00`** — `0x3E` is 62, unexplained. The ASHRAE setting was 20, so it isn't that
  directly.

## Hardware

- 4-conductor thermostat-style link on `CN2`: `5V`, `0V`, `RXD`, `TXD`. The 5 V supply comes from
  the unit to the wall control.
- Wall control PCB (photo `pcb.jpg`):
  - Silkscreen PCB-86ER-U-M-V1.1 (2020/08/21), sticker SW20VE111D RoHS TNA16.
  - `CN1` unpopulated 5-pin header; `J1` 6-pin test header (RESET, +5V, GND), suggesting
    in-circuit programming/debug.
  - No RS-485 or CAN transceiver — signaling goes more or less directly to the MCU.
  - Main MCU at U1 marked `0257 231014` (vendor/family unidentified); likely drives the display
    and buttons directly.
- UI buttons: On/Off and Boost (immediate actions); In/Out (toggles the displayed temp source);
  Time, Set, Up, Down, Filter (hold 3 s to reset), Vent (settings). The display shows
  supply/exhaust speed, inside/outside temp and humidity, and an ASHRAE value.

### Capture setup

Ground to `0V`, probe `RXD` and `TXD`, and **capture analog as well as digital** — digital exports
bake in threshold choices, and getting those wrong is what derailed the first attempt. Logic
analyzer mapping used so far: `D2` = TXD (red), `D3` = RXD (orange). In the CSV exports, column 1
is RXD and column 2 is TXD.

## Next steps

1. **Airflow scaling — do this one first.** The `0x30` byte is the single biggest unknown, and one
   capture cracks it. Rather than two captures at two speeds, run a **staircase**: enter the
   airflow setting, then press Up once every ~3 s for 6–8 steps, writing down the displayed CFM
   after each press. One 30 s capture then yields a whole calibration curve of
   (displayed CFM → byte value) pairs, which distinguishes a linear scale from a step index from a
   percentage immediately. Two points could fit several mappings; eight cannot.
2. **Sensor layout.** Toggle In/Out to confirm which `B1` bytes are inside vs outside, and
   re-capture at a clearly different temperature/humidity to test the `0x55 = 85 %RH` hypothesis.
   Do not build on that hypothesis until it is confirmed.
3. **Remaining controls.** Boost, ASHRAE percentage, timer, filter reset — one isolated action per
   capture.
4. **Hardware validation.** Measure the TXD high level at both cable ends to settle whether the
   2.3 V swing is real, then choose a level shifter or comparator front end.
5. **Passive ESPHome sniffer** on both lines (inverted UART, 4800 8E1) to log long runs and catch
   registers that only move on a slow schedule.
6. **Inline bridge** with selective command injection, once the command registers are understood.

Per-capture protocol: store as `captures/YYYY-MM-DD/` with a `YYYY-MM-DD-notes.md` operator log,
one isolated action per capture, and **log a timestamp for every button press**. The existing notes
were timed by counting in the operator's head, and reconciling them against the capture took real
effort — the press is at 14.24 s, not the noted ~10 s.

## Repo layout

```
tools/decode.py                 The decoder (stdlib only)
pcb.jpg                         Wall control PCB photo
captures/2025-12-14/
  Session 0.sal                Saleae session — archival ground truth (28 MB)
  analog.csv                   Analog export, 645 MB — GITIGNORED, regenerate from the .sal
  digital.csv, rxd.txt, txd.txt  First-attempt exports — INVALID, kept for provenance only
  2025-12-14-notes.md          Operator log
  derived/                     Re-thresholded edge lists + state-change summary
```

`analog.csv` is gitignored because it exceeds GitHub's 100 MB per-file limit. Nothing is lost: the
`.sal` holds the full raw analog data, and the committed `derived/edges-*.csv` reproduce the decode
byte-for-byte without it.

## On the first attempt (December 2025)

The initial investigation reached a wrong decode and stalled. Its protocol conclusions —
9600 baud on RXD, 8N1, non-inverted, "3.3 V TTL" levels, and byte streams like `FC C7` and
`3F AE E6 EE` — are all artifacts of two off-centre Saleae thresholds on slow edges, which
cascaded into the wrong baud rate, polarity and framing. The framing errors it attributed to
oscillator tolerance had the same root cause.

Its *hardware* observations (pinout, PCB markings, button inventory) were sound and are retained
above. The original log is preserved in git history at commit `a8d523a`.
