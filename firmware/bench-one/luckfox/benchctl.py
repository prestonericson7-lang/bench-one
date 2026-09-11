#!/usr/bin/env python3
"""
benchctl.py -- the BENCH ONE command line, for the Luckfox.

    ./benchctl.py sys                        whole-stack health in one round trip
    ./benchctl.py scan                       what is on the I2C bus
    ./benchctl.py ports                      the port map, read FROM the hardware
    ./benchctl.py selftest [0|1|2]           the L0-L7 ladder
    ./benchctl.py ping [count]               link round trip, this clock only
    ./benchctl.py spi P3 0F 00               raw SPI to a named port
    ./benchctl.py i2c 0x76 --write D0 --read 1
    ./benchctl.py adc [leaf] [channel]
    ./benchctl.py sweep [first] [count]      all 64 analog channels
    ./benchctl.py shiftout AA 55             drive the 74HC595 chain
    ./benchctl.py shiftin [chips]            snapshot the 74HC165 chain
    ./benchctl.py watch                      stream unsolicited events
    ./benchctl.py mark [pulses]              pulse the analyser marker
    ./benchctl.py sync                       clock offset, with its uncertainty
    ./benchctl.py bench [job] [n]            run a worker job and time it

Every number this prints is either read from the hardware or measured on one clock. Where a
value could not be verified, it says so rather than filling in a plausible one.
"""

import argparse
import sys
import time

import iop
from bench import Bench, BenchError, BenchStatus


def hexbytes(items):
    """Parse '0F', '0x0f' or '15' into bytes. Accepts a run of them."""
    out = bytearray()
    for it in items:
        s = str(it)
        base = 16 if (s.lower().startswith("0x") or any(c in "abcdefABCDEF" for c in s)) else 16
        out.append(int(s, base) & 0xFF)
    return bytes(out)


def cmd_sys(b, a):
    s = b.sys()
    print("links mask 0x%02X" % s["links_mask"])
    print("  %-9s %-6s %-12s %s" % ("node", "state", "uptime", "errors"))
    for n in s["nodes"]:
        print("  %-9s %-6s %-12s %d" % (
            iop.NODE_NAMES.get(n["node"], "0x%02X" % n["node"]),
            "UP" if n["up"] else "down",
            "%.1f s" % (n["uptime_ms"] / 1000.0),
            n["errors"]))
    i = b.info()
    print("\nfabric hw rev %d, protocol v%d, bench v%d, %d ports"
          % (i["hw_rev"], i["proto"], i["bench_proto"], i["ports"]))
    print("  spi %d   i2c %d   i2c NACKs %d   irq %d   irq storms %d   faults %d"
          % (i["spi_transfers"], i["i2c_transactions"], i["i2c_nacks"],
             i["irq_events"], i["irq_storms"], i["faults"]))
    if i["irq_storms"]:
        print("  WARNING: irq storms are non-zero. The shared interrupt line stayed low after a")
        print("           full service. Check that every expander has MIRROR and ODR set, and")
        print("           that the 4.7k pull-up to 3V3 is fitted.")


def cmd_scan(b, a):
    found = b.scan(a.bus)
    if not found:
        print("nothing answered on bus %d." % a.bus)
        print("Check, in this order:")
        print("  1. 3.3 V present at the module")
        print("  2. pull-ups fitted ONCE at the master, 2.2k -- not one per module")
        print("  3. address straps hard-tied, never floating")
        print("  4. SDA/SCL not swapped")
        return
    print("%d device%s on bus %d:" % (len(found), "" if len(found) == 1 else "s", a.bus))
    for addr in found:
        note = ""
        if 0x20 <= addr <= 0x27:
            note = "  MCP23017 range (also PCF8574 and many I2C radios -- keep this block)"
        elif 0x40 <= addr <= 0x4F:
            note = "  INA219 range"
        elif addr == 0x70:
            note = "  TCA9548A mux default"
        print("  0x%02X%s" % (addr, note))


def cmd_ports(b, a):
    ports = b.ports(refresh=True)
    print("%-5s %-5s %-10s %-9s %-7s %-8s %s"
          % ("name", "id", "kind", "bus/slot", "bits", "present", "flags"))
    for p in ports:
        name = iop.PORT_NAMES.get(p["id"], "?")
        flags = []
        if p["flags"] & 0x0001: flags.append("present")
        if p["flags"] & 0x0002: flags.append("hotplug")
        if p["flags"] & 0x0004: flags.append("irq")
        if p["flags"] & 0x0020: flags.append("sink-only")
        if p["flags"] & 0x0040: flags.append("RESERVED")
        print("%-5s 0x%02X  %-10s %d/0x%02X    %d+%-4d %-8s %s"
              % (name, p["id"], p["kind_name"], p["bus"], p["slot"],
                 p["first"], p["width"],
                 "yes" if (p["flags"] & 0x0001) else "-",
                 ",".join(flags)))
    print("\nSPI, shift and analog ports cannot be probed: a 74HC595 acknowledges nothing, so a")
    print("write into an empty socket genuinely succeeds. Only I2C-backed ports report presence.")


def cmd_selftest(b, a):
    results = b.selftest(a.level)
    npass = nfail = nskip = 0
    print("self-test level %d" % a.level)
    for r in results:
        print("  %-16s %-5s  0x%04X" % (r["name"], r["result_name"], r["detail"]))
        if r["result"] == 0: npass += 1
        elif r["result"] == 1: nfail += 1
        else: nskip += 1
    print("\n%d passed, %d FAILED, %d skipped" % (npass, nfail, nskip))
    if nskip:
        print("\n'skipped' is NOT 'passed'. CS_EXCLUSIVE and CS_DESELECT cannot be read back")
        print("from inside the Teensy -- nothing there can see the 74HC138's outputs. Capture")
        print("them on the analyser: the marker pin pulses once before each address is set and")
        print("three times at the end of the walk.")
    if a.level < 2:
        print("Level 0 is non-invasive. Level 1 adds loopbacks (needs the two jumpers).")
        print("Level 2 drives outputs -- do not run it with modules attached.")


def cmd_ping(b, a):
    times = []
    for _ in range(a.count):
        try:
            times.append(b.ping())
        except BenchError as e:
            print("  timeout: %s" % e)
        time.sleep(0.005)
    if not times:
        print("no replies.")
        return
    times.sort()
    print("%d/%d replies" % (len(times), a.count))
    print("  min  %.3f ms" % times[0])
    print("  med  %.3f ms" % times[len(times) // 2])
    print("  max  %.3f ms" % times[-1])
    print("\nMeasured entirely on this host's clock: t_send and t_recv are both taken here, so")
    print("no figure above compares two free-running crystals.")


def cmd_spi(b, a):
    tx = hexbytes(a.data)
    rx = b.spi_xfer(a.port, tx, mode=a.mode, khz=a.khz)
    print("tx %s" % " ".join("%02X" % c for c in tx))
    print("rx %s" % " ".join("%02X" % c for c in rx))


def cmd_i2c(b, a):
    wr = hexbytes(a.write) if a.write else b""
    rd = b.i2c_xfer(int(a.addr, 0), wr, a.read, a.bus)
    if wr:
        print("wr %s" % " ".join("%02X" % c for c in wr))
    if rd:
        print("rd %s" % " ".join("%02X" % c for c in rd))
    else:
        print("ok")


def cmd_adc(b, a):
    v = b.adc(a.leaf, a.channel, a.samples)
    volts = v * 3.3 / 4095.0
    print("leaf %d ch %d = %d counts  (%.4f V at 3.300 V reference)" % (a.leaf, a.channel, v, volts))
    print("First conversion after the mux switched was discarded by firmware, always.")


def cmd_sweep(b, a):
    vals = b.adc_sweep(a.first, a.count, a.samples)
    for i, v in enumerate(vals):
        ch = a.first + i
        bar = "#" * int(v * 40 / 4095)
        print("  ch %2d  leaf %d.%d  %4d  %.3f V  %s"
              % (ch, ch >> 3, ch & 7, v, v * 3.3 / 4095.0, bar))


def cmd_shiftout(b, a):
    data = hexbytes(a.data)
    b.shift_out(data)
    print("latched %d byte%s: %s" % (len(data), "" if len(data) == 1 else "s",
                                     " ".join("%02X" % c for c in data)))
    print("data[0] is the chip NEAREST the Teensy. Firmware reverses the chain order for you.")


def cmd_shiftin(b, a):
    data = b.shift_in(a.chips)
    print(" ".join("%02X" % c for c in data))
    for i, byte in enumerate(data):
        bits = "".join("1" if byte & (1 << (7 - k)) else "0" for k in range(8))
        print("  chip %d  %s   (bit7 = pin 6 'H', bit0 = pin 11 'A')" % (i, bits))


def cmd_watch(b, a):
    mask, div = b.subscribe(iop.EVMASK_ALL, a.divisor)
    print("subscribed: mask 0x%08X divisor %d. Ctrl-C to stop.\n" % (mask, div))
    try:
        while True:
            for f in b.poll_events():
                if f.chan == iop.CHAN_ORCH and f.cmd == iop.CMD_ORC_EVENT and len(f.payload) >= 3:
                    src, ch, cmd = f.payload[0], f.payload[1], f.payload[2]
                    body = f.payload[3:]
                    print("%8.3f  %-8s %-8s %-20s %s"
                          % (time.monotonic() % 1000,
                             iop.NODE_NAMES.get(src, "0x%02X" % src),
                             iop.CHAN_NAMES.get(ch, "0x%02X" % ch),
                             iop.CMD_NAMES.get(cmd, "0x%02X" % cmd),
                             " ".join("%02X" % c for c in body[:24])))
                    if cmd == iop.CMD_FAB_FAULT_EVENT and len(body) >= 2:
                        print("          FAULT %s on port %s"
                              % (iop.FAULT_NAMES.get(body[0], "0x%02X" % body[0]),
                                 iop.PORT_NAMES.get(body[1], "0x%02X" % body[1])))
                else:
                    print("%8.3f  %s" % (time.monotonic() % 1000, f))
            time.sleep(0.005)
    except KeyboardInterrupt:
        b.subscribe(iop.EVMASK_NONE, 1)
        print("\nunsubscribed.")


def cmd_mark(b, a):
    us = b.mark(1, a.pulses)
    print("%d pulse%s on the marker pin. Teensy1 micros() = %d"
          % (a.pulses, "" if a.pulses == 1 else "s", us))
    print("Arm the analyser on this pin's edge to centre a capture on an instant firmware chose.")


def cmd_sync(b, a):
    s = b.sync(a.samples)
    print("best round trip   %.3f ms  (of %d samples)" % (s["rtt_ms"], a.samples))
    print("device micros()   %d" % s["device_us"])
    print("host monotonic    %.6f s" % s["host_s"])
    print("uncertainty       +/- %.3f ms" % s["uncertainty_ms"])
    print("\nThe offset is DERIVED, not measured: the host times its own send and receive on its")
    print("own clock, the device reports its own timestamp on its clock, and the uncertainty is")
    print("half the round trip because the one-way delay cannot be split without more evidence.")


def cmd_reset(b, a):
    """Pulse a module's reset line. The CTL line lives on a 74HC595, so this is ~2 us per edge,
    not the ~120 us an I2C expander would cost."""
    b.port_reset(a.port, a.hold)
    print("%s reset: held low %d ms, released" % (a.port, a.hold))


def cmd_irq(b, a):
    v = b.port_irq(a.port)
    print("%s IRQ = %d  (%s)" % (a.port, v, "idle high" if v else "ASSERTED low"))
    print("Most modules pull this low to signal. For events rather than a snapshot, use 'watch'.")


def cmd_panel(b, a):
    """Walk the bring-up jig: LEDs, then inputs, then the loopback."""
    import time as _t
    print("shift-out walk on OB (8 LEDs, anode to 3V3 via 270R)")
    for bit in range(8):
        b.shift_out(bytes([0, 1 << bit, 0]))
        print("  bit %d" % bit)
        _t.sleep(0.15)
    b.shift_out(bytes([0, 0, 0]))

    print("\nshift-in snapshot on IA:")
    d = b.shift_in(1)
    bits = "".join("1" if d[0] & (1 << (7 - k)) else "0" for k in range(8))
    print("  %s   (bit7 = pin 6 'H', bit0 = pin 11 'A')" % bits)

    print("\nwalking-1 loopback OB.0 -> IA.0 (needs the jumper):")
    ok = 0
    for bit in range(8):
        b.shift_out(bytes([0, 1 << bit, 0]))
        _t.sleep(0.01)
        got = b.shift_in(1)[0]
        hit = (got == (1 << bit))
        ok += 1 if hit else 0
        print("  bit %d  sent %02X  got %02X  %s" % (bit, 1 << bit, got, "ok" if hit else "MISMATCH"))
    b.shift_out(bytes([0, 0, 0]))
    print("\n%d/8 bit positions verified." % ok)
    if ok < 8:
        print("A mismatch on EVERY bit means the jumper is missing or the MISO gate is not")
        print("enabled. A mismatch on SOME bits means a chain-order or bit-order error, which")
        print("looks exactly like mirrored wiring and is not.")


JOBS = {"nop": 0, "echo": 1, "crc16": 2, "sieve": 3, "float": 4, "membw": 5, "sort": 6}


def cmd_bench(b, a):
    if a.job not in JOBS:
        print("unknown job. Known: %s" % ", ".join(sorted(JOBS)))
        return
    b.subscribe(iop.EVMASK_WORKER_JOB, 1)
    st = b.job(JOBS[a.job], a.n)
    if st != iop.ST_OK:
        print("worker refused the job: %s" % iop.ST_NAMES.get(st, hex(st)))
        return
    print("job '%s' accepted, %d units. waiting..." % (a.job, a.n))
    r = b.wait_job(1, timeout=a.timeout)
    if r is None:
        print("no completion event within %.1f s." % a.timeout)
        print("The worker slices its jobs, so a long job is normal -- but a job that never")
        print("completes means the slice loop is stuck. Check 'sys' for the worker link.")
        return
    ns = r["ns"]
    print("state %d   units %d   result 0x%08X" % (r["state"], r["units"], r["result"]))
    print("compute time %.3f ms  (%.1f ns per unit)"
          % (ns / 1e6, (ns / r["units"]) if r["units"] else 0.0))
    print("\nThat time is cycles spent INSIDE the work on the worker's own clock, excluding the")
    print("link handling between slices. It is not wall clock and does not include transport.")


def main():
    ap = argparse.ArgumentParser(description="BENCH ONE control")
    ap.add_argument("--port", default="/dev/ttyS3")
    ap.add_argument("--baud", type=int, default=921600)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("sys").set_defaults(fn=cmd_sys)

    p = sub.add_parser("scan"); p.add_argument("bus", nargs="?", type=int, default=0)
    p.set_defaults(fn=cmd_scan)

    sub.add_parser("ports").set_defaults(fn=cmd_ports)

    p = sub.add_parser("selftest"); p.add_argument("level", nargs="?", type=int, default=0)
    p.set_defaults(fn=cmd_selftest)

    p = sub.add_parser("ping"); p.add_argument("count", nargs="?", type=int, default=100)
    p.set_defaults(fn=cmd_ping)

    p = sub.add_parser("spi")
    p.add_argument("port"); p.add_argument("data", nargs="+")
    p.add_argument("--mode", type=int, default=0); p.add_argument("--khz", type=int, default=1000)
    p.set_defaults(fn=cmd_spi)

    p = sub.add_parser("i2c")
    p.add_argument("addr"); p.add_argument("--write", nargs="*", default=[])
    p.add_argument("--read", type=int, default=0); p.add_argument("--bus", type=int, default=0)
    p.set_defaults(fn=cmd_i2c)

    p = sub.add_parser("adc")
    p.add_argument("leaf", nargs="?", type=int, default=0)
    p.add_argument("channel", nargs="?", type=int, default=0)
    p.add_argument("--samples", type=int, default=4)
    p.set_defaults(fn=cmd_adc)

    p = sub.add_parser("sweep")
    p.add_argument("first", nargs="?", type=int, default=0)
    p.add_argument("count", nargs="?", type=int, default=8)
    p.add_argument("--samples", type=int, default=4)
    p.set_defaults(fn=cmd_sweep)

    p = sub.add_parser("shiftout"); p.add_argument("data", nargs="+")
    p.set_defaults(fn=cmd_shiftout)

    p = sub.add_parser("shiftin"); p.add_argument("chips", nargs="?", type=int, default=1)
    p.set_defaults(fn=cmd_shiftin)

    p = sub.add_parser("watch"); p.add_argument("--divisor", type=int, default=1)
    p.set_defaults(fn=cmd_watch)

    p = sub.add_parser("reset"); p.add_argument("port")
    p.add_argument("--hold", type=int, default=10)
    p.set_defaults(fn=cmd_reset)

    p = sub.add_parser("irq"); p.add_argument("port")
    p.set_defaults(fn=cmd_irq)

    sub.add_parser("panel").set_defaults(fn=cmd_panel)

    p = sub.add_parser("mark"); p.add_argument("pulses", nargs="?", type=int, default=3)
    p.set_defaults(fn=cmd_mark)

    p = sub.add_parser("sync"); p.add_argument("--samples", type=int, default=32)
    p.set_defaults(fn=cmd_sync)

    p = sub.add_parser("bench")
    p.add_argument("job", nargs="?", default="crc16")
    p.add_argument("n", nargs="?", type=int, default=100000)
    p.add_argument("--timeout", type=float, default=30.0)
    p.set_defaults(fn=cmd_bench)

    a = ap.parse_args()

    try:
        b = Bench(a.port, a.baud)
    except Exception as e:
        print("could not open %s: %s" % (a.port, e), file=sys.stderr)
        print("\nOn the Luckfox, UART3 is not enabled by default and is not the console.", file=sys.stderr)
        print("The console stays on UART2 (header pins 4/5) -- do not disturb it, it is the", file=sys.stderr)
        print("only way back in when a UART change goes wrong.", file=sys.stderr)
        return 2

    try:
        a.fn(b, a)
        return 0
    except BenchStatus as e:
        print("refused: %s" % e, file=sys.stderr)
        return 1
    except BenchError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    finally:
        b.close()


if __name__ == "__main__":
    sys.exit(main())
