#!/usr/bin/env python3
"""Decode the Panasonic Intelli-Balance 200 <-> FV-SW20VEC1 wall control link.

Pipeline:  Saleae analog CSV  ->  auto mid-swing threshold  ->  inverted 8E1 UART
           @4800 baud  ->  STX/ETX framing  ->  BCC-checked register blocks

Why re-threshold from analog rather than using Saleae's digital export: the two
lines have very different active levels (RXD reaches ~4.7 V, TXD only ~2.3 V), so
no single digital threshold suits both. Edges are also RC-limited (~80 us slew on
a 208 us bit), so an off-centre threshold introduces a duty-cycle skew that shifts
the sampling points and corrupts the decode. Thresholds are measured per channel
from the capture itself, so a fresh capture needs no code changes. See CLAUDE.md.

Usage
-----
  # full run from a fresh analog export, caching the edges for later
  ./tools/decode.py captures/2026-07-26/analog.csv --save-edges captures/2026-07-26/derived

  # replay from cached edges (instant)
  ./tools/decode.py --edges captures/2026-07-26/derived

  # only frames whose register value changed -- the useful view
  ./tools/decode.py --edges captures/2026-07-26/derived --changes

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
# 7769/7769 bytes pass both parity and stop-bit checks. The decode stays clean
# across roughly 204-215 us, so this needs no per-capture tuning.
BIT_SECONDS = 208.4e-6
DATA_BITS = 8

# The link labels are from the *unit's* point of view, so the line named RXD is
# the one the unit receives on: wall control -> unit.
LINES = ("RXD", "TXD")
DIRECTION = {"RXD": "panel->unit (command)", "TXD": "unit->panel (state)"}

LEVEL_SUBSAMPLE = 20      # every Nth sample is enough to measure the rails
RAIL_PERCENTILE = 0.005   # trim this fraction off each end when finding rails
HYSTERESIS = 0.10         # +/- this fraction of the swing around mid-point


def detect_columns(header):
    """Map line name -> CSV column index using the Saleae channel names."""
    fields = [f.strip().strip('"') for f in header.split(",")]
    cols = {}
    for name in LINES:
        for i, f in enumerate(fields):
            if i > 0 and name.lower() in f.lower():
                cols[name] = i
                break
    if len(cols) != len(LINES):
        if len(fields) >= 3:
            cols = {"RXD": 1, "TXD": 2}
            print("note: channel names not found in header %r; assuming "
                  "column 1=RXD, 2=TXD" % header.strip(), file=sys.stderr)
        else:
            sys.exit("error: cannot find RXD/TXD columns in header: %r" % header.strip())
    return cols


def measure_levels(path, cols):
    """Subsampled pass to find each channel's low and high rail."""
    hist = {name: collections.Counter() for name in cols}
    with open(path) as fh:
        fh.readline()
        for n, line in enumerate(fh):
            if n % LEVEL_SUBSAMPLE:
                continue
            parts = line.split(",")
            if len(parts) <= max(cols.values()):
                continue
            for name, col in cols.items():
                hist[name][round(float(parts[col]), 2)] += 1

    levels = {}
    for name, h in hist.items():
        total = sum(h.values())
        if not total:
            sys.exit("error: no samples for %s" % name)
        keys = sorted(h)
        cut = total * RAIL_PERCENTILE
        acc = 0
        low = keys[0]
        for k in keys:
            acc += h[k]
            if acc >= cut:
                low = k
                break
        acc = 0
        high = keys[-1]
        for k in reversed(keys):
            acc += h[k]
            if acc >= cut:
                high = k
                break
        levels[name] = (low, high)
    return levels


def thresholds_from_levels(low, high):
    """Hysteresis band centred on mid-swing."""
    mid = (low + high) / 2.0
    band = (high - low) * HYSTERESIS
    return mid - band, mid + band


def extract_edges(path, cols, thresh):
    """Stream the analog CSV and return {name: [(t, level), ...]}."""
    edges = {name: [] for name in cols}
    state = {name: 0 for name in cols}
    maxcol = max(cols.values())
    with open(path) as fh:
        fh.readline()
        for line in fh:
            parts = line.split(",")
            if len(parts) <= maxcol:
                continue
            t = None
            for name, col in cols.items():
                lo, hi = thresh[name]
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


def measure_bit_time(edges):
    """Estimate bit time from the shortest recurring pulse in each polarity.

    A one-bit pulse is stretched in one polarity and squeezed in the other by the
    same amount, so averaging the two cancels the slew-induced skew.
    Returns (bit_us, skew_us) or None if the capture is too sparse.
    """
    widths = {0: collections.Counter(), 1: collections.Counter()}
    for i in range(len(edges) - 1):
        w = (edges[i + 1][0] - edges[i][0]) * 1e6
        if w < 2600:
            widths[edges[i][1]][int(round(w / 4.0)) * 4] += 1
    shortest = {}
    for level, h in widths.items():
        if not h:
            return None
        floor = max(h.values()) * 0.10   # ignore rare glitch-width bins
        candidates = [w for w, c in h.items() if c >= floor]
        if not candidates:
            return None
        shortest[level] = min(candidates)
    return (shortest[1] + shortest[0]) / 2.0, (shortest[1] - shortest[0]) / 2.0


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
    for name in LINES:
        path = os.path.join(dirpath, "edges-%s.csv" % name.lower())
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
        path = os.path.join(dirpath, "edges-%s.csv" % name.lower())
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
                    help="ignore data before this capture timestamp")
    ap.add_argument("--changes", action="store_true",
                    help="only print frames whose data changed for that address")
    ap.add_argument("--bit-us", type=float, default=BIT_SECONDS * 1e6,
                    help="bit time in us (default %.1f, ~4800 baud)" % (BIT_SECONDS * 1e6))
    args = ap.parse_args()

    if args.edges:
        edges = load_edges(args.edges)
    elif args.analog_csv:
        with open(args.analog_csv) as fh:
            header = fh.readline()
        if "Time" not in header:
            sys.exit("error: %s does not look like a Saleae analog CSV export"
                     % args.analog_csv)
        cols = detect_columns(header)
        levels = measure_levels(args.analog_csv, cols)
        thresh = {}
        for name, (low, high) in levels.items():
            thresh[name] = thresholds_from_levels(low, high)
            print("%s: rails %+.2f V .. %+.2f V (swing %.2f V) -> thresholds %.2f / %.2f V"
                  % (name, low, high, high - low, thresh[name][0], thresh[name][1]),
                  file=sys.stderr)
            if high - low < 1.0:
                print("  WARNING: swing under 1 V -- is this channel connected and active?",
                      file=sys.stderr)
        edges = extract_edges(args.analog_csv, cols, thresh)
    else:
        ap.error("give an analog CSV path or --edges DIR")

    if args.save_edges:
        save_edges(edges, args.save_edges)

    bit = args.bit_us * 1e-6
    rows = []
    clean = True
    for name in LINES:
        timing = measure_bit_time(edges[name])
        if timing:
            measured, skew = timing
            note = ""
            if abs(measured - args.bit_us) > 0.05 * args.bit_us:
                note = "  <-- differs >5%% from the %.1f us being used, consider --bit-us" % args.bit_us
                clean = False
            print("%s: measured bit time %.1f us (%.0f baud), slew skew %+.0f us%s"
                  % (name, measured, 1e6 / measured, skew, note), file=sys.stderr)

        stream = [b for b in decode_bytes(edges[name], bit=bit) if b[0] >= args.start]
        if not stream:
            print("%s: no bytes decoded" % name, file=sys.stderr)
            clean = False
            continue
        frames, skipped = parse_frames([(t, v) for t, v, _, _ in stream])
        good = sum(1 for f in frames if f[3])
        par = sum(1 for b in stream if b[2])
        stp = sum(1 for b in stream if b[3])
        print("%s  %-22s %d bytes (parity %d, stop %d)  %d frames (BCC ok %d)  %d unframed bytes"
              % (name, DIRECTION[name], len(stream), par, stp, len(frames), good, skipped),
              file=sys.stderr)
        if good != len(frames) or par != len(stream) or not frames:
            clean = False
        for t, payload, got, ok in frames:
            addr, data = render(payload)
            rows.append((t, name, addr, data, got, ok))

    if not clean:
        print("\nWARNING: this is not a clean decode. On a good capture every byte passes\n"
              "parity and every frame passes BCC. Suspect the capture or the parameters,\n"
              "not the bus -- check the measured rails and bit time above.\n", file=sys.stderr)

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
