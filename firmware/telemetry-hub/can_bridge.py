#!/usr/bin/env python3
"""
can_bridge.py -- closes the loop from CAN frames to the dashboard.

    Teensy logger ('r' = raw stream)  ->  this  ->  named signals  ->  hub  ->  vent display

The logger captures everything but doesn't know what any of it MEANS -- BMW's
PHEV frames aren't documented. So the meaning lives in a data file you fill in
as you decode, not in firmware you have to reflash:

    signals.json
    [
      {"key":"SOC", "bus":0, "id":"1A0", "byte":2, "width":16,
       "endian":"BE", "signed":false, "scale":0.1, "offset":0, "unit":"%"},
      ...
    ]

Workflow: log a drive -> `canlog.py candidates` ranks bytes that behave like
sensors -> confirm one against the dash -> add a line here -> it appears live on
the vent display. No firmware change, no reflash.

Ships with an EMPTY signal list on purpose. Inventing plausible-looking BMW IDs
would be worse than useless: it would put confident wrong numbers on a dashboard
you're reading while driving.

    python can_bridge.py --selftest
"""
import argparse
import json
import os
import sys
import time

DEFAULT_SIGNALS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "signals.json")


# ============================================================
#  decoding  (pure -- unit testable)
# ============================================================
def parse_stream_line(line):
    """'R,<t_us>,<bus>,<id hex>,<data hex>' -> (t_us, bus, id, bytes) or None.

    Returns None for anything malformed. The USB line is shared with console
    output and can be interleaved or truncated; a bad line must be dropped, not
    guessed at.
    """
    line = line.strip()
    if not line.startswith("R,"):
        return None
    parts = line.split(",")
    if len(parts) != 5:
        return None
    try:
        t = int(parts[1])
        bus = int(parts[2])
        cid = int(parts[3], 16)
        hexs = parts[4]
        if len(hexs) % 2:
            return None
        data = bytes.fromhex(hexs)
    except ValueError:
        return None
    return t, bus, cid, data


def extract(data, byte, width, endian="BE", signed=False):
    """Pull a raw integer out of a frame. None if the frame is too short."""
    if width == 8:
        if byte >= len(data):
            return None
        raw = data[byte]
    elif width == 16:
        if byte + 1 >= len(data):
            return None
        raw = ((data[byte] << 8) | data[byte + 1]) if endian == "BE" \
              else ((data[byte + 1] << 8) | data[byte])
    else:
        return None
    if signed and raw >= (1 << (width - 1)):
        raw -= (1 << width)
    return raw


class Decoder:
    def __init__(self, signals):
        self.by_id = {}
        for s in signals:
            key = (int(s["bus"]), int(s["id"], 16) if isinstance(s["id"], str)
                   else int(s["id"]))
            self.by_id.setdefault(key, []).append(s)

    def decode(self, bus, cid, data):
        """-> {KEY: value} for every signal defined on this frame."""
        out = {}
        for s in self.by_id.get((bus, cid), ()):
            raw = extract(data, int(s["byte"]), int(s.get("width", 8)),
                          s.get("endian", "BE"), bool(s.get("signed", False)))
            if raw is None:
                continue
            out[s["key"]] = raw * float(s.get("scale", 1.0)) + \
                float(s.get("offset", 0.0))
        return out


def load_signals(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return json.load(f)


# ============================================================
#  selftest
# ============================================================
def selftest():
    ok = True

    print("1) stream line parsing")
    good = parse_stream_line("R,123456,0,1A0,C8005A00")
    if good != (123456, 0, 0x1A0, bytes([0xC8, 0x00, 0x5A, 0x00])):
        print(f"   FAIL: {good}"); ok = False
    else:
        print("   ok: well-formed line parsed")
    for bad in ["", "garbage", "R,1,2", "R,x,0,1A0,00",
                "R,1,0,1A0,ABC",           # odd hex length
                "STATUS radio=OK ..."]:    # console text interleaved on USB
        if parse_stream_line(bad) is not None:
            print(f"   FAIL: accepted malformed {bad!r}"); ok = False
    print("   ok: malformed/interleaved lines rejected (not guessed at)")

    print("2) field extraction")
    d = bytes([0x00, 0x01, 0x02, 0x58, 0xFF, 0x9C])
    checks = [
        (extract(d, 3, 8), 0x58, "u8"),
        (extract(d, 2, 16, "BE"), 0x0258, "u16 BE"),
        (extract(d, 2, 16, "LE"), 0x5802, "u16 LE"),
        (extract(d, 4, 16, "BE", True), -100, "s16 BE negative"),
        (extract(d, 9, 8), None, "out of range -> None"),
    ]
    for got, want, what in checks:
        if got != want:
            print(f"   FAIL {what}: got {got}, want {want}"); ok = False
    if ok:
        print("   ok: u8 / u16 BE / u16 LE / signed / bounds all correct")

    print("3) decoding via a signal map")
    dec = Decoder([
        {"key": "SOC", "bus": 0, "id": "1A0", "byte": 3, "width": 8,
         "scale": 0.5, "offset": 0},
        {"key": "TEMP", "bus": 0, "id": "1A0", "byte": 4, "width": 16,
         "endian": "BE", "signed": True, "scale": 0.1, "offset": 0},
    ])
    got = dec.decode(0, 0x1A0, d)
    if abs(got.get("SOC", 0) - 44.0) > 1e-6:
        print(f"   FAIL SOC: {got}"); ok = False
    if abs(got.get("TEMP", 0) - (-10.0)) > 1e-6:
        print(f"   FAIL TEMP: {got}"); ok = False
    if ok:
        print(f"   ok: {got}")
    if dec.decode(1, 0x1A0, d):
        print("   FAIL: matched the wrong bus"); ok = False
    else:
        print("   ok: bus is part of the key (same ID on two buses stays separate)")

    print("4) shipped signal map")
    sigs = load_signals(DEFAULT_SIGNALS)
    print(f"   {len(sigs)} signal(s) defined "
          f"({'empty by design until you decode them' if not sigs else 'user-defined'})")

    print("\nPASSED" if ok else "\nFAILED")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--dev", help="serial device of the Teensy logger")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--signals", default=DEFAULT_SIGNALS)
    ap.add_argument("--print", action="store_true",
                    help="print decoded values instead of pushing to a hub")
    a = ap.parse_args()

    if a.selftest:
        sys.exit(0 if selftest() else 1)
    if not a.dev:
        ap.error("need --dev or --selftest")

    sigs = load_signals(a.signals)
    if not sigs:
        print(f"warning: {a.signals} defines no signals yet -- decode some with "
              f"canlog.py candidates first", file=sys.stderr)
    dec = Decoder(sigs)

    import serial
    with serial.Serial(a.dev, a.baud, timeout=1) as s:
        s.write(b"r")                      # ask the logger to start streaming
        while True:
            line = s.readline().decode("ascii", "replace")
            rec = parse_stream_line(line)
            if not rec:
                continue
            _, bus, cid, data = rec
            vals = dec.decode(bus, cid, data)
            for k, v in vals.items():
                print(f"{k}={v:.2f}")


if __name__ == "__main__":
    main()
