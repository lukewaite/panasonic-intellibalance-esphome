# Panasonic Intelli-Balance 200 Wall Control Reverse-Engineering

Goal: decode the low-voltage protocol between the Intelli-Balance 200 unit and the FV-SW20VEC1 wall control, then build an ESPHome-based inline interface (similar spirit to ratgdo).

## Current knowledge
- 4-conductor link labeled `5V`, `0V`, `RXD`, `TXD` (thermostat-style connector).
- Likely 5 V supply provided from the unit to the wall control.
- Observed signal swing on RXD/TXD: ~1.2 V to ~3.8 V (treat as 3.3 V TTL levels).
- UI buttons: On/Off, Boost (immediate actions); In/Out (toggles displayed temp source); Time, Set, Up, Down, Filter (hold 3s to reset), Vent (used for settings); display shows supply/exhaust speed, inside/outside temp/humidity, ASHRAE value.
- Wall control PCB (photo `pcb.jpg`):
  - Silkscreen PCB-86ER-U-M-V1.1 (2020/08/21), sticker SW20VE111D RoHS TNA16.
  - CN2 = `5V`, `0V`, `RXD`, `TXD` (matches the field wiring).
  - CN1 unpopulated 5-pin header, J1 6-pin test header (RESET, +5V, GND) suggesting in-circuit programming/debug.
  - No obvious RS-485/CAN transceiver; signaling likely direct 5 V TTL UART to the MCU.
  - Main MCU at U1 marked `0257 231014` (unknown vendor/family); likely handles display/buttons directly.

## Capture wiring notes
- Current logic analyzer lead mapping (D2/D3): `D2` = red lead on `TXD`; `D3` = orange lead on `RXD`; common ground to `0V`.

## Working assumptions
- Signaling is probably TTL UART at 5 V. Idle-high would indicate non-inverted UART; idle-low or differential swing would suggest otherwise.
- RXD/TXD are relative to the wall control (i.e., panel RXD = panel input, TXD = panel output). To be confirmed by capture.

## Captures
- `captures/2025-12-14/2025-12-14-notes.md` and analyzer exports `rxd.txt` / `txd.txt` (latest re-export). Sequence: power-on of ERV, then On/Off press; display at capture time: 80 cfm supply/exhaust, ASHRAE 20.
- Raw exports: `captures/2025-12-14/digital.csv` and `analog.csv` (full capture).
- Observed levels: RXD ~1.2–3.9 V; TXD ~1.2–2.34 V. Use per-channel thresholds (RXD ~1.5–2.0 V, TXD ~1.0–1.5 V).
- Preliminary decode from raw edges:
  - RXD: ~9600 baud, 8N1, non-inverted (idle high). Bytes around 6.13–6.20 s decode cleanly (repeating `0xFC` frames, etc.).
  - TXD: ~4800 baud, 8N1, non-inverted (idle high). Bytes around 6.178–6.25 s decode to a repeating stream (`0x3F 0xAE 0xE6 0xEE ...`).
- Suggested Saleae setup: separate Async Serial analyzers per line (RXD at 9600 8N1 non-inverted; TXD at 4800 8N1 non-inverted). Confirm framing errors drop to zero.
- Glitch-filtered capture (2025-12-14, thresholds ~2.0 V RXD / ~1.1 V TXD):
  - Offline decode works cleanly at bit times ~102.4 µs (RXD ≈ 9.76 kbps) and ~204.3 µs (TXD ≈ 4.89 kbps), 8N1, non-inverted, grouping with 5 ms gaps.
  - RXD common frames: short `FC C7` pairs (hundreds), and longer status-like blocks with heavy `FC/FF/C7` patterns.
  - TXD common frames: frequent `E6 3E EE 9F … 2E F9` blocks, heartbeats like `3F 3E AE 9D` and `EE … 2E`, plus shorter `A6 3A` bursts.
  - Saleae still flags some framing errors (likely oscillator/tolerance); offline decode of the raw digital edges yields consistent packets.

## Immediate data to gather
1) **Idle voltages**: measure each conductor to ground with the system powered. Note if RXD/TXD sit high (~5 V) or low at idle.  
2) **Continuity**: confirm the 5 V and 0 V pins are shared with panel ground; ensure logic analyzer ground matches.  
3) **Board markings (optional but helpful)**: photos or chip part numbers on the wall control PCB and the unit’s low-voltage board, especially any transceivers or test pads.

## First capture plan (Saleae)
- Connect GND to `0V`. Probe `RXD` and `TXD`. Leave `5V` unprobed unless using analog for reference.
- Start with 10–25 MHz digital sample rate; if file size is an issue, also take a 2–5 MHz version.
- Capture sequences:
  - Full power-on from wall control connected.
  - Button events spaced a few seconds apart: Off→On, Boost toggle, In/Out toggle, entering/exiting a settings change (e.g., adjust a speed or timer).
  - Let the unit run briefly to capture steady-state chatter.
- Log actions with timestamps for each capture.
- Run Saleae protocol suggestions on each line: UART search (common rates 9600/19200/38400/57600/115200, 8N1), I2C, 1-Wire. Check if the two lines look differential/inverted.

## Next steps after first captures
- Determine directionality: which line is panel→unit vs unit→panel.
- Identify baud rate and framing; extract a few payloads per button action.
- Spot recurring frames (heartbeat/status) vs event-driven frames.
- Based on baud and framing, craft a passive ESPHome UART sniffer to log traffic for longer runs, then move toward an inline bridge with selective command injection.

If you can provide the idle voltages and a power-on + button-press capture, we can lock in baud/framing and start decoding messages.
