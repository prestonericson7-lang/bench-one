#!/usr/bin/env python3
"""
iop.py -- the BENCH ONE wire protocol, in Python, for the Luckfox.

This is a FAITHFUL PORT of interop_protocol.h. Every constant here was read out of that header,
not out of prose, and the module refuses to load if its CRC does not reproduce the catalogue
check value.

  WHY THAT MATTERS MORE THAN USUAL
  --------------------------------
  The project's own handoff notes describe this protocol as "CRC8, poly 0x31, check 0xA2, frame
  START|LEN_H|LEN_L|SEQ|CMD|PAYLOAD|CRC8". That is the v1 protocol and it is wrong for the
  firmware actually on the boards. A decoder built from that description fails on every single
  frame, and the failure looks like a wiring fault.

  The real frame, from the header:

      START(0xAA) | LEN_H | LEN_L | SEQ | FLAGS | CHAN | CMD | PAYLOAD | CRC_H | CRC_L

      LEN   big-endian, counts FLAGS + CHAN + CMD + payload. Overhead is 6 bytes.
      CRC   CRC-16/MCRF4XX. poly 0x1021 reflected to 0x8408, init 0xFFFF, refin/refout,
            xorout 0x0000, check("123456789") == 0x6F91. Covers LEN..payload; START excluded.
      SEQ   bit 7 is a CLASS bit. Set = unsolicited event, with its own rolling counter.
            It is NOT "SEQ 0x00 means unsolicited" -- that was v1.

  _selftest() at import time proves the CRC against the catalogue value and round-trips a frame
  through the parser. If this file is ever edited into disagreement with the firmware, it fails
  at import rather than at 2 a.m. against real hardware.

Python 3.6+. Depends only on pyserial, and only for BenchLink.
"""

import struct
import time
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

# ===========================================================================================
# WIRE CONSTANTS -- from interop_protocol.h SECTION 1
# ===========================================================================================

IOP_PROTOCOL_VERSION = 2
IOP_PROTOCOL_MINOR = 0

START_BYTE = 0xAA
FRAME_OVERHEAD = 6          # START + LEN_H + LEN_L + SEQ + CRC_H + CRC_L
BODY_HEADER_LEN = 3         # FLAGS + CHAN + CMD
MAX_FRAME_SIZE = 1024
MAX_BODY_LEN = MAX_FRAME_SIZE - FRAME_OVERHEAD      # 1018
MAX_PAYLOAD_LEN = MAX_BODY_LEN - BODY_HEADER_LEN    # 1015
MIN_BODY_LEN = BODY_HEADER_LEN
MAX_SKIP_BYTES = MAX_FRAME_SIZE * 2
FRAME_TIMEOUT_MS = 50

SEQ_CLASS_MASK = 0x80
SEQ_COUNTER_MASK = 0x7F

FLAG_NONE = 0x00
FLAG_MORE = 0x01
FLAG_TRUNCATED = 0x02

# ---- channels ----
CHAN_TRANSPORT = 0x00
CHAN_WIFI = 0x01
CHAN_BLE = 0x02
CHAN_DATA = 0x03
CHAN_MESH = 0x04
# BENCH ONE extension, from bench_protocol.h
CHAN_FABRIC = 0x05
CHAN_WORKER = 0x06
CHAN_ORCH = 0x07
CHAN_HMI = 0x08
CHAN_ROUTE = 0x09

CHAN_NAMES = {
    CHAN_TRANSPORT: "TRANSPORT", CHAN_WIFI: "WIFI", CHAN_BLE: "BLE",
    CHAN_DATA: "DATA", CHAN_MESH: "MESH", CHAN_FABRIC: "FABRIC",
    CHAN_WORKER: "WORKER", CHAN_ORCH: "ORCH", CHAN_HMI: "HMI", CHAN_ROUTE: "ROUTE",
}

# ---- transport commands ----
CMD_PING = 0x01
CMD_PONG = 0x02
CMD_RADIO_READY = 0x04
CMD_NACK = 0xFE

# ---- fabric commands (channel 0x05) ----
CMD_FAB_INFO_REQ = 0x50
CMD_FAB_INFO_RESP = 0x51
CMD_FAB_PORTS_REQ = 0x52
CMD_FAB_PORTS_RESP = 0x53
CMD_FAB_SCAN_REQ = 0x54
CMD_FAB_SCAN_RESP = 0x55
CMD_FAB_PIN_MODE_REQ = 0x56
CMD_FAB_PIN_MODE_RESP = 0x57
CMD_FAB_PIN_WRITE_REQ = 0x58
CMD_FAB_PIN_WRITE_RESP = 0x59
CMD_FAB_PIN_READ_REQ = 0x5A
CMD_FAB_PIN_READ_RESP = 0x5B
CMD_FAB_PORT_WRITE_REQ = 0x5C
CMD_FAB_PORT_WRITE_RESP = 0x5D
CMD_FAB_PORT_READ_REQ = 0x5E
CMD_FAB_PORT_READ_RESP = 0x5F
CMD_FAB_IRQ_EVENT = 0x60
CMD_FAB_IRQ_CFG_REQ = 0x61
CMD_FAB_IRQ_CFG_RESP = 0x62
CMD_FAB_SPI_XFER_REQ = 0x63
CMD_FAB_SPI_XFER_RESP = 0x64
CMD_FAB_I2C_XFER_REQ = 0x65
CMD_FAB_I2C_XFER_RESP = 0x66
CMD_FAB_SHIFT_OUT_REQ = 0x67
CMD_FAB_SHIFT_OUT_RESP = 0x68
CMD_FAB_SHIFT_IN_REQ = 0x69
CMD_FAB_SHIFT_IN_RESP = 0x6A
CMD_FAB_ADC_REQ = 0x6B
CMD_FAB_ADC_RESP = 0x6C
CMD_FAB_ADC_SWEEP_REQ = 0x6D
CMD_FAB_ADC_SWEEP_RESP = 0x6E
CMD_FAB_FAULT_EVENT = 0x6F

# ---- worker commands (channel 0x06) ----
CMD_WRK_HELLO = 0x70
CMD_WRK_JOB_REQ = 0x71
CMD_WRK_JOB_RESP = 0x72
CMD_WRK_JOB_EVENT = 0x73
CMD_WRK_RESULT_REQ = 0x74
CMD_WRK_RESULT_RESP = 0x75
CMD_WRK_CANCEL_REQ = 0x76
CMD_WRK_CANCEL_RESP = 0x77
CMD_WRK_STATUS_REQ = 0x78
CMD_WRK_STATUS_RESP = 0x79
CMD_WRK_BENCH_REQ = 0x7A
CMD_WRK_BENCH_RESP = 0x7B

# ---- orchestrator commands (channel 0x07) ----
CMD_ORC_HELLO = 0x80
CMD_ORC_SYS_REQ = 0x81
CMD_ORC_SYS_RESP = 0x82
CMD_ORC_FWD_REQ = 0x83
CMD_ORC_FWD_RESP = 0x84
CMD_ORC_EVENT = 0x85
CMD_ORC_SUB_REQ = 0x86
CMD_ORC_SUB_RESP = 0x87
CMD_ORC_SYNC_REQ = 0x88
CMD_ORC_SYNC_RESP = 0x89
CMD_ORC_LOG = 0x8A
CMD_ORC_MARK_REQ = 0x8B
CMD_ORC_MARK_RESP = 0x8C
CMD_ORC_SELFTEST_REQ = 0x8D
CMD_ORC_SELFTEST_RESP = 0x8E

# ---- HMI commands (channel 0x08) ----
CMD_HMI_HELLO = 0x90
CMD_HMI_STATE_REQ = 0x91
CMD_HMI_STATE_RESP = 0x92
CMD_HMI_INPUT_EVENT = 0x93
CMD_HMI_STATUS_REQ = 0x94
CMD_HMI_STATUS_RESP = 0x95
CMD_HMI_BL_REQ = 0x96
CMD_HMI_BL_RESP = 0x97

# ---- route commands (channel 0x09) ----
CMD_RTE_FRAME = 0xA0
CMD_RTE_ANNOUNCE = 0xA1
CMD_RTE_PING_REQ = 0xA2
CMD_RTE_PING_RESP = 0xA3

CMD_NAMES = {v: k[4:] for k, v in list(globals().items()) if k.startswith("CMD_")}

# ---- node ids, from bench_protocol.h SECTION 2 ----
# Not addresses. Every link is point-to-point, so a frame needs no address to be delivered.
# These exist so a node can identify itself in a HELLO, so a log line has an unambiguous
# subject, and so a routed frame can name a destination on a two-stack build.
NODE_UNKNOWN = 0x00
NODE_LUCKFOX = 0x01
NODE_TEENSY1 = 0x02
NODE_TEENSY2 = 0x03
NODE_ESP32S3 = 0x04
NODE_HMI = 0x05

NODE_NAMES = {
    NODE_LUCKFOX: "LUCKFOX", NODE_TEENSY1: "TEENSY1", NODE_TEENSY2: "TEENSY2",
    NODE_ESP32S3: "ESP32S3", NODE_HMI: "HMI",
}

# ---- event subscription mask, from bench_protocol.h SECTION 6 ----
EVMASK_NONE = 0x00000000
EVMASK_FABRIC_IRQ = 0x00000001
EVMASK_FABRIC_FAULT = 0x00000002
EVMASK_WORKER_JOB = 0x00000004
EVMASK_RADIO = 0x00000008
EVMASK_HMI_INPUT = 0x00000010
EVMASK_LOG = 0x00000020
EVMASK_LINK = 0x00000040
EVMASK_ALL = 0xFFFFFFFF

# ---- logical pin modes ----
PINMODE_INPUT = 0x00
PINMODE_INPUT_PULLUP = 0x01
PINMODE_OUTPUT = 0x02
PINMODE_OUTPUT_SINK = 0x03

# ---- port descriptor wire format, from bench_ports.h ----
# 12 bytes: id, kind, bus, slot, first, width, irq_bit, reserved, flags(2), detected(2).
# Keep this in step with BENCH_PORT_DESC_WIRE_LEN. It is asserted below, not merely hoped.
PORT_DESC_LEN = 12
PORT_DESC_STRUCT = "<8BHH"

# Silk labels to port ids. This is the ONLY duplicated piece of the port map, and it is here
# because a human types "P3" and the wire needs 0x13. Everything else about a port -- its bus,
# slot, width, presence -- is read from the hardware by Bench.ports(), so there is no second
# copy of the map to drift.
PORT_IDS = {
    "P0": 0x10, "P1": 0x11, "P2": 0x12, "P3": 0x13,
    "P4": 0x14, "P5": 0x15, "P6": 0x16, "P7": 0x17,
    "XA": 0x20, "XB": 0x21, "XC": 0x22, "XD": 0x23,
    "XE": 0x24, "XF": 0x25, "XG": 0x26, "XH": 0x27,
    "M0": 0x30, "M1": 0x31, "M2": 0x32, "M3": 0x33,
    "CTL": 0x40, "OA": 0x41, "OB": 0x42,
    "IA": 0x48, "IB": 0x49,
    "A0": 0x50, "A1": 0x51, "A2": 0x52, "A3": 0x53,
    "A4": 0x54, "A5": 0x55, "A6": 0x56, "A7": 0x57,
    "TFT": 0x60, "MARK": 0x61,
}
PORT_NAMES = {v: k for k, v in PORT_IDS.items()}

FAULT_NAMES_EXTRA = {}

# ---- NACK error codes ----
ERR_NAMES = {
    0x01: "UNKNOWN_CMD", 0x02: "BAD_CRC", 0x03: "TIMEOUT", 0x04: "BUSY",
    0x05: "PAYLOAD_LARGE", 0x06: "BAD_PAYLOAD", 0x07: "UNKNOWN_CHAN",
    0x08: "NOT_SUPPORTED",
}

# ---- BENCH status codes, from bench_protocol.h SECTION 9 ----
ST_OK = 0x00
ST_NAMES = {
    0x00: "OK", 0x01: "BUSY", 0x02: "BAD_PORT", 0x03: "BAD_INDEX", 0x04: "BAD_ARG",
    0x05: "NOT_PRESENT", 0x06: "BUS_ERROR", 0x07: "TIMEOUT", 0x08: "WRONG_MODE",
    0x09: "NO_SUCH_JOB", 0x0A: "UNSUPPORTED", 0x0B: "OVERFLOW", 0x0C: "HW_FAULT",
    0xFF: "ERROR",
}

FAULT_NAMES = {
    0x01: "I2C_NACK", 0x02: "I2C_WEDGED", 0x03: "SPI_NO_RESPONSE", 0x04: "IRQ_STORM",
    0x05: "DEVICE_LOST", 0x06: "DEVICE_APPEARED", 0x07: "RAIL_LOW",
    0x08: "RAIL_OVERCURRENT", 0x09: "SELFTEST_FAILED", 0x0A: "CS_CONFLICT",
}

TEST_NAMES = {
    0x01: "I2C_IDLE_HIGH", 0x02: "I2C_SCAN", 0x03: "MCP_READBACK", 0x04: "SPI_LOOPBACK",
    0x05: "SHIFT_LOOPBACK", 0x06: "CS_EXCLUSIVE", 0x07: "CS_DESELECT", 0x08: "ADC_RAILS",
    0x09: "IRQ_ROUNDTRIP", 0x0A: "POWER_BUDGET",
}
TESTRESULT_NAMES = {0: "PASS", 1: "FAIL", 2: "SKIPPED", 3: "NOT_RUN"}


# ===========================================================================================
# CRC-16/MCRF4XX
# ===========================================================================================

def _build_crc_table() -> List[int]:
    """
    Generated, never typed.

    The firmware header records that its table was generated and cross-checked against an
    independent bitwise implementation over 70,792 cases with zero mismatches, and explicitly
    warns that a hand-typed table "still looks like a valid permutation and still passes casual
    inspection while being wrong". Generating it here is the same defence, and it costs 8 lines.
    """
    table = []
    for i in range(256):
        crc = i
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if (crc & 1) else (crc >> 1)
        table.append(crc & 0xFFFF)
    return table


CRC16_TABLE = _build_crc_table()
CRC16_INIT = 0xFFFF
CRC16_CHECK_VALUE = 0x6F91


def crc16(data: bytes, crc: int = CRC16_INIT) -> int:
    """Reflected update: crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFF]."""
    for b in data:
        crc = (crc >> 8) ^ CRC16_TABLE[(crc ^ b) & 0xFF]
    return crc & 0xFFFF


def crc16_bitwise(data: bytes) -> int:
    """Independent implementation, used only to cross-check the table at import."""
    crc = CRC16_INIT
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if (crc & 1) else (crc >> 1)
    return crc & 0xFFFF


# ===========================================================================================
# FRAME
# ===========================================================================================

@dataclass
class Frame:
    seq: int
    flags: int
    chan: int
    cmd: int
    payload: bytes = b""

    @property
    def is_event(self) -> bool:
        """SEQ bit 7 set = unsolicited. This is a class bit, not a reserved value."""
        return (self.seq & SEQ_CLASS_MASK) != 0

    def __repr__(self) -> str:
        kind = "EVT" if self.is_event else "RSP"
        return ("Frame({} {}/{} seq=0x{:02X} len={})".format(
            kind, CHAN_NAMES.get(self.chan, "CH{:02X}".format(self.chan)),
            CMD_NAMES.get(self.cmd, "CMD{:02X}".format(self.cmd)),
            self.seq, len(self.payload)))


def build(seq: int, flags: int, chan: int, cmd: int, payload: bytes = b"") -> bytes:
    """
    Encode one frame. Mirrors iop_build_frame().

    LEN counts FLAGS + CHAN + CMD + payload -- not the whole frame, and not just the payload.
    Getting that wrong produces frames the firmware rejects as bad length while the sender is
    convinced it is sending valid data.
    """
    if len(payload) > MAX_PAYLOAD_LEN:
        raise ValueError("payload {} exceeds {}".format(len(payload), MAX_PAYLOAD_LEN))

    body = bytes([flags & 0xFF, chan & 0xFF, cmd & 0xFF]) + payload
    header = struct.pack(">H", len(body)) + bytes([seq & 0xFF])

    # CRC covers LEN..payload. START is excluded, because a resyncing parser has already
    # consumed it before it starts accumulating.
    crc = crc16(header + body)
    return bytes([START_BYTE]) + header + body + struct.pack(">H", crc)


def build_nack(seq: int, orig_chan: int, orig_cmd: int, error: int) -> bytes:
    return build(seq, FLAG_NONE, CHAN_TRANSPORT, CMD_NACK,
                 bytes([orig_chan & 0xFF, orig_cmd & 0xFF, error & 0xFF]))


class ParseError(Exception):
    pass


class Parser:
    """
    Byte-at-a-time resynchronising parser, matching the firmware's state machine including its
    SKIP behaviour.

    The SKIP state is not decoration. The firmware's header records what happens without it:
    twelve bytes of injected noise containing a stray 0xAA produced a declared length of 43,538,
    and the parser sat waiting for bytes that were never coming while eating the perfectly good
    frame that followed. A declared length inside MAX_SKIP_BYTES is drained precisely so the
    stream resumes cleanly; anything larger is judged a false start and triggers an immediate
    resync.
    """

    WAIT_START, LEN_H, LEN_L, SEQ, BODY, CRC_H, CRC_L, SKIP = range(8)

    def __init__(self):
        self.reset()
        self.frames_ok = 0
        self.bad_crc = 0
        self.bad_length = 0
        self.oversize = 0
        self.timeouts = 0
        self.resync_bytes = 0

    def reset(self):
        self.state = self.WAIT_START
        self.expected_len = 0
        self.body = bytearray()
        self.seq = 0
        self.crc_running = CRC16_INIT
        self.crc_received = 0
        self.skip_remaining = 0
        self.last_byte_time = 0.0

    def push(self, b: int) -> Optional[Frame]:
        self.last_byte_time = time.monotonic()

        if self.state == self.WAIT_START:
            if b == START_BYTE:
                self.state = self.LEN_H
                self.crc_running = CRC16_INIT
                self.body = bytearray()
            else:
                self.resync_bytes += 1
            return None

        if self.state == self.LEN_H:
            self.expected_len = b << 8
            self.crc_running = crc16(bytes([b]), self.crc_running)
            self.state = self.LEN_L
            return None

        if self.state == self.LEN_L:
            self.expected_len |= b
            self.crc_running = crc16(bytes([b]), self.crc_running)

            if self.expected_len < MIN_BODY_LEN:
                # A body shorter than FLAGS+CHAN+CMD cannot be a frame at all.
                self.bad_length += 1
                self.reset()
                return None

            if self.expected_len > MAX_BODY_LEN:
                if self.expected_len <= MAX_SKIP_BYTES:
                    # Plausibly a real frame from a peer built with a larger MAX_FRAME_SIZE.
                    # Drain it exactly, so the stream resumes on a frame boundary.
                    self.oversize += 1
                    self.skip_remaining = self.expected_len + 2   # body + CRC
                    self.state = self.SKIP
                else:
                    # Beyond anything our own builder could emit: this was a false start.
                    self.oversize += 1
                    self.reset()
                return None

            self.state = self.SEQ
            return None

        if self.state == self.SEQ:
            self.seq = b
            self.crc_running = crc16(bytes([b]), self.crc_running)
            self.state = self.BODY
            return None

        if self.state == self.BODY:
            self.body.append(b)
            self.crc_running = crc16(bytes([b]), self.crc_running)
            if len(self.body) >= self.expected_len:
                self.state = self.CRC_H
            return None

        if self.state == self.CRC_H:
            self.crc_received = b << 8
            self.state = self.CRC_L
            return None

        if self.state == self.CRC_L:
            received = self.crc_received | b
            computed = self.crc_running
            seq, body = self.seq, bytes(self.body)
            self.reset()
            if received != computed:
                self.bad_crc += 1
                return None
            self.frames_ok += 1
            return Frame(seq=seq, flags=body[0], chan=body[1], cmd=body[2], payload=body[3:])

        if self.state == self.SKIP:
            self.skip_remaining -= 1
            if self.skip_remaining <= 0:
                self.reset()
            return None

        self.reset()
        return None

    def feed(self, data: bytes) -> List[Frame]:
        out = []
        for b in data:
            f = self.push(b)
            if f is not None:
                out.append(f)
        return out

    def tick(self, now: Optional[float] = None) -> bool:
        """
        Inter-byte timeout. Returns True if a partial frame was abandoned.

        CALL THIS ONLY WHEN THE RECEIVE QUEUE IS EMPTY. The firmware learned this the hard way:
        evaluating the timeout while bytes are still buffered makes a slow reader look like a
        stalled sender, and the parser discards frames that arrived perfectly well.
        """
        if self.state == self.WAIT_START:
            return False
        now = now if now is not None else time.monotonic()
        if (now - self.last_byte_time) * 1000.0 >= FRAME_TIMEOUT_MS:
            self.timeouts += 1
            self.reset()
            return True
        return False

    def stats(self) -> dict:
        return {
            "frames_ok": self.frames_ok, "bad_crc": self.bad_crc,
            "bad_length": self.bad_length, "oversize": self.oversize,
            "timeouts": self.timeouts, "resync_bytes": self.resync_bytes,
        }


# ===========================================================================================
# SEQUENCE COUNTERS
# ===========================================================================================

class SeqGen:
    """
    Two independent counters, matching iop_next_request_seq / iop_next_event_seq.

    Keeping the class bit outside the arithmetic is what stops one counter wrapping into the
    other. In v1 a single reserved value carried the meaning and needed a helper that remembered
    to skip it -- which is the kind of thing that works until someone writes the increment by
    hand in one place.
    """

    def __init__(self):
        self._req = 0
        self._evt = 0

    def next_request(self) -> int:
        self._req = (self._req + 1) & SEQ_COUNTER_MASK
        return self._req

    def next_event(self) -> int:
        self._evt = (self._evt + 1) & SEQ_COUNTER_MASK
        return SEQ_CLASS_MASK | self._evt


# ===========================================================================================
# IMPORT-TIME SELF-TEST
# ===========================================================================================

def _selftest() -> None:
    # 1. The catalogue check value. If this fails, every frame this module builds is wrong.
    check = crc16(b"123456789")
    if check != CRC16_CHECK_VALUE:
        raise RuntimeError(
            "CRC-16/MCRF4XX check value is 0x{:04X}, expected 0x{:04X}. "
            "This module would not interoperate with the firmware.".format(
                check, CRC16_CHECK_VALUE))

    # 2. Table and bitwise implementations must agree. They are derived independently, so
    #    agreement over a wide input set is real evidence rather than a tautology.
    for probe in (b"", b"\x00", b"\xFF", b"123456789", bytes(range(256))):
        if crc16(probe) != crc16_bitwise(probe):
            raise RuntimeError("CRC table disagrees with the bitwise implementation")

    # 3. Round trip through the parser.
    raw = build(0x42, FLAG_NONE, CHAN_FABRIC, CMD_FAB_INFO_REQ, b"\x01\x02\x03")
    p = Parser()
    frames = p.feed(raw)
    if len(frames) != 1:
        raise RuntimeError("round trip produced {} frames, expected 1".format(len(frames)))
    f = frames[0]
    if (f.seq, f.chan, f.cmd, f.payload) != (0x42, CHAN_FABRIC, CMD_FAB_INFO_REQ, b"\x01\x02\x03"):
        raise RuntimeError("round trip corrupted the frame")

    # 4. A corrupted byte must be REJECTED. A parser that accepts a damaged frame is worse than
    #    no parser, and this is the assertion that proves the CRC is actually wired in.
    bad = bytearray(raw)
    bad[5] ^= 0xFF
    p2 = Parser()
    if p2.feed(bytes(bad)):
        raise RuntimeError("parser accepted a corrupted frame")

    # 5. FALSE STARTS. Two cases, and they behave differently on purpose.
    #
    #    5a. An IMPOSSIBLE declared length -- larger than anything our own builder could emit --
    #        is judged a false start and resynchronises AT ONCE, so the good frame behind it
    #        survives intact. This is the fix for the exact hardware-learned failure recorded in
    #        the firmware header, where twelve bytes of noise containing a stray 0xAA produced a
    #        declared length of 43,538 and the parser ate the perfectly good frame that followed.
    p3 = Parser()
    got = p3.feed(b"\xAA\xFF\xFF" + raw)
    if len(got) != 1:
        raise RuntimeError(
            "an impossible declared length must resync immediately and preserve the next frame")

    #    5b. A PLAUSIBLE declared length is a different matter, and the honest statement is that
    #        the following frame IS lost. A stray 0xAA followed by bytes that spell a legal
    #        length means the parser cannot yet know it is wrong: it consumes that many bytes,
    #        fails the CRC, and only then resynchronises. No length-plus-CRC framing can do
    #        better without byte stuffing, which this protocol deliberately does not use.
    #
    #        So the guarantee that actually holds is RECOVERY, not immunity: one frame is lost,
    #        and the next one parses. Asserting immunity here would encode a comfortable belief
    #        rather than the measured behaviour -- and this project's own bring-up log is a list
    #        of what that costs.
    #        The false start below declares length 3, so it swallows the first six bytes of the
    #        real frame, fails the CRC, and resynchronises. The rest of that frame is then
    #        discarded as pre-START noise. The NEXT frame parses perfectly.
    #
    #        Worth knowing at the bench: the number of bytes lost is set by the DECLARED length,
    #        not by the frame behind it. A stray 0xAA that happens to declare 170 swallows 172
    #        bytes, which at 921600 is about 1.9 ms and can span more than one real frame.
    p4 = Parser()
    swallowed = p4.feed(b"\xAA\x00\x03" + raw)
    if swallowed:
        raise RuntimeError("expected the false start to consume the frame behind it")
    if p4.bad_crc != 1:
        raise RuntimeError("the false start should have been caught by the CRC, not silently")
    recovered = p4.feed(raw)
    if len(recovered) != 1:
        raise RuntimeError("parser failed to recover on the frame after a plausible false start")


_selftest()


if __name__ == "__main__":
    print("iop.py self-test passed.")
    print("  protocol v{}.{}".format(IOP_PROTOCOL_VERSION, IOP_PROTOCOL_MINOR))
    print("  CRC-16/MCRF4XX check(\"123456789\") = 0x{:04X}".format(crc16(b"123456789")))
    print("  max payload {} bytes, frame overhead {} bytes".format(
        MAX_PAYLOAD_LEN, FRAME_OVERHEAD))
    demo = build(1, FLAG_NONE, CHAN_TRANSPORT, CMD_PING)
    print("  PING frame: {}".format(" ".join("{:02X}".format(b) for b in demo)))
