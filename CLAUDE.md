# CLAUDE.md

## What this repo is

Reverse-engineering of the low-voltage serial link between a **Panasonic Intelli-Balance 200
ERV** and its **FV-SW20VEC1 wall control**, with the goal of building an ESPHome-based inline
interface (ratgdo-style) for Home Assistant.

This is a **research/data repo**, not a software project. There is no build, no test suite, no
dependencies. It holds logic-analyzer captures plus notes.

> Not a git repository yet — run `git init` before doing anything destructive.

## Layout

```
README.md                       Research log — protocol section rewritten to match the decode
pcb.jpg                         Wall control PCB photo (2.7 MB)
tools/decode.py                 The decoder. Stdlib only, no dependencies.
captures/2025-12-14/
  Session 0.sal                Saleae session, 28 MB — ARCHIVAL GROUND TRUTH
  analog.csv                   Saleae analog export, 1.25 MS/s, 26.9M samples, 645 MB — GITIGNORED
  digital.csv                  Saleae digital export — thresholds were wrong, do not use
  rxd.txt / txd.txt            Saleae Async Serial decode — INVALID (wrong baud/polarity)
  2025-12-14-notes.md          Operator log for the capture
  derived/
    edges-rxd.csv              Re-thresholded edges, enough to reproduce the decode (272 KB)
    edges-txd.csv                                                                   (232 KB)
    state-changes.txt          The whole capture as 25 lines of register deltas
```

`Session 0.sal` is the archival ground truth — it holds the full raw analog data for both
channels. `analog.csv` is **gitignored**: at 645 MB it exceeds GitHub's per-file limit, and it is
a regenerable export (re-export from the Saleae app when needed). The committed `derived/edges-*.csv`
let you reproduce the full decode without it — verified byte-identical to a full analog run.

Never trust `digital.csv`, `rxd.txt`, or `txd.txt`.

### Running the decoder

```bash
./tools/decode.py --edges captures/2025-12-14/derived --start 6.8            # instant
./tools/decode.py --edges captures/2025-12-14/derived --start 6.8 --changes  # register deltas only
./tools/decode.py captures/2025-12-14/analog.csv --save-edges captures/2025-12-14/derived
```

The last form re-extracts from analog (~10 s for 26.9M samples). It prints per-line byte/parity/
stop/BCC/frame counts to stderr; **anything below 100 % on all of those means the framing
parameters are wrong, not that the bus is noisy.**

`analog.csv` is 645 MB / 26.9M lines — stream it line by line, don't load it into memory, and
don't reach for numpy (not installed). Columns: `Time [s], RXD, TXD`.

## Wiring / capture setup

4-conductor thermostat-style link on `CN2`: `5V`, `0V`, `RXD`, `TXD`.
Logic analyzer mapping used in the 2025-12-14 capture: `D2` = TXD (red), `D3` = RXD (orange),
ground to `0V`. In the CSV exports, column 1 = RXD, column 2 = TXD.

Capture timeline (t is capture-relative): ERV mains power-on ≈ 6.83 s, On/Off button press
≈ 14.24 s, capture ends 21.52 s.

## VERIFIED protocol

All of the following is confirmed against the analog capture with **zero** inconsistencies:
596/596 frames pass BCC, 7769/7769 bytes pass parity and stop-bit checks.

### Physical layer

| | RXD line | TXD line |
|---|---|---|
| Idle level | **0 V** | **0 V** |
| Active level | **4.7 V** | **2.3 V** |
| Bit time | 208.4 µs | 208.4 µs |

- **4800 baud, 8 data bits, even parity, 1 stop bit (8E1) — 11 bits/byte, INVERTED.**
- Inverted means idle = 0 V and the start bit is a **rising** edge. A normal (non-inverted)
  UART decode produces garbage — that is the single biggest error in the old notes.
- Bit time fit by requiring all pulse widths to be integer multiples of T: rms residual
  0.004 bits at 208.4 µs. Unambiguous.
- Edges are heavily RC-limited (~80 µs slew on a 208 µs bit). Threshold **at true mid-swing**
  (RXD ≈ 2.35 V, TXD ≈ 1.15 V) or you get a large duty-cycle skew and spurious framing errors.
- The 2.3 V swing on TXD is **below the ESP32 V_IH of ~2.48 V** — an ESP32 cannot reliably read
  this line directly. Needs a comparator or level shifter. (Open question: is 2.3 V the real
  driven level or an artifact of where the probe was placed?)

### Frame format

```
0x02 (STX) | A1 A0 | D7 D6 D5 D4 D3 D2 D1 D0 | 0x03 (ETX) | BCC
```

13 bytes total. `A1 A0` and `D7..D0` are **ASCII hex characters** ('0'–'9', 'A'–'F'), so the
payload is a 2-char address plus 4 data bytes.

**BCC = `~(XOR of every byte from A1 through ETX inclusive) & 0xFF`**

```python
def bcc(payload_bytes):          # payload_bytes = the 10 ASCII chars between STX and ETX
    x = 0
    for b in payload_bytes: x ^= b
    return (~(x ^ 0x03)) & 0xFF
```

Worked example: `02 '0' '0' 'F' 'F' 'F' 'F' 'F' 'F' 'F' 'F' 03 FC` →
`0x30^0x30 = 0`, eight `'F'` XOR to 0, `^0x03 = 0x03`, `~0x03 = 0xFC`. ✓

### Link timing

Both sides **free-run independently** at ~200 ms; there is no polling or arbitration. RXD period
200.1 ms, TXD period 200.2 ms, and they slowly drift against each other. Within a cycle, blocks
are spaced ~29.7 ms.

- **RXD sends 5 blocks/cycle**: addresses `00 10 20 30 F0`
- **TXD sends 4 blocks/cycle**: addresses `01 11 21 31`
- Once per second, TXD's slot carries `A1 B1 F1` (3 blocks) *instead of* `01 11 21 31`

Note the address low nibble encodes the direction: `0` on RXD, `1` on TXD.

### Direction (established, not assumed)

**RXD = wall control → unit (commands). TXD = unit → wall control (state).**

The labels are therefore from the **unit's** point of view, not the panel's.

Proof, from the On/Off press: RXD `00` changed to a new value and held it for exactly 3 cycles
(14.236, 14.436, 14.636 s), then **reverted** to its idle pattern. TXD `01` then latched that
same value **permanently**. A transient 3× burst that reverts is a command; the value that
latches is the resulting state.

### Observed register values

Command line (RXD), idle / "no request" pattern — note the FF staircase:

| addr | idle | during On/Off command (3 cycles) |
|---|---|---|
| `00` | `FFFFFF00` | `10303000` |
| `10` | `FFFF0000` | `20FF0000` |
| `20` | `FF000000` | `00000000` |
| `30` | `00000000` | `00000000` |
| `F0` | `00000000` | `00000000` |

At mains power-on (6.83 s) all five RXD registers send `FFFFFFFF` once, then the line goes quiet
for 2.1 s while the unit boots.

State line (TXD):

| addr | unit off | unit on (after press) |
|---|---|---|
| `01` | `00000000` | `10303000` |
| `11` | `00000000` | `20000000` |
| `21` | `00000000` | `00000000` |
| `31` | `00000000` | `00000000` |
| `A1` | `00000000` | `00000000` |
| `B1` | `41D13055` | `41D13055` |
| `F1` | `00003E00` | `00003E00` |

## Hypotheses — NOT yet confirmed

Treat everything in this section as unproven. The transport is solved; the semantics are not.

- `B1` is the **sensor register**. Operator noted outside temp 37 °F and outside humidity 85 %RH.
  `B1 = 41 D1 30 55` and **`0x55 = 85`** matches outside RH exactly. Speculative reading of the
  rest: `0x41 = 65` (inside temp °F?), `0x30 = 48` (inside RH %?), `0xD1 = 209` (outside temp in
  some offset/signed encoding? 37 °F = 2.78 °C does not fall out of this cleanly).
  One matching byte is not proof — could be coincidence.
- `00`/`01` byte layout `10 30 30 00`: the **two equal `0x30` bytes** are suggestive of supply and
  exhaust airflow, which the operator noted as 80 cfm / 80 cfm. But `0x30 = 48`, not 80, so any
  mapping is unknown (percent of max? speed index? something else). Byte 0 `0x10` is probably a
  mode/on flag.
- `0xFF` in a command byte may mean "no change / don't care" — consistent with the idle pattern
  and with `10` sending `20FF0000` while the resulting state is `20000000`.
- `F1 = 00003E00`, `0x3E = 62` — unexplained. ASHRAE setting was 20, so this is not it directly.

## Corrections to README.md

The README predates a correct decode. These claims in it are **wrong**:

1. "RXD ~9600 baud" — no. **Both lines are 4800 baud.**
2. "non-inverted (idle high)" — no. **Both lines are inverted, idle low (0 V).**
3. "8N1" — no. **8E1** (even parity); 11 bits/byte, confirmed by the 2.29 ms byte pitch.
4. "signal swing ~1.2 V to ~3.8 V, treat as 3.3 V TTL" — no. RXD is **0–4.7 V (5 V logic)**;
   TXD is **0–2.3 V**. The 1.2–3.8 V figures were an artifact of bad thresholds on slow edges.
5. Decoded byte streams like `FC C7`, `3F AE E6 EE`, `2E F9` are **decode artifacts** and carry
   no meaning. Same for the "framing errors" attributed to oscillator tolerance — the real cause
   was wrong polarity and off-centre thresholds.
6. "Directionality to be determined" — resolved, see above.

The README's hardware observations (connector pinout, PCB markings, button inventory) are fine.

## Conventions for this repo

- Store new captures as `captures/YYYY-MM-DD/` with a `YYYY-MM-DD-notes.md` operator log.
- **Always log wall-clock or capture-relative timestamps for every button press.** The existing
  notes were timed by counting in the operator's head, which cost real effort to reconcile
  against the capture (the press is at 14.24 s, not the noted ~10 s).
- Export analog, not just digital. Digital exports bake in threshold choices that turned out to
  be wrong.
- When decoding, verify against BCC + parity + stop bits. All three at 100 % is the bar; anything
  less means the framing parameters are off, not that the bus is noisy.
