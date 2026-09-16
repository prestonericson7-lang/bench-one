#!/usr/bin/env python3
# ============================================================
#  packet_hub.py  --  Luckfox Pico A companion for the LoRa handheld
#
#  Gives the idle Teensy<->Luckfox UART a purpose:
#    * reads the line protocol the Teensy already streams (PKT / STS / SWP / ACK / ERR)
#    * logs every received packet to the microSD card as CSV
#    * serves a live green-on-black web console over the USB-gadget ethernet,
#      showing status + live packets + a sweep view, and sending commands back
#      to the Teensy (mode / freq / preset / power / sync / send / stop).
#
#  Stdlib only -- the stock Luckfox image has python3 but no pyserial.
#  The serial glue (termios) is imported lazily so the parser / store / web layer
#  can be unit-tested on any machine.  See firmware/luckfox/README.md to deploy.
# ============================================================
import os, sys, json, time, threading, queue
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# ---------------- config (env overrides) ----------------
DEV     = os.environ.get("HUB_DEV",  "/dev/ttyS3")     # Teensy is on the Luckfox UART3 (pins per WIRING.md)
BAUD    = int(os.environ.get("HUB_BAUD", "921600"))    # matches LINK_BAUD in config.h (bench-one proven-exact rate)
PORT    = int(os.environ.get("HUB_PORT", "8080"))
LOGDIR  = os.environ.get("HUB_LOGDIR", "/mnt/sdcard/lora")
SDDEV   = os.environ.get("HUB_SDDEV", "/dev/mmcblk1p1")
SDMOUNT = os.environ.get("HUB_SDMOUNT", "/mnt/sdcard")
MAXPKTS = 400   # ring buffer kept in RAM for the web view (the SD log keeps everything)
LEDPATH = os.environ.get("HUB_LED", "")   # /sys/class/leds/<name>; blank = auto-detect (heartbeat blink so you can see the board is on)

# ---------------- packet / telemetry parsing ----------------
def parse_line(line):
    """Return (kind, payload) or None. Pure; safe to unit-test."""
    line = line.strip()
    if not line:
        return None
    p = line.split()
    tag = p[0].upper()
    try:
        if tag == "PKT" and len(p) >= 5:
            return ("PKT", {"freq": float(p[1]), "rssi": float(p[2]), "snr": float(p[3]),
                            "len": int(p[4]), "hex": p[5] if len(p) > 5 else ""})
        if tag == "STS" and len(p) >= 6:
            return ("STS", {"mode": int(p[1]), "freq": float(p[2]), "sf": int(p[3]),
                            "bw": float(p[4]), "pwr": int(p[5])})
        if tag == "SWP" and len(p) >= 3:
            return ("SWP", {"freq": float(p[1]), "rssi": float(p[2])})
        if tag in ("ACK", "ERR"):
            return (tag, line[len(tag):].strip())
    except (ValueError, IndexError):
        return None
    return None

MODE_NAMES = ["MENU", "SCAN", "CAPTURE", "REPLAY", "JAM", "SWEEP", "FUZZ", "CONFIG"]

# ---------------- shared state + SD logging ----------------
class Store:
    def __init__(self, logdir):
        self.lock = threading.Lock()
        self.pkts = deque(maxlen=MAXPKTS)
        self.total = 0
        self.status = {}
        self.sweep = []          # list of [freq, rssi] for the last sweep pass
        self._sweep_build = []
        self.acks = deque(maxlen=20)
        self.t0 = time.time()
        self.logdir = logdir
        self.logpath = None
        self.logcount = 0
        self.warn = ""
        self._logf = None

    def open_log(self):
        try:
            if self.logdir.startswith(SDMOUNT) and not os.path.ismount(SDMOUNT):
                raise OSError("SD not mounted at %s; refusing to log to rootfs" % SDMOUNT)
            os.makedirs(self.logdir, exist_ok=True)
            self.logpath = os.path.join(self.logdir, "cap_%d.csv" % int(self.t0))
            self._logf = open(self.logpath, "a", buffering=1)  # line-buffered
            if self._logf.tell() == 0:
                self._logf.write("host_epoch,freq_mhz,rssi_dbm,snr_db,len,hex\n")
        except OSError as e:
            self.warn = "log disabled: %s" % e
            self._logf = None

    def add_packet(self, d):
        with self.lock:
            self.total += 1
            rec = dict(d); rec["t"] = time.time()
            self.pkts.append(rec)
            if self._logf:
                try:
                    self._logf.write("%.3f,%.3f,%.1f,%.1f,%d,%s\n" %
                                     (rec["t"], d["freq"], d["rssi"], d["snr"], d["len"], d.get("hex", "")))
                    self.logcount += 1
                except OSError as e:
                    self.warn = "log write failed: %s" % e

    def set_status(self, d):
        with self.lock:
            self.status = dict(d); self.status["t"] = time.time()

    def add_sweep(self, d):
        # a sweep pass restarts when the frequency steps back down
        with self.lock:
            if self._sweep_build and d["freq"] < self._sweep_build[-1][0]:
                self._sweep_build = []
            self._sweep_build.append([d["freq"], d["rssi"]])
            self.sweep = self._sweep_build   # show the pass as it builds, not one pass behind

    def add_ack(self, s):
        with self.lock:
            self.acks.append({"t": time.time(), "s": s})

    def snapshot(self):
        with self.lock:
            return {
                "uptime": int(time.time() - self.t0),
                "total": self.total,
                "status": self.status,
                "sweep": self.sweep,
                "packets": list(self.pkts)[-120:][::-1],   # newest first
                "acks": list(self.acks)[::-1],
                "log": {"path": self.logpath, "count": self.logcount},
                "warn": self.warn,
            }

# ---------------- onboard LED heartbeat (so you can tell the board is powered/running) ----------------
def start_heartbeat(store):
    base = LEDPATH
    try:
        if not base:
            leds = "/sys/class/leds"
            if os.path.isdir(leds):
                names = sorted(os.listdir(leds))
                pref = [n for n in names if any(k in n.lower() for k in ("work", "user", "green", "act", "led"))]
                pick = pref or names
                if pick:
                    base = os.path.join(leds, pick[0])
        if not base or not os.path.isdir(base):
            store.warn = (store.warn + " | " if store.warn else "") + "no LED found for heartbeat"
            return None
        trig = os.path.join(base, "trigger")
        bright = os.path.join(base, "brightness")
        # release any kernel trigger (this RV110x kernel has no 'heartbeat' trigger), then blink ourselves
        try:
            with open(trig, "w") as f:
                f.write("none")
        except OSError:
            pass
        def blink():
            on = False
            while True:
                on = not on
                try:
                    with open(bright, "w") as f:
                        f.write("1" if on else "0")
                except OSError:
                    return
                time.sleep(0.5)
        threading.Thread(target=blink, daemon=True).start()
        return base
    except Exception as e:
        store.warn = (store.warn + " | " if store.warn else "") + ("LED heartbeat failed: %s" % e)
        return None

# ---------------- serial (lazy termios; only runs on the board) ----------------
def open_serial(dev, baud):
    import termios
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    a = termios.tcgetattr(fd)            # [iflag, oflag, cflag, lflag, ispeed, ospeed, cc]
    bconst = getattr(termios, "B%d" % baud, None)
    if bconst is None:
        os.close(fd)
        raise ValueError("no termios baud constant B%d; set the port with stty first" % baud)
    a[0] = 0                                                   # iflag: raw, no flow control
    a[1] = 0                                                   # oflag: raw
    a[2] = (a[2] & ~termios.CSIZE) | termios.CS8 | termios.CREAD | termios.CLOCAL
    a[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
    a[3] = 0                                                   # lflag: non-canonical, no echo
    a[4] = bconst; a[5] = bconst
    a[6][termios.VMIN] = 0; a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd

def reader_thread(fd, store, cmdq):
    import select
    buf = b""
    while True:
        # send any queued commands to the Teensy
        try:
            while True:
                line = cmdq.get_nowait()
                try:
                    os.write(fd, (line.rstrip("\r\n") + "\n").encode())
                except OSError as e:
                    store.warn = "serial write failed: %s" % e
        except queue.Empty:
            pass
        r, _, _ = select.select([fd], [], [], 0.2)
        if fd not in r:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            continue
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            parsed = parse_line(raw.decode("ascii", "replace"))
            if not parsed:
                continue
            kind, payload = parsed
            if kind == "PKT":   store.add_packet(payload)
            elif kind == "STS": store.set_status(payload)
            elif kind == "SWP": store.add_sweep(payload)
            else:               store.add_ack("%s %s" % (kind, payload))
        if len(buf) > 4096:
            buf = b""   # runaway line without a newline: drop

# ---------------- web console ----------------
PAGE = """<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>LoRa Hub</title><style>
:root{color-scheme:dark}
body{background:#000;color:#20e020;font:14px ui-monospace,Consolas,monospace;margin:0;padding:10px}
h1{font-size:16px;color:#8fffa0;margin:0 0 8px}
.bar{background:#031a06;border:1px solid #0a3;padding:6px 8px;border-radius:5px;margin-bottom:8px}
.warn{color:#ff5a5a}
button,input,select{background:#041f08;color:#3f6;border:1px solid #0a4;border-radius:4px;padding:5px 8px;font:13px ui-monospace,monospace}
button:hover{background:#0a3;color:#000;cursor:pointer}
table{width:100%;border-collapse:collapse;font-size:12px}
th,td{text-align:left;padding:2px 6px;border-bottom:1px solid #062}
th{color:#8fffa0;position:sticky;top:0;background:#000}
.hex{color:#9fdf9f;word-break:break-all}
.controls>*{margin:2px}
#sweep{height:60px;display:flex;align-items:flex-end;gap:1px;border:1px solid #063;padding:2px}
#sweep i{flex:1;background:#0a5;display:block}
small{color:#0a6}
</style></head><body>
<h1>LoRa Handheld &mdash; Luckfox Hub</h1>
<div class=bar id=status>connecting...</div>
<div class="bar controls">
  <button onclick="send('STOP')">STOP</button>
  <button onclick="send('MODE 1')">SCAN</button>
  <button onclick="send('MODE 2')">CAPTURE</button>
  <button onclick="send('MODE 5')">SWEEP</button>
  <button onclick="send('MODE 6')">FUZZ</button>
  <label>freq <input id=freq size=6 value="915.0"></label><button onclick="send('FREQ '+v('freq'))">set</button>
  <label>preset <select id=preset><option value=0>MAXRANGE</option><option value=1>BALANCED</option><option value=2>FAST</option></select></label><button onclick="send('PRESET '+v('preset'))">set</button>
  <label>pwr <input id=pwr size=3 value="22"></label><button onclick="send('PWR '+v('pwr'))">set</button>
  <label>sync <input id=sync size=4 value="12"></label><button onclick="send('SYNC '+v('sync'))">set</button>
  <label>send <input id=tx size=14 placeholder="DEADBEEF"></label><button onclick="send('SEND '+v('tx'))">tx</button>
</div>
<div class=bar>sweep (last pass)<div id=sweep></div></div>
<div class=bar>packets <span id=cnt>0</span> &mdash; log <small id=log>-</small></div>
<table id=tbl><thead><tr><th>#</th><th>t</th><th>freq</th><th>rssi</th><th>snr</th><th>len</th><th>hex</th></tr></thead><tbody id=rows></tbody></table>
<script>
const MODES=["MENU","SCAN","CAPTURE","REPLAY","JAM","SWEEP","FUZZ","CONFIG"];
function v(id){return document.getElementById(id).value.trim();}
function send(c){fetch('/cmd?c='+encodeURIComponent(c)).catch(()=>{});}
async function tick(){
  try{
    const r=await fetch('/api'); const d=await r.json();
    const s=d.status||{};
    document.getElementById('status').innerHTML =
      'radio '+(s.mode!=null?('mode '+(MODES[s.mode]||s.mode)):'?')+
      ' &middot; '+(s.freq!=null?s.freq.toFixed(1)+'MHz':'?')+
      ' &middot; SF'+(s.sf??'?')+' BW'+(s.bw??'?')+' &middot; '+(s.pwr??'?')+'dBm'+
      ' &middot; up '+d.uptime+'s'+(d.warn?(' &middot; <span class=warn>'+d.warn+'</span>'):'');
    document.getElementById('cnt').textContent=d.total;
    document.getElementById('log').textContent=(d.log.path||'-')+' ('+d.log.count+')';
    const sw=document.getElementById('sweep'); sw.innerHTML='';
    (d.sweep||[]).forEach(p=>{const i=document.createElement('i');
      let h=Math.max(1,Math.min(100,(p[1]+128)/98*100));i.style.height=h+'%';
      i.title=p[0].toFixed(2)+'MHz '+p[1]+'dBm';sw.appendChild(i);});
    const rows=document.getElementById('rows'); rows.innerHTML='';
    (d.packets||[]).forEach((p,n)=>{const tr=document.createElement('tr');
      tr.innerHTML='<td>'+(d.total-n)+'</td><td>'+p.t.toFixed(1)+'</td><td>'+p.freq.toFixed(1)+
        '</td><td>'+p.rssi+'</td><td>'+p.snr+'</td><td>'+p.len+'</td><td class=hex>'+(p.hex||'')+'</td>';
      rows.appendChild(tr);});
  }catch(e){document.getElementById('status').innerHTML='<span class=warn>hub unreachable</span>';}
}
setInterval(tick,1000); tick();
</script></body></html>"""

def make_handler(store, cmdq):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a): pass   # quiet
        def _send(self, code, body, ctype="text/html"):
            b = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
        def do_GET(self):
            u = urlparse(self.path)
            if u.path == "/" or u.path == "/index.html":
                self._send(200, PAGE)
            elif u.path == "/api":
                self._send(200, json.dumps(store.snapshot()), "application/json")
            elif u.path == "/cmd":
                q = parse_qs(u.query)
                line = (q.get("c", [""])[0]).strip()
                if line:
                    cmdq.put(line); store.add_ack(">> " + line)
                    self._send(200, json.dumps({"ok": True, "sent": line}), "application/json")
                else:
                    self._send(400, json.dumps({"ok": False}), "application/json")
            else:
                self._send(404, "not found")
    return H

def try_mount_sd():
    if os.path.ismount(SDMOUNT):
        return
    if os.path.exists(SDDEV):
        os.makedirs(SDMOUNT, exist_ok=True)
        os.system("mount -t vfat %s %s 2>/dev/null" % (SDDEV, SDMOUNT))

def main():
    try_mount_sd()
    store = Store(LOGDIR)
    store.open_log()
    led = start_heartbeat(store)
    if led: print("heartbeat LED: %s" % led)
    cmdq = queue.Queue()
    fd = None
    try:
        fd = open_serial(DEV, BAUD)
        threading.Thread(target=reader_thread, args=(fd, store, cmdq), daemon=True).start()
        print("serial %s @ %d open" % (DEV, BAUD))
    except Exception as e:
        store.warn = "serial %s unavailable: %s" % (DEV, e)
        print(store.warn)
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), make_handler(store, cmdq))
    print("web console on http://<luckfox-ip>:%d  (log: %s)" % (PORT, store.logpath))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
