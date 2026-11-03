# ERV ↔ wall control protocol

The serial link between the Panasonic Intelli-Balance 200 and its FV-SW20VEC1 wall control, as
decoded from logic-analyzer captures and the installed board. Verified against the 2025-12-14
analog capture with no inconsistencies: 596/596 frames pass BCC, 7769/7769 bytes pass parity
and stop bits. The capture notes in `captures/` hold the timestamped evidence for each point.

## Surprises

- **The terminal labels are backwards.** CN2's `RXD`/`TXD` are the wall control's point of view
  from its own PCB, but the conductor on the terminal *labelled* `TXD` carries the wall control's
  **commands**. This repo calls that line **RXD** (and the state line TXD), everywhere: captures,
  decoder, firmware, nets. Found 2026-10-29 when label-to-label wiring put the commands on the
  board's TXD tap. The board's J1/J2 silkscreen now follows the terminal labels (pin 3 printed
  `TXD` = RXD net); boards from the 2026-10-10 order are printed with the line names and must be
  wired crossed.
- **Inverted UART.** Both lines idle at 0 V and the start bit is a rising edge. A normal UART
  decode gives garbage (the first attempt's "9600 baud 8N1" came from that).
- **TXD only reaches 2.3 V**, below an ESP32's V_IH (~2.48 V), and RXD reaches 4.7 V: no single
  threshold suits both, hence the comparator taps. Edges are RC-limited (~80 µs on a 208 µs bit),
  so thresholds must sit at mid-swing (≈2.35 V RXD, ≈1.15 V TXD) or bit timing skews.
- **The wall control runs on the ERV's 5 V.** At mains power-up it sends `FFFFFFFF` once on every
  RXD register, then is silent ~2.1 s while booting; the ERV's TXD starts within 50 ms. That
  silence is the most reliable sign of a power-up.
- **The ERV comes back *off* after a power cut** and forgets its airflow (`01`/`11` read zero
  while off); the wall control re-sends a full snapshot on Power on.
- **No arbitration.** Both sides free-run their own ~200 ms cycle and drift against each other
  (200.1 vs 200.2 ms); a shared gap between the lines comes and goes over minutes.
- **`FF` means "leave this byte alone"** in commands, and the idle pattern is an FF staircase.
- **Airflow codes skip:** 60–180 CFM are odd codes 1–13 (CFM = 50 + 10 × code), but 200 CFM is
  **16**.
- **`FE`/`FE` airflow while on = the ASHRAE rest**: the ERV stops its fans for the rest of each
  hour on its own. The temperature readings then drift to the air inside the unit.
- **The ERV times boost itself** (60 min) and restores the airflow; the panel's countdown is
  display only. Boost overrides a rest, and the cycle restarts with a full run period after it.
- **BCC is never `02`**: it's the complement of an XOR of 7-bit bytes, so bit 7 is always set. An
  STX inside a frame always means a truncated frame.

## Physical layer

| | RXD (commands) | TXD (state) |
|---|---|---|
| Direction | wall control → ERV | ERV → wall control |
| Idle | 0 V | 0 V |
| Active | 4.7 V | 2.3 V |
| Bit time | 208.4 µs | 208.4 µs |

**4800 baud, 8E1, inverted**, 11 bits/byte. The bit time fits all pulse widths to 0.004 bits rms.
Whether TXD's 2.3 V is the driven level or a loaded one hasn't been measured at the ERV end.

## Frame format

```
0x02 (STX) | A1 A0 | D7 D6 D5 D4 D3 D2 D1 D0 | 0x03 (ETX) | BCC
```

13 bytes. The address and data are **ASCII hex characters**. BCC is the complement of the XOR of
every byte from A1 through ETX:

```python
def bcc(payload):        # the 10 ASCII chars between STX and ETX
    x = 0
    for b in payload:
        x ^= b
    return (~(x ^ 0x03)) & 0xFF
```

Example: `02 '0' '0' 'F' 'F' 'F' 'F' 'F' 'F' 'F' 'F' 03 FC`.

The 8 data characters are 4 bytes, each sent **low nibble first**: `"3103F0B4"` = `13 30 0F 4B`.

## Timing

- RXD sends `00 10 20 30 F0` every cycle, frames ~29.7 ms apart, then a ~51 ms gap.
- TXD sends `01 11 21 31`, and once a second `A1 B1 F1` in their place; ~81 ms gap after `31`.
- The low address nibble is the direction: `0` on RXD, `1` on TXD.
- A command is held for 3 cycles (~0.6 s), then the wall control reverts to idle. The ERV applies
  it 0.2–0.4 s after its first frame. Fields edited in quick succession overlap in one frame.

Direction was established from the data: on an On/Off press, RXD `00` changed for exactly 3
cycles and reverted, and TXD `01` then latched the same value. The transient is the command, the
latched value the state.

## Registers

Each command register has a matching state register (`x0` → `x1`). Bytes as values:

| Cmd → state | Byte 0 | Byte 1 | Byte 2 | Byte 3 |
|---|---|---|---|---|
| `00` → `01` | power (0 off, 1 on) | supply airflow code | exhaust airflow code | 0 |
| `10` → `11` | ASHRAE, tens of min/h | boost length, tens of min (state: only while boosting) | 0 | 0 |
| `20` → `21` | boost (1 on) | 0 | 0 | 0 |
| — → `B1` | inside temp °C | inside %RH | outside temp °C | outside %RH |

- **Idle commands:** `00 = FFFFFF00`, `10 = FFFF0000`, `20 = FF000000`, `30`/`F0 = 00000000`.
- **Power on** (panel): `00 = {1, supply, exhaust, 0}`, `10 = {ashrae, FF, 0, 0}`, `20 = 0`.
- **Boost on:** `00 = {1, FF, FF, 0}`, `10 = {ashrae, 6, 0, 0}`, `20 = {1, 0, 0, 0}`. While boosting
  `01` reports 200/200 (code 16). **Cancel:** `20 = 0` plus an explicit airflow restore in `00`.
- **Airflow codes seen:** 1, 3, 7, 9, 13, 16. Even codes and 14/15 are never sent by the panel;
  whether the ERV accepts them is untested.
- **Rest:** at 30 min/h, `01 = 10EFEF00` from exactly 30 min after the setting took effect, back
  to the airflow 30 min later. At 60 min/h it never rests.
- **ASHRAE:** 10 min/h steps; the byte is the value in tens (seen 1, 2, 3, 6).
- **`B1`:** whole degrees Celsius (the panel converts to °F). Sub-zero encoding is unseen; signed
  is assumed.
- **UI-only buttons:** Vent opens the airflow editor (each field sends its command as you move
  past it, even if unchanged). A short Filter press sends nothing.

Never seen to change: `30`/`F0` (`00000000`), `31`/`A1` (`00000000`), `F1` (`00003E00`).
Unknown: the 3 s Filter reset, sub-zero temperatures, the ASHRAE range limits.

## Decoding a capture

`tools/decode.py` takes a Saleae **analog** CSV export (columns named `RXD` and `TXD`), measures
each line's rails, re-thresholds at mid-swing with hysteresis, and decodes inverted 8E1 with
parity, stop-bit and BCC checks. Stdlib only.

```bash
./tools/decode.py --edges captures/2025-12-14/derived --start 6.8            # from committed edges
./tools/decode.py --edges captures/2025-12-14/derived --start 6.8 --changes  # register changes only
./tools/decode.py captures/YYYY-MM-DD/analog.csv --save-edges captures/YYYY-MM-DD/derived
```

- **Anything under 100 % on BCC, parity or stop bits means wrong parameters or a bad capture,
  not a noisy bus.** Margins are wide (clean from 204–215 µs bit time); `--bit-us` overrides if a
  capture is more than 5 % off 208.4 µs.
- Capture **analog** (1.25 MS/s is plenty): a digital export bakes in one threshold. Keep ≥1 s
  either side of an action and ≥3 s between presses, and log a timestamp for every press.
- The 2025-12-14 capture: Saleae `D2` = TXD, `D3` = RXD; mains power-on ≈ 6.83 s, On/Off press
  ≈ 14.24 s. `Session 0.sal` is the ground truth; its 645 MB `analog.csv` is gitignored (stream
  it line by line if regenerated), and `derived/` reproduces the decode byte for byte.

## Wall control hardware

Photo: [pcb.jpg](../pcb.jpg).

- 4-wire link on `CN2`: `5V`, `0V`, `RXD`, `TXD` (labels reversed, see above).
- PCB `PCB-86ERV-M-V1.1` (2020/08/21), sticker `SW20VE111D`. MCU at U1 marked `0257 231014`.
- `J1` 4-pin header: TOOL0, RESET, +5V, GND. `CN1` 5-pin header, unpopulated.
- No RS-485 or CAN transceiver: the lines go almost directly to the MCU.
- Buttons: On/Off, Boost, In/Out (inside/outside readings), Time, Set, Up, Down, Filter (hold
  3 s to reset), Vent (airflow settings).
