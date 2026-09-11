#!/usr/bin/env python3
"""
bench.py -- the BENCH ONE client, for the Luckfox Pico Mini B.

Speaks the framed protocol over /dev/ttyS3 to Teensy 1, which owns the fabric and routes to the
radio, the worker and the display.

  WHAT THIS IS FOR
  ----------------
  This is how a module nobody has written a driver for gets driven on the day it arrives. The
  raw SPI and raw I2C calls below reach any device on the fabric without a single line of new
  firmware:

      f = Bench()
      f.scan()                                   # what is on the I2C bus
      f.spi_xfer("P3", tx=[0x0F, 0x00])          # any SPI module, addressed by silk label
      f.i2c_xfer(0x76, write=[0xD0], read=1)     # any I2C sensor

  Ports are named the way the silk names them. Nothing here holds a copy of the pin map -- it is
  read from the hardware with ports(), so there is no second copy to drift out of step.

  THE ONE RULE THIS FILE ENFORCES
  -------------------------------
  One outstanding request at a time, because that is what the protocol allows. Every call below
  is synchronous: send, wait for the matching SEQ, return. Unsolicited events that arrive while
  waiting are queued, not discarded -- an interrupt that fires during an unrelated request is
  still real, and dropping it would make the fabric's interrupt behaviour look unreliable when
  it was the client that lost the frame.
"""

import time
from typing import List, Optional, Tuple

import iop

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    serial = None


class BenchError(Exception):
    """A transport-level failure: timeout, NACK, or a malformed reply."""


class BenchStatus(BenchError):
    """The far end understood the request and refused it. Carries the status byte."""

    def __init__(self, status: int, context: str = ""):
        self.status = status
        name = iop.ST_NAMES.get(status, "0x%02X" % status)
        super().__init__("{}{}".format(name, (" (" + context + ")") if context else ""))


class Bench:
    def __init__(self, port: str = "/dev/ttyS3", baud: int = 921600, timeout: float = 2.0):
        if serial is None:
            raise BenchError("pyserial is not installed:  pip3 install pyserial")

        # exclusive=True matters on a shared box: two processes on one UART interleave bytes
        # mid-frame, and the symptom is a stream of CRC failures that looks exactly like a
        # wiring fault. Failing to open is a far better outcome than corrupting a bus.
        self.ser = serial.Serial(port, baud, timeout=0.05, exclusive=True)
        self.parser = iop.Parser()
        self.seq = iop.SeqGen()
        self.timeout = timeout
        self.events: List[iop.Frame] = []
        self._port_cache: Optional[List[dict]] = None

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    # -- transport ---------------------------------------------------------------------------

    def _pump(self) -> List[iop.Frame]:
        data = self.ser.read(4096)
        return self.parser.feed(data) if data else []

    def _txn(self, chan: int, cmd: int, payload: bytes = b"",
             timeout: Optional[float] = None) -> iop.Frame:
        """Send a request and return its reply. Events seen while waiting are queued."""
        seq = self.seq.next_request()
        self.ser.write(iop.build(seq, iop.FLAG_NONE, chan, cmd, payload))

        deadline = time.monotonic() + (timeout if timeout is not None else self.timeout)
        while time.monotonic() < deadline:
            for f in self._pump():
                if f.is_event:
                    # Queued, never dropped. An interrupt that happened to fire during an
                    # unrelated request is still a real interrupt.
                    self.events.append(f)
                    continue
                if f.seq != seq:
                    # A reply to something that already timed out. Ignoring it is correct;
                    # acting on it would attribute an old answer to a new question and put
                    # every subsequent response off by one.
                    continue
                if f.cmd == iop.CMD_NACK:
                    p = f.payload
                    raise BenchError(
                        "NACK for {}/{}: {}".format(
                            iop.CHAN_NAMES.get(p[0], hex(p[0])) if len(p) > 0 else "?",
                            iop.CMD_NAMES.get(p[1], hex(p[1])) if len(p) > 1 else "?",
                            iop.ERR_NAMES.get(p[2], hex(p[2])) if len(p) > 2 else "?"))
                return f
            if self.ser.in_waiting == 0:
                self.parser.tick()

        raise BenchError("timeout waiting for a reply to {}/{}".format(
            iop.CHAN_NAMES.get(chan, hex(chan)), iop.CMD_NAMES.get(cmd, hex(cmd))))

    def _checked(self, f: iop.Frame, context: str = "") -> bytes:
        if not f.payload:
            raise BenchError("empty reply to " + context)
        st = f.payload[0]
        if st != iop.ST_OK:
            raise BenchStatus(st, context)
        return f.payload[1:]

    def poll_events(self) -> List[iop.Frame]:
        """Drain queued and newly-arrived unsolicited frames."""
        for f in self._pump():
            if f.is_event:
                self.events.append(f)
        out, self.events = self.events, []
        return out

    # -- transport-level ---------------------------------------------------------------------

    def ping(self) -> float:
        """Round trip in milliseconds, measured on THIS clock only."""
        t0 = time.monotonic()
        self._txn(iop.CHAN_TRANSPORT, iop.CMD_PING)
        return (time.monotonic() - t0) * 1000.0

    # -- fabric ------------------------------------------------------------------------------

    def info(self) -> dict:
        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_INFO_REQ), "fabric info")
        return {
            "hw_rev": b[0], "proto": b[1], "bench_proto": b[2], "ports": b[3],
            "i2c_transactions": int.from_bytes(b[4:8], "big"),
            "i2c_nacks": int.from_bytes(b[8:12], "big"),
            "spi_transfers": int.from_bytes(b[12:16], "big"),
            "irq_events": int.from_bytes(b[16:20], "big"),
            "irq_storms": int.from_bytes(b[20:24], "big"),
            "faults": int.from_bytes(b[24:28], "big"),
        }

    KIND_NAMES = {0: "none", 1: "SPI", 2: "I2C", 3: "GPIO_EXP",
                  4: "SHIFT_OUT", 5: "SHIFT_IN", 6: "ANALOG", 7: "NATIVE"}

    def ports(self, refresh: bool = False) -> List[dict]:
        """
        Read the port map FROM THE HARDWARE.

        Deliberately not a constant in this file. A second copy of the map would need keeping in
        step with firmware, and this project already carries a sync script for exactly that class
        of duplication. Asking the device removes the possibility of drift entirely.
        """
        if self._port_cache is not None and not refresh:
            return self._port_cache

        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_PORTS_REQ), "port map")
        count = b[0]
        out = []
        off = 1
        for _ in range(count):
            if off + iop.PORT_DESC_LEN > len(b):
                break
            d = b[off:off + iop.PORT_DESC_LEN]
            out.append({
                "id": d[0], "kind": d[1], "kind_name": self.KIND_NAMES.get(d[1], "?"),
                "bus": d[2], "slot": d[3], "first": d[4], "width": d[5],
                "irq_bit": d[6], "ctl_bit": d[7],
                "flags": int.from_bytes(d[8:10], "big"),
                "detected": int.from_bytes(d[10:12], "big"),
            })
            off += iop.PORT_DESC_LEN
        self._port_cache = out
        return out

    def _port_id(self, port) -> int:
        """Accept a numeric id or a silk label like 'P3'."""
        if isinstance(port, int):
            return port
        name = str(port).upper()
        if name in iop.PORT_IDS:
            return iop.PORT_IDS[name]
        raise BenchError("unknown port {!r}. Known: {}".format(
            port, ", ".join(sorted(iop.PORT_IDS))))

    def scan(self, bus: int = 0) -> List[int]:
        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_SCAN_REQ, bytes([bus]),
                                    timeout=4.0), "i2c scan")
        return list(b[2:2 + b[1]])

    def pin_mode(self, port, index: int, mode: int) -> None:
        self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_PIN_MODE_REQ,
                                bytes([self._port_id(port), index, mode])), "pin_mode")

    def pin_write(self, port, index: int, value: int) -> None:
        self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_PIN_WRITE_REQ,
                                bytes([self._port_id(port), index, 1 if value else 0])),
                      "pin_write")

    def pin_read(self, port, index: int) -> int:
        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_PIN_READ_REQ,
                                    bytes([self._port_id(port), index])), "pin_read")
        return b[0]

    def port_read(self, port) -> int:
        """All 16 bits in one bus transaction, so they are one instant and not two."""
        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_PORT_READ_REQ,
                                    bytes([self._port_id(port)])), "port_read")
        return int.from_bytes(b[1:3], "big")

    def port_write(self, port, value: int) -> None:
        self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_PORT_WRITE_REQ,
                                bytes([self._port_id(port), 2]) + value.to_bytes(2, "big")),
                      "port_write")

    def irq_config(self, port, mask: int) -> None:
        self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_IRQ_CFG_REQ,
                                bytes([self._port_id(port)]) + mask.to_bytes(2, "big")),
                      "irq_config")

    # -- SPI port auxiliaries -----------------------------------------------------------------
    #
    # A SPI port carries two signals beyond the bus, and they live on different silicon because
    # they point in opposite directions:
    #
    #     CTL  index 0  OUTPUT, on a 74HC595   reset / chip-enable / mode strap    ~2 us
    #     IRQ  index 1  INPUT,  on an MCP23017  interrupt / data-ready / busy       interrupt-capable
    #
    # A shift-register output changes about sixty times faster than an expander pin; an expander
    # pin is the only one that can raise a flag without being polled. Direction decides which
    # silicon carries the signal, which is why these are two calls and not one.

    def port_ctl(self, port, value: int) -> None:
        """Drive a module's reset / chip-enable line. Latched on one RCLK edge."""
        self.pin_write(port, 0, value)

    def port_reset(self, port, hold_ms: int = 10) -> None:
        """Pulse a module's reset low, then release it and let it come up."""
        self.port_ctl(port, 0)
        time.sleep(hold_ms / 1000.0)
        self.port_ctl(port, 1)
        time.sleep(hold_ms / 1000.0)

    def port_irq(self, port) -> int:
        """Read a module's interrupt line right now. For events, subscribe instead."""
        return self.pin_read(port, 1)

    def spi_xfer(self, port, tx: bytes, mode: int = 0, khz: int = 1000) -> bytes:
        """
        Raw SPI against a named port. The chip select is asserted and released inside this one
        operation, break-before-make, by firmware. There is no way to leave a chip selected.
        """
        tx = bytes(tx)
        payload = (bytes([self._port_id(port), mode]) + khz.to_bytes(2, "big") +
                   bytes([0]) + len(tx).to_bytes(2, "big") + tx)
        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_SPI_XFER_REQ, payload),
                          "spi_xfer")
        n = int.from_bytes(b[0:2], "big")
        return b[2:2 + n]

    def i2c_xfer(self, addr: int, write: bytes = b"", read: int = 0, bus: int = 0) -> bytes:
        """
        Write-then-read against one address, with a REPEATED START when read > 0. That makes a
        register read indivisible, so nothing can land between the pointer write and the data.
        """
        write = bytes(write)
        payload = bytes([bus, addr, len(write), read]) + write
        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_I2C_XFER_REQ, payload),
                          "i2c_xfer addr 0x%02X" % addr)
        return b[1:1 + b[0]]

    def shift_out(self, data: bytes) -> None:
        """data[0] is the chip NEAREST the Teensy. Firmware handles the chain reversal."""
        data = bytes(data)
        self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_SHIFT_OUT_REQ,
                                bytes([0, len(data)]) + data), "shift_out")

    def shift_in(self, chips: int = 1) -> bytes:
        """Simultaneous snapshot of every input, then clocked in. bit7 of each byte is pin H."""
        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_SHIFT_IN_REQ,
                                    bytes([0, chips])), "shift_in")
        return b[1:1 + b[0]]

    def adc(self, leaf: int, channel: int, samples: int = 4) -> int:
        """The first conversion after the mux switches is always discarded by firmware."""
        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_ADC_REQ,
                                    bytes([leaf, channel, samples])), "adc")
        return int.from_bytes(b[3:5], "big")

    def adc_sweep(self, first: int = 0, count: int = 8, samples: int = 4) -> List[int]:
        b = self._checked(self._txn(iop.CHAN_FABRIC, iop.CMD_FAB_ADC_SWEEP_REQ,
                                    bytes([first, count, samples]), timeout=4.0), "adc_sweep")
        n = b[1]
        return [int.from_bytes(b[2 + i * 2:4 + i * 2], "big") for i in range(n)]

    # -- orchestrator --------------------------------------------------------------------------

    def sys(self) -> dict:
        b = self._checked(self._txn(iop.CHAN_ORCH, iop.CMD_ORC_SYS_REQ), "sys")
        mask, n = b[0], b[1]
        nodes = []
        off = 2
        for _ in range(n):
            if off + 8 > len(b):
                break
            nodes.append({
                "node": b[off], "up": bool(b[off + 1]),
                "uptime_ms": int.from_bytes(b[off + 2:off + 6], "big"),
                "errors": int.from_bytes(b[off + 6:off + 8], "big"),
            })
            off += 8
        return {"links_mask": mask, "nodes": nodes}

    def subscribe(self, mask: int = iop.EVMASK_ALL, divisor: int = 1) -> Tuple[int, int]:
        """
        Events default to OFF and stay off until asked for.

        That default is the fix for Defect 1 in this project's own baseline: an unconditional
        relay pushed 202 packets per second at a node that never asked and could not decline.
        Whether telemetry flows upward, and how much, is a decision for whoever is reading it.
        """
        b = self._checked(self._txn(iop.CHAN_ORCH, iop.CMD_ORC_SUB_REQ,
                                    mask.to_bytes(4, "big") + bytes([divisor])), "subscribe")
        return int.from_bytes(b[0:4], "big"), b[4]

    def selftest(self, level: int = 0) -> List[dict]:
        b = self._checked(self._txn(iop.CHAN_ORCH, iop.CMD_ORC_SELFTEST_REQ,
                                    bytes([level]), timeout=25.0), "selftest")
        n = b[1]
        out = []
        for i in range(n):
            q = b[2 + i * 4:6 + i * 4]
            if len(q) < 4:
                break
            out.append({
                "id": q[0], "name": iop.TEST_NAMES.get(q[0], "0x%02X" % q[0]),
                "result": q[1], "result_name": iop.TESTRESULT_NAMES.get(q[1], "?"),
                "detail": int.from_bytes(q[2:4], "big"),
            })
        return out

    def mark(self, mark_id: int = 1, pulses: int = 3) -> int:
        """Pulse the logic-analyser marker pin. Returns Teensy 1's micros() at that instant."""
        b = self._checked(self._txn(iop.CHAN_ORCH, iop.CMD_ORC_MARK_REQ,
                                    bytes([mark_id, pulses])), "mark")
        return int.from_bytes(b[5:9], "big")

    def sync(self, samples: int = 32) -> dict:
        """
        Estimate the offset between this host's clock and Teensy 1's, WITHOUT ever subtracting
        one board's timestamp from another's.

        The host times its own send and receive on its own clock; the Teensy reports its own
        timestamp on its clock. The offset is then a derived quantity with a stated uncertainty
        of half the round trip, not a measurement pretending to be exact. That distinction is
        the whole reason this project refuses to compare two free-running clocks.
        """
        best = None
        for _ in range(samples):
            t0 = time.monotonic()
            f = self._txn(iop.CHAN_ORCH, iop.CMD_ORC_SYNC_REQ, (0).to_bytes(8, "big"))
            t1 = time.monotonic()
            b = self._checked(f, "sync")
            rtt = t1 - t0
            t_dev_us = int.from_bytes(b[12:16], "big")
            if best is None or rtt < best["rtt"]:
                best = {"rtt": rtt, "host_mid": (t0 + t1) / 2.0, "dev_us": t_dev_us}
        return {
            "rtt_ms": best["rtt"] * 1000.0,
            "device_us": best["dev_us"],
            "host_s": best["host_mid"],
            "uncertainty_ms": best["rtt"] * 1000.0 / 2.0,
        }

    def forward(self, dst_node: int, chan: int, cmd: int, payload: bytes = b"") -> int:
        """
        Ask Teensy 1 to relay a frame to a node it owns. The ACCEPTANCE comes back here; the
        far end's actual answer arrives later as an unsolicited event, because blocking the hub
        for the downstream duration would hold this link's single request slot for as long as
        the downstream took -- fifteen seconds, for a WiFi scan.
        """
        payload = bytes(payload)
        b = self._checked(self._txn(iop.CHAN_ORCH, iop.CMD_ORC_FWD_REQ,
                                    bytes([dst_node, chan, cmd]) +
                                    len(payload).to_bytes(2, "big") + payload), "forward")
        return b[0]

    # -- worker, via the hub -------------------------------------------------------------------

    JOB_NOP, JOB_ECHO, JOB_CRC16, JOB_SIEVE, JOB_FLOAT_MADD, JOB_MEMBW, JOB_SORT = range(7)

    def job(self, job_type: int, total: int, job_id: int = 1, args: bytes = b"") -> int:
        payload = (bytes([job_id, job_type]) + total.to_bytes(4, "big") +
                   len(args).to_bytes(2, "big") + bytes(args))
        return self.forward(iop.NODE_TEENSY2, iop.CHAN_WORKER, iop.CMD_WRK_JOB_REQ, payload)

    def wait_job(self, job_id: int = 1, timeout: float = 30.0) -> Optional[dict]:
        """Block until the worker's completion event arrives, or give up and say so."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for f in self.poll_events():
                if f.chan != iop.CHAN_ORCH or f.cmd != iop.CMD_ORC_EVENT:
                    continue
                if len(f.payload) < 3:
                    continue
                inner_cmd = f.payload[2]
                body = f.payload[3:]
                if inner_cmd == iop.CMD_WRK_JOB_EVENT and len(body) >= 16 and body[0] == job_id:
                    return {
                        "job_id": body[0], "state": body[1], "pct": body[2],
                        "result": int.from_bytes(body[3:7], "big"),
                        "ns": int.from_bytes(body[7:11], "big"),
                        "units": int.from_bytes(body[11:15], "big"),
                        "type": body[15],
                    }
            time.sleep(0.002)
        return None
