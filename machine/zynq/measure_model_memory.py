#!/usr/bin/env python3
"""measure_model_memory.py -- how much of its own memory (anonymous RSS, what the OOM killer counts) the
model runtime needs on the Zynq image, CPU alone and with the engine attached, measured on the image's own
armhf binaries in QEMU. The two-board emulation ran out at the engine-attach step with the Orange Pi's swap
export on (57 MB available of 223). On the board the engine's store is outside Linux; run_model's own peak
is what decides whether the attach step fits next to the export there.

The swap export is stopped inside the emulated board first, so the measurement itself is not starved.
Against qemu_serial_tcp.sh on localhost:5555 (no fpgagpu marker: the engine is the server's CPU stand-in).
    python machine/zynq/measure_model_memory.py
"""
import re, sys, time
import serial

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5555
for _ in range(240):
    try:
        s = serial.serial_for_url("socket://localhost:%d" % PORT + "", baudrate=115200, timeout=0.2); break
    except Exception:
        time.sleep(1)
buf = b""


def pump(sec, pat):
    global buf
    end = time.time() + sec
    while time.time() < end:
        buf += s.read(4096)
        if re.search(pat, buf.decode("utf-8", "replace")):
            return True
    return False


n = [0]


def sh(cmd, sec=600):
    global buf
    n[0] += 1; tag = "__M%dE__" % n[0]
    buf = b""
    s.write((cmd + "; echo '__M%d''E__'\r" % n[0]).encode())
    if not pump(sec, re.escape(tag) + r"\s"):
        print(f"  no answer in {sec} s: {cmd}"); return ""
    t = buf.decode("utf-8", "replace").replace("\r", "")
    t = re.sub(r"\x1b\[[?0-9;]*[A-Za-z]", "", t)
    body = t.split("\n", 1)[1] if "\n" in t else ""
    return body[:body.rfind(tag)].strip()


assert pump(900, r"ZYNQ-REPORT END"), "no report"
time.sleep(2); s.write(b"\r"); pump(10, r"root@zynq")
sh("stty cols 4000")
print("== with the swap export stopped, for room to measure")
print(sh("systemctl stop nbd-server zynqram-prep; umount /run/zynqram 2>/dev/null; free -m | head -2"))
sh("""peak() { m=0; while kill -0 $1 2>/dev/null; do a=$(awk '/RssAnon/{print $2}' /proc/$1/status 2>/dev/null); [ -n "$a" ] && [ "$a" -gt "$m" ] && m=$a; sleep 0.2; done; echo $m; }""")
M, P, B = "/opt/machine/models/qwen05b.gguf", '"def fibonacci(n):"', "/usr/local/lib/machine"
for name, extra in (("CPU alone (bench step b)", ""), ("engine attached (bench step c)", " --zaccel 127.0.0.1")):
    print(f"== {name}")
    out = sh(f"{B}/run_model {M} {P} 2 --fast{extra} > /tmp/rm.out 2>&1 & p=$!; echo PEAK_ANON_KB $(peak $p); "
             f"grep -E 'zaccel|engine|decode|per decoded|rows' /tmp/rm.out | tail -4", 7200)
    print(out)
print(sh("free -m | head -2; grep -E 'MemAvailable' /proc/meminfo"))
