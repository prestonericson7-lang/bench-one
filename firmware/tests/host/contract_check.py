#!/usr/bin/env python3
"""
contract_check.py -- both ends of every wire, checked against each other.

The host firmware tests (build_and_run.sh) leave behind exactly what each REAL firmware sent:
  out/can_logger_usb.txt    the CAN logger's USB output (banner, STATUS, R lines, replies)
  out/CAN_0001.BIN          the file it wrote to the SD card
  out/climate_node_usb.txt  the climate node's USB output
This script feeds those bytes through the REAL Pi-side parsers -- can_bridge.parse_stream_line,
hub.parse_kv_line, canlog.read_log -- and fails if any machine line is rejected, any prose line would
become a key, or the SD file does not decode to the frames that were injected.

    python contract_check.py        (after build_and_run.sh)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
FW = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path[:0] = [os.path.join(FW, "telemetry-hub"), os.path.join(FW, "car-can-logger")]
import can_bridge  # noqa: E402
import canlog      # noqa: E402
import hub         # noqa: E402

ok = True


def fail(msg):
    global ok
    ok = False
    print(f"  FAIL  {msg}")


def lines(name):
    return open(os.path.join(OUT, name), "rb").read().decode("ascii", "replace").replace("\r", "").split("\n")


print("1) CAN logger USB output -> can_bridge.parse_stream_line")
L = lines("can_logger_usb.txt")
r_lines = [ln for ln in L if ln.startswith("R,")]
parsed = [can_bridge.parse_stream_line(ln) for ln in r_lines]
if not r_lines or any(p is None for p in parsed):
    fail(f"{sum(p is None for p in parsed)} of {len(r_lines)} R lines rejected")
else:
    frames = {(b, i, d.hex().upper()) for _, b, i, d in parsed}
    want = {(1, 0x1A0, "C8005A00"), (2, 0x18DAF110, "021003"), (1, 0x7, ""), (1, 0x100, "01"), (1, 0x101, "02")}
    if not want <= frames:
        fail(f"missing frames {want - frames}")
    else:
        print(f"  ok    {len(r_lines)} R lines, all parsed; bus/id/data match the injected frames")
    ts = [t for t, *_ in parsed]
    if ts != sorted(ts):
        fail("timestamps not monotonic")
    else:
        print("  ok    timestamps monotonic")
stray = [ln for ln in L if ln and not ln.startswith("R,") and can_bridge.parse_stream_line(ln) is not None]
if stray:
    fail(f"non-frame lines accepted as frames: {stray[:3]}")
else:
    print(f"  ok    {sum(1 for ln in L if ln and not ln.startswith('R,'))} console lines (banner, STATUS, replies) never taken for frames")
kv_from_logger = [ln for ln in L if hub.parse_kv_line(ln)]
if [ln for ln in kv_from_logger if not ln.startswith("ID=")]:
    fail(f"logger console lines would create hub keys: {kv_from_logger[:4]}")
else:
    print("  ok    the logger's console chatter creates no hub keys")

print("2) CAN logger SD file -> canlog.read_log")
recs = list(canlog.read_log(os.path.join(OUT, "CAN_0001.BIN")))
hdr, body = recs[0], recs[1:]
if hdr != ("HEADER", 500000, 100000):
    fail(f"header {hdr}")
else:
    print("  ok    header: bus1 500000, bus2 100000")
got = {(b, i, e, d.hex().upper()) for _, b, i, e, _, d in body}
want = {(1, 0x1A0, False, "C8005A00"), (2, 0x18DAF110, True, "021003"), (1, 0x7, False, "")}
if not want <= got:
    fail(f"SD records missing {want - got}")
else:
    print(f"  ok    {len(body)} records decoded, extended flag and data intact")

print("3) climate node USB output -> hub.parse_kv_line")
L = lines("climate_node_usb.txt")
keys, prose = {}, []
for ln in L:
    if not ln:
        continue
    kv = hub.parse_kv_line(ln)
    if kv:
        keys.setdefault(kv[0], []).append(kv[1])
    elif not ln.startswith("#") and not ln.endswith("=nan"):
        prose.append(ln)
need = {"AIRT", "CASET", "DAMPER", "CLIMATE_FAULT", "CLIMATE_MODE", "ID"}
if not need <= set(keys):
    fail(f"missing keys {need - set(keys)}")
else:
    print(f"  ok    keys {sorted(keys)}")
if prose:
    fail(f"lines that are neither KEY=value nor '#': {prose[:4]}")
else:
    print("  ok    every other line is '#' prose or an explicit nan")
if set(keys.get("DAMPER", [])) - {"OPEN", "CLOSED"}:
    fail(f"DAMPER values {set(keys['DAMPER'])}")
nan_lines = [ln for ln in L if ln.endswith("=nan")]
if nan_lines and any(hub.parse_kv_line(ln) for ln in nan_lines):
    fail("a nan reading was accepted as a value")
else:
    print(f"  ok    {len(nan_lines)} nan reading(s) withheld by the hub, never shown as a number")

print("\nCONTRACT CHECK PASSED" if ok else "\nCONTRACT CHECK FAILED")
sys.exit(0 if ok else 1)
