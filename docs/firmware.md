# Firmware

## Layout

| | |
|---|---|
| `firmware/components/ervlink/ervlink_core.{h,cpp}` | All the behaviour, in plain C++ with no ESPHome dependencies: decoding, controls, intercept timing, outage resume. |
| `firmware/ervlink-package.yaml` | ESPHome package: board, UARTs, relay, entities, and the wiring of the core's hooks (bytes in; entities, relay, UART writes and flash out). |
| `firmware/ervlink.yaml`, `example-wifi.yaml` | Device configs: name, API key, network, and the package line. |
| `firmware/test/` | Host tests: a byte-timed simulator of the ERV, wall control and board, plus a replay of the 2025-12-14 capture. |

`mise run test` runs the tests; `mise run compile [example-wifi.yaml]` builds for the ESP32-C6
against the checkout. CI runs both. Entity IDs in HA follow the entity names in the package.

## Intercepting

The board normally listens, with the relay in passthrough. To send a command it takes over the
link briefly: it waits for a gap in **both** lines' traffic, switches the relay, sends the wall
control's frames itself for 5 cycles (3 with the command, 2 idle), and switches back in the next
gap. About 1.4 s in all.

- Nothing is forwarded. During the intercept the ERV hears only the board and the wall control
  hears nothing; the panel shows the result afterwards from the ERV's state.
- The relay must switch inside a gap (~51 ms on RXD, ~81 ms on TXD) with a 20 ms margin. As the
  two cycles drift, a shared gap can be missing for minutes; after 30 s the board settles for an
  RXD gap (the wall control may then see one cut-off frame, which fails BCC).
- Frame ends are timestamped when the loop sees them, so a slow loop makes them look later than
  they were; the gap calculation allows for the time since the previous loop.
- The relay is never held for more than 5 s.
- Requests the ERV can't take are refused and the control snaps back: anything before the ERV's
  state is known, Boost while off, airflow while boosting. Airflow and ASHRAE changes while off
  only change what Power on will send.

## Resuming after a power cut

The ERV comes back off after an outage. If it was on before, the board turns it back on.

- **Spotting the power-up:** the wall control runs on the ERV's 5 V, so it reboots with it. The
  board treats as a power-up either the wall control's `FFFFFFFF` burst on any RXD register, or
  RXD silent for over 0.5 s while the ERV is talking (the wall control boots for ~2.1 s). If the
  board itself just powered up and started listening too late to see that, its own power-on
  reset counts.
- **"Should be on"** is the ERV's last reported power state, saved to flash. It is only learned
  once the wall control has been heard, so a board that boots with the ERV doesn't record the
  outage's "off" as a choice.
- **The resume:** 10 s after the power-up, up to 3 tries 30 s apart, each a Power on with the
  saved airflow and ASHRAE. It ends as soon as the ERV reports on, on a Power off, if the
  "Resume after power failure" switch is off, after 2 min without state from the ERV, or after
  5 min; then the ERV's actual state is recorded.
- The tries made are saved to flash, so a board reset mid-resume (say, a brownout as the relay
  switches) carries on rather than starting over.
- If the ERV's state line goes silent for 10 s, the board reports it off and disconnected, so
  "off for 10 minutes" alerts still fire during an outage.

## Other behaviour

- The airflow and ASHRAE settings are saved for Power on once the ERV has run steadily for 5 s
  without boost (boost's 200/200 is never saved, nor the rest's `FE`).
- Temperatures and humidity are blank unless air has moved for a minute: while off or resting,
  the sensors read the air inside the unit.
- Flash is written only while the relay is out.
- Junk bytes counted at boot (around 10) are the partial frame the board catches when it starts
  listening; the counter is a total since boot.

## Installing

Device configs pull the package from GitHub (`@main`, or `@fw-vNN` to pin a release), and the
package loads the core at its own release tag. The Device Builder's first fetch of a new tag can
fail with `fetch-pack: invalid index-pack output` (two of its processes cloning at once):
choose "Save anyway", then Install.
