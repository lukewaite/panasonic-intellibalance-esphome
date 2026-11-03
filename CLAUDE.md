# CLAUDE.md

Home Assistant control of a Panasonic Intelli-Balance 200 ERV through an inline ESP32-C6 board on
the 4-wire link to its FV-SW20VEC1 wall control. Board 1 is installed and running the ESPHome
firmware; it also turns the ERV back on after a mains outage (so an unprompted power-on after an
outage is that, not a stray intercept).

**Line names:** in this repo "RXD" is always the command line (wall control → ERV) and "TXD" the
state line. The physical terminal labels are the other way round. Read `docs/protocol.md`
"Surprises" before touching wiring, captures or decoding.

## Where things are

- `docs/protocol.md`: the decoded protocol, its quirks, and how to decode a capture.
- `hardware/DESIGN.md`: the board. `hardware/STATUS.md`: build/bring-up log and what's untested.
  `hardware/kicad/README.md`: regenerating the board (generated; edit `gen/*.py` only).
- `docs/firmware.md`: how the firmware intercepts and resumes. All behaviour is in
  `firmware/components/ervlink/` (plain C++); `firmware/ervlink-package.yaml` is the ESPHome
  package and `ervlink.yaml` the installed device's config.
- `captures/YYYY-MM-DD/`: captures with timestamped notes; the source for every protocol claim.

## Commands

```bash
cd firmware && mise run test      # host tests with a simulated ERV/wall control/board
cd firmware && mise run compile   # ESP32-C6 build against the checkout
./tools/decode.py --edges captures/2025-12-14/derived --start 6.8 --changes
```

## Rules

- Change firmware test-first: add a scenario in `firmware/test/` and check it fails without the fix.
- Releases are tags `fw-vNN`; `main` must never reference a tag that isn't pushed yet (bump on a
  branch, CI, tag, then fast-forward `main`). Keep entity names stable: HA's IDs follow them.
- A decode below 100 % on BCC/parity/stop means wrong parameters, not a noisy bus.
- New captures: analog export, and a timestamp for every button press.
