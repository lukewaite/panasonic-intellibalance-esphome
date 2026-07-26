#!/usr/bin/env python3
"""Decode the Panasonic Intelli-Balance 200 <-> FV-SW20VEC1 wall control link.

Pipeline:  Saleae analog CSV  ->  mid-swing re-threshold  ->  inverted 8E1 UART
           @4800 baud  ->  STX/ETX framing  ->  BCC-checked register blocks

Why re-threshold from analog instead of using Saleae's digital export: the line
edges are RC-limited (~80 us slew on a 208 us bit). Thresholds that are not at
true mid-swing introduce a large duty-cycle skew, which shifts the sampling
points and corrupts the decode. See CLAUDE.md.

Usage
-----
  # full run from the analog export (~20 s for a 645 MB / 26.9 M sample file)
  ./tools/decode.py captures/2025-12-14/analog.csv

  # cache the extracted edges so later runs are instant
  ./tools/decode.py captures/2025-12-14/analog.csv --save-edges captures/2025-12-14/derived
  ./tools/decode.py --edges captures/2025-12-14/derived

  # only frames whose payload changed from the previous frame at that address
  ./tools/decode.py --edges captures/2025-12-14/derived --changes

Requires only the standard library; numpy is deliberately not used.
"""

import argparse
import bisect
import collections
import csv
import os
import sys

STX, ETX = 0x02, 0x03

# Verified against the 2025-12-14 capture: 596/596 frames pass BCC and
# 7769/7769 bytes pass both parity and stop-bit checks.
BIT_SECONDS = 208.4e-6
DATA_BITS = 8

# Per-channel analog thresholds, placed at mid-swing with hysteresis.
# The two lines have very different active levels: RXD reaches 4.7 V, TXD only 2.3 V.
CHANNELS = [
    # name  csv_col  falling_thresh  rising_thresh  residual skew correction (us)
    ("rxd", 1, 1.90, 2.80, -16.0, +16.0),
    ("txd", 2, 0.90, 1.40, +18.0, -18.0),
]

# The link labels are from the *unit's* point of view, so the line named RXD is
# the one the unit receives on: wall control -> unit.
DIRECTION = {"rxd": "panel->unit (command)", "txd": "unit->panel (state)"}


def extract_edges(analog_path):
    """Stream the analog CSV and return {name: [(t, level), ...]} at mid-swing."""
    edges = {name: [] for name, *_ in CHANNELS}
    state = {name: 0 for name, *_ in CHANNELS}
    with open(analog_path) as fh:
        header = fh.readline()
        if "Time" not in header:
            sys.exit("error: %s does not look like a Saleae analog CSV export" % analog_path)
        for line in fh:
            parts = line.split(",")
            if len(parts) < 3:
                continue
            t = None
            for name, col, lo, hi, _, _ in CHANNELS:
                v = float(parts[col])
                if state[name] == 0 and v > hi:
                    state[name] = 1
                elif state[name] == 1 and v < lo:
                    state[name] = 0
                else:
                    continue
                if t is None:
                    t = float(parts[0])
                edges[name].append((t, state[name]))
    return edges


def apply_skew(edges, rise_us, fall_us):
    """Nudge edges to cancel the residual duty-cycle skew left by slow slew."""
    out = [(t + (rise_us if v == 1 else fall_us) * 1e-6, v) for t, v in edges]
    out.sort()
    return out


class Waveform:
    """Level lookup over a sorted edge list."""

    def __init__(self, edges):
        self.times = [t for t, _ in edges]
        self.levels = [v for _, v in edges]

    def at(self, t):
        i = bisect.bisect_right(self.times, t) - 1
        if i < 0:
            return self.levels[0] ^ 1  # before the first edge
        return self.levels[i]


def decode_bytes(edges, bit=BIT_SECONDS):
    """Inverted 8E1 UART. Idle is 0 V, so the start bit is a *rising* edge.

    Returns [(t, value, parity_ok, stop_ok), ...].
    """
    wf = Waveform(edges)
    logical = lambda t: wf.at(t) ^ 1  # noqa: E731 - inverted line
    out = []
    busy_until = -1.0
    for t, v in edges:
        if (v ^ 1) != 0 or t < busy_until:
            continue  # not a logical start bit, or mid-byte
        if logical(t + 0.5 * bit) != 0:
            continue  # start bit did not hold; glitch
        value = 0
        for k in range(DATA_BITS):
            value |= logical(t + (1.5 + k) * bit) << k
        parity = logical(t + (1.5 + DATA_BITS) * bit)
        stop = logical(t + (2.5 + DATA_BITS) * bit)
        parity_ok = (bin(value).count("1") + parity) % 2 == 0  # even parity
        out.append((t, value, parity_ok, stop == 1))
        busy_until = t + (2.0 + DATA_BITS) * bit
    return out


def bcc(payload):
    """Block check character: complement of XOR over payload bytes plus ETX."""
    x = 0
    for b in payload:
        x ^= b
    return (~(x ^ ETX)) & 0xFF


def parse_frames(byte_stream):
    """STX | 2 ASCII-hex address chars | 8 ASCII-hex data chars | ETX | BCC."""
    frames = []
    skipped = 0
    i, n = 0, len(byte_stream)
    while i < n:
        if byte_stream[i][1] != STX:
            skipped += 1
            i += 1
            continue
        j = i + 1
        while j < n and byte_stream[j][1] != ETX and j - i <= 12:
            j += 1
        if j >= n or byte_stream[j][1] != ETX or j + 1 >= n:
            skipped += 1
            i += 1
            continue
        payload = [b for _, b in byte_stream[i + 1:j]]
        got = byte_stream[j + 1][1]
        frames.append((byte_stream[i][0], payload, got, bcc(payload) == got))
        i = j + 2
    return frames, skipped


def render(payload):
    """-> (address, data) as ASCII, e.g. ('B1', '41D13055')."""
    text = bytes(payload).decode("ascii", "replace")
    return text[:2], text[2:]


def load_edges(dirpath):
    edges = {}
    for name, *_ in CHANNELS:
        path = os.path.join(dirpath, "edges-%s.csv" % name)
        if not os.path.exists(path):
            sys.exit("error: %s not found" % path)
        rows = []
        with open(path) as fh:
            r = csv.reader(fh)
            next(r)
            for t, v in r:
                rows.append((float(t), int(v)))
        edges[name] = rows
    return edges


def save_edges(edges, dirpath):
    os.makedirs(dirpath, exist_ok=True)
    for name, rows in edges.items():
        path = os.path.join(dirpath, "edges-%s.csv" % name)
        with open(path, "w") as fh:
            fh.write("time_s,level\n")
            for t, v in rows:
                fh.write("%.9f,%d\n" % (t, v))
        print("wrote %s (%d edges)" % (path, len(rows)), file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("analog_csv", nargs="?", help="Saleae analog CSV export")
    ap.add_argument("--edges", metavar="DIR", help="load cached edges from DIR instead")
    ap.add_argument("--save-edges", metavar="DIR", help="write extracted edges to DIR")
    ap.add_argument("--start", type=float, default=0.0,
                    help="ignore data before this capture timestamp (default 0)")
    ap.add_argument("--changes", action="store_true",
                    help="only print frames whose data changed for that address")
    args = ap.parse_args()

    if args.edges:
        edges = load_edges(args.edges)
    elif args.analog_csv:
        edges = extract_edges(args.analog_csv)
    else:
        ap.error("give an analog CSV path or --edges DIR")

    if args.save_edges:
        save_edges(edges, args.save_edges)

    rows = []
    for name, _, _, _, rise, fall in CHANNELS:
        stream = decode_bytes(apply_skew(edges[name], rise, fall))
        stream = [b for b in stream if b[0] >= args.start]
        if not stream:
            print("%s: no bytes decoded" % name.upper(), file=sys.stderr)
            continue
        frames, skipped = parse_frames([(t, v) for t, v, _, _ in stream])
        good = sum(1 for f in frames if f[3])
        par = sum(1 for b in stream if b[2])
        stp = sum(1 for b in stream if b[3])
        print("%s  %-22s %d bytes (parity %d, stop %d)  %d frames (BCC ok %d)  %d unframed bytes"
              % (name.upper(), DIRECTION[name], len(stream), par, stp,
                 len(frames), good, skipped), file=sys.stderr)
        if good != len(frames) or par != len(stream):
            print("  WARNING: not a clean decode -- check thresholds and bit time",
                  file=sys.stderr)
        for t, payload, got, ok in frames:
            addr, data = render(payload)
            rows.append((t, name.upper(), addr, data, got, ok))

    rows.sort()
    last = {}
    print("time_s     line addr data      bcc  ok")
    for t, line, addr, data, got, ok in rows:
        key = (line, addr)
        if args.changes and last.get(key) == data:
            continue
        changed = "  <-- changed" if key in last and last[key] != data else ""
        last[key] = data
        print("%10.6f %-4s %-4s %-9s %02X   %s%s"
              % (t, line, addr, data, got, "ok" if ok else "BAD", changed))

    counts = collections.Counter((r[1], r[2]) for r in rows)
    print("\nblocks per address: %s"
          % ", ".join("%s/%s=%d" % (l, a, n) for (l, a), n in sorted(counts.items())),
          file=sys.stderr)


if __name__ == "__main__":
    main()
