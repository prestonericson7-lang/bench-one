#!/usr/bin/env python3
"""
orchestrator.py -- the long-running BENCH ONE service on the Luckfox.

Keeps the link to Teensy 1 alive, records every unsolicited event, and exposes the stack's
current state as a JSON file anything else on the box can read.

  WHAT IT DELIBERATELY DOES NOT DO
  --------------------------------
  It does not poll the fabric, it does not drive the display, and it does not decide anything
  about the radio. Those are commands somebody issues; this is the process that keeps the
  channel open and remembers what came up it.

  A daemon that quietly polls hardware in the background is the enemy of a bench session: every
  measurement then has an invisible second actor on the bus. This one is silent unless asked.

  THE THREE THINGS IT HANDLES THAT A NAIVE SCRIPT GETS WRONG
  ----------------------------------------------------------
  1. /dev/ttyS3 MAY NOT EXIST YET. The UART3 overlay is applied at boot by an init script that
     runs at S99. This retries rather than assuming, because ordering is a hope and a retry is
     a mechanism.

  2. SILENCE IS NOT A LINK-DOWN SIGNAL BY ITSELF. Both ends idle high through pull-ups, so an
     unplugged cable and a powered-off peer look identical to an idle healthy link. The only
     way to know is to ask: hence the keepalive on the transport channel.

  3. A REBOOTED PEER RESTARTS ITS EVENT COUNTER. Counting that as packet loss reports a large
     fake number the first time the Teensy is reflashed while this keeps running, and a counter
     that cries wolf once is ignored forever after. A HELLO re-anchors instead.
"""

import json
import os
import signal
import sys
import time

import iop
from bench import Bench, BenchError

PORT = os.environ.get("BENCH_PORT", "/dev/ttyS3")
BAUD = int(os.environ.get("BENCH_BAUD", "921600"))
STATE_PATH = os.environ.get("BENCH_STATE", "/tmp/bench-one.json")
EVENT_LOG = os.environ.get("BENCH_EVENTLOG", "/var/log/bench-events.jsonl")

KEEPALIVE_S = 2.0
RECONNECT_S = 3.0

_running = True


def _stop(signum, frame):
    global _running
    _running = False


signal.signal(signal.SIGTERM, _stop)
signal.signal(signal.SIGINT, _stop)


def log(msg):
    # Unbuffered and timestamped on OUR clock. Never mixes in a device timestamp: two
    # free-running crystals differ by hundreds of ppm, and a log line that silently blends them
    # is a measurement nobody can undo later.
    sys.stdout.write("%.3f  %s\n" % (time.monotonic(), msg))
    sys.stdout.flush()


class State:
    """What the stack looked like the last time anybody asked. Written atomically."""

    def __init__(self):
        self.d = {
            "link": "down",
            "since": None,
            "teensy1_hello": None,
            "nodes": [],
            "fabric": None,
            "counters": {"events": 0, "faults": 0, "reconnects": 0, "keepalive_fail": 0},
            "last_fault": None,
            "updated": None,
        }

    def write(self):
        self.d["updated"] = time.time()
        # Write to a temp file and rename. A reader that catches a half-written JSON file gets
        # a parse error it will probably swallow, and then acts on stale data believing it is
        # fresh. Rename is atomic on the same filesystem, so a reader sees either the old file
        # or the new one and never a partial one.
        tmp = STATE_PATH + ".tmp"
        try:
            with open(tmp, "w") as fh:
                json.dump(self.d, fh, indent=1)
            os.replace(tmp, STATE_PATH)
        except OSError as e:
            log("state write failed: %s" % e)


def record_event(f):
    """Append one unsolicited frame to the event log as a JSON line."""
    try:
        rec = {
            "t": time.time(),
            "chan": iop.CHAN_NAMES.get(f.chan, "0x%02X" % f.chan),
            "cmd": iop.CMD_NAMES.get(f.cmd, "0x%02X" % f.cmd),
            "seq": f.seq,
            "payload": f.payload.hex(),
        }
        if f.chan == iop.CHAN_ORCH and f.cmd == iop.CMD_ORC_EVENT and len(f.payload) >= 3:
            rec["src"] = iop.NODE_NAMES.get(f.payload[0], "0x%02X" % f.payload[0])
            rec["inner_chan"] = iop.CHAN_NAMES.get(f.payload[1], "0x%02X" % f.payload[1])
            rec["inner_cmd"] = iop.CMD_NAMES.get(f.payload[2], "0x%02X" % f.payload[2])
            rec["payload"] = f.payload[3:].hex()
        with open(EVENT_LOG, "a") as fh:
            fh.write(json.dumps(rec) + "\n")
    except OSError:
        # A full or read-only filesystem must not take the link down. Losing the log is bad;
        # losing the link because logging failed is worse.
        pass


def open_link():
    """Retry until the port exists and opens. Returns a Bench, or None if asked to stop."""
    announced = False
    while _running:
        try:
            b = Bench(PORT, BAUD, timeout=1.0)
            log("opened %s at %d" % (PORT, BAUD))
            return b
        except Exception as e:
            if not announced:
                # Said once, not every three seconds. A log that repeats the same line forever
                # is a log nobody reads, and this condition is completely normal at boot: the
                # UART3 overlay is applied by an init script that runs at S99.
                log("cannot open %s (%s) -- retrying every %.0fs" % (PORT, e, RECONNECT_S))
                announced = True
            for _ in range(int(RECONNECT_S * 10)):
                if not _running:
                    return None
                time.sleep(0.1)
    return None


def main():
    st = State()
    st.write()
    log("BENCH ONE orchestrator starting")
    log("protocol v%d.%d, CRC-16/MCRF4XX check 0x%04X"
        % (iop.IOP_PROTOCOL_VERSION, iop.IOP_PROTOCOL_MINOR, iop.crc16(b"123456789")))

    b = None
    last_keepalive = 0.0

    while _running:
        if b is None:
            b = open_link()
            if b is None:
                break
            st.d["counters"]["reconnects"] += 1

            # Events default to OFF and stay off until asked for. That default is the fix for
            # Defect 1 in this project's own baseline, where an unconditional relay pushed 202
            # packets per second at a node that never asked and could not decline.
            #
            # Faults and link changes are subscribed because they are the two things nobody
            # would think to poll for. Fabric interrupts and worker progress are NOT, because
            # they are high-rate and only interesting to whoever started them.
            try:
                mask = (iop.EVMASK_FABRIC_FAULT | iop.EVMASK_LINK | iop.EVMASK_LOG)
                got, div = b.subscribe(mask, 1)
                log("subscribed: mask 0x%08X divisor %d" % (got, div))
                st.d["link"] = "up"
                st.d["since"] = time.time()
                st.write()
            except BenchError as e:
                log("subscribe failed: %s" % e)
                b.close()
                b = None
                continue

        now = time.monotonic()

        # Drain events first. Anything that arrived is real regardless of what happens below.
        try:
            for f in b.poll_events():
                st.d["counters"]["events"] += 1
                record_event(f)

                if f.chan == iop.CHAN_ORCH and f.cmd == iop.CMD_ORC_EVENT and len(f.payload) >= 3:
                    inner = f.payload[2]
                    body = f.payload[3:]

                    if inner == iop.CMD_FAB_FAULT_EVENT and len(body) >= 2:
                        st.d["counters"]["faults"] += 1
                        fault = {
                            "t": time.time(),
                            "code": iop.FAULT_NAMES.get(body[0], "0x%02X" % body[0]),
                            "port": iop.PORT_NAMES.get(body[1], "0x%02X" % body[1]),
                        }
                        st.d["last_fault"] = fault
                        log("FABRIC FAULT %s on port %s" % (fault["code"], fault["port"]))
                        st.write()

                elif f.chan == iop.CHAN_ORCH and f.cmd == iop.CMD_ORC_HELLO:
                    # Teensy 1 rebooted. Re-anchor rather than counting a counter restart as
                    # loss, and re-subscribe, because its event mask went back to zero with it.
                    log("TEENSY1 HELLO -- it rebooted; re-subscribing")
                    st.d["teensy1_hello"] = time.time()
                    if len(f.payload) >= 9:
                        peer_check = int.from_bytes(f.payload[7:9], "big")
                        if peer_check != iop.CRC16_CHECK_VALUE:
                            log("PROTOCOL MISMATCH: peer CRC check 0x%04X, ours 0x%04X"
                                % (peer_check, iop.CRC16_CHECK_VALUE))
                            log("  The headers have drifted. Run tools/sync_headers.py and "
                                "REFLASH EVERY BOARD.")
                    try:
                        b.subscribe(iop.EVMASK_FABRIC_FAULT | iop.EVMASK_LINK | iop.EVMASK_LOG, 1)
                    except BenchError:
                        pass
                    st.write()
        except (BenchError, OSError) as e:
            log("read failed: %s -- reopening" % e)
            b.close()
            b = None
            st.d["link"] = "down"
            st.write()
            continue

        # Keepalive. Absence of bytes is the ONLY link-down signal there is: both ends idle
        # high through pull-ups, so an unplugged cable, a powered-off peer and a perfectly
        # healthy idle link are indistinguishable on the wire. Asking is the only way to know.
        if (now - last_keepalive) >= KEEPALIVE_S:
            last_keepalive = now
            try:
                rtt = b.ping()
                if st.d["link"] != "up":
                    st.d["link"] = "up"
                    st.d["since"] = time.time()
                    log("link up, %.3f ms round trip" % rtt)
                    st.write()
            except BenchError:
                st.d["counters"]["keepalive_fail"] += 1
                if st.d["link"] == "up":
                    log("keepalive lost -- link down")
                    st.d["link"] = "down"
                    st.write()
                # The port is not reopened on a single miss. A missed keepalive is usually the
                # far end being briefly busy; reopening on every one would turn a hiccup into a
                # reconnect storm. Three consecutive misses is a real fault.
                if st.d["counters"]["keepalive_fail"] % 3 == 0:
                    log("three consecutive keepalive failures -- reopening the port")
                    b.close()
                    b = None
                    continue

        time.sleep(0.005)

    log("stopping")
    if b is not None:
        try:
            b.subscribe(iop.EVMASK_NONE, 1)
        except Exception:
            pass
        b.close()
    st.d["link"] = "down"
    st.write()
    return 0


if __name__ == "__main__":
    sys.exit(main())
